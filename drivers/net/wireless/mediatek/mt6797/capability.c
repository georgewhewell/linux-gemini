// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 GEN3 WLAN NIC capability exchange and bounded IRQ session. */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-consys-wifi.h>

#include "boot.h"
#include "capability.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "protocol.h"

#define MT6797_WIFI_WHIER_CAPABILITY	(MT6797_WIFI_WHISR_TX_DONE | \
					 MT6797_WIFI_WHISR_RX1_DONE | \
					 MT6797_WIFI_WHISR_ABNORMAL)

#define MT6797_WIFI_CMD_CAPABILITY	0x80U
#define MT6797_WIFI_EVENT_CAPABILITY	0x01U
#define MT6797_WIFI_CAPABILITY_SEQUENCE	1U
#define MT6797_WIFI_CAPABILITY_TIMEOUT_MS 1000U
#define MT6797_WIFI_CAPABILITY_MAX_IRQS	8U

static bool
mt6797_wifi_capability_ops_valid(const struct mt6797_wifi_capability_ops *ops)
{
	return ops && ops->precheck && ops->arm && ops->enable_linux_irq &&
		ops->enable_device_irq && ops->unmask_hif_irq && ops->send &&
		ops->wait && ops->disarm && ops->mask_hif_irq &&
		ops->disable_device_irq && ops->disable_linux_irq && ops->fence;
}

int
mt6797_wifi_capability_transaction(const struct mt6797_wifi_capability_ops *ops)
{
	bool device_touched = false;
	bool linux_enabled = false;
	bool command_dirty = false;
	bool armed = false;
	int cleanup = 0;
	int ret;
	int err;

	if (!mt6797_wifi_capability_ops_valid(ops))
		return -EINVAL;
	ret = ops->precheck(ops->context);
	if (ret)
		goto out_fence;
	ret = ops->arm(ops->context);
	if (ret)
		goto out_fence;
	armed = true;
	/* Close the wrapper gate before changing device or Linux IRQ state. */
	ops->mask_hif_irq(ops->context);
	device_touched = true;
	ret = ops->enable_device_irq(ops->context);
	if (ret)
		goto cleanup;
	ops->enable_linux_irq(ops->context);
	linux_enabled = true;
	ops->unmask_hif_irq(ops->context);

	/* The command write can leave firmware state changed without a response. */
	command_dirty = true;
	ret = ops->send(ops->context);
	if (!ret)
		ret = ops->wait(ops->context);

cleanup:
	if (armed)
		ops->disarm(ops->context);
	if (device_touched)
		ops->mask_hif_irq(ops->context);
	/* Synchronize a possible timeout-racing thread before device teardown. */
	if (linux_enabled)
		ops->disable_linux_irq(ops->context);
	if (device_touched) {
		err = ops->disable_device_irq(ops->context);
		if (err)
			cleanup = err;
	}
	if (ret) {
		if (ret == -EUCLEAN || command_dirty || cleanup)
			ops->fence(ops->context);
		return ret;
	}
	if (cleanup) {
		ops->fence(ops->context);
		return cleanup;
	}
	return 0;

out_fence:
	if (ret == -EUCLEAN || command_dirty || cleanup)
		ops->fence(ops->context);
	return ret;
}

int mt6797_wifi_capability_command_build(u8 *command, size_t length,
					 u8 sequence)
{
	if (!command || sequence != MT6797_WIFI_CAPABILITY_SEQUENCE)
		return -EINVAL;
	if (length != MT6797_WIFI_CAPABILITY_LENGTH)
		return -EMSGSIZE;

	return mt6797_wifi_command_init(command, length,
					MT6797_WIFI_CAPABILITY_LENGTH -
					MT6797_WIFI_COMMAND_HEADER_LENGTH,
					MT6797_WIFI_CMD_CAPABILITY, false,
					sequence);
}

int mt6797_wifi_capability_event_parse(const u8 *event, size_t length,
				       u8 sequence,
				       struct mt6797_wifi_capability *capability)
{
	struct mt6797_wifi_event_view view;
	const u8 *payload;
	int ret;

	if (!event || !capability)
		return -EINVAL;
	if (length != MT6797_WIFI_CAPABILITY_LENGTH)
		return -EMSGSIZE;
	ret = mt6797_wifi_event_parse(event, length, &view);
	if (ret)
		return ret;
	if (view.packet_length != MT6797_WIFI_CAPABILITY_LENGTH)
		return -EMSGSIZE;
	if (view.id != MT6797_WIFI_EVENT_CAPABILITY ||
	    view.sequence != sequence)
		return -EPROTO;
	payload = view.payload;

	memset(capability, 0, sizeof(*capability));
	capability->product_id = get_unaligned_le16(payload);
	capability->firmware_version = get_unaligned_le16(payload + 2);
	capability->driver_version = get_unaligned_le16(payload + 4);
	capability->hw_5ghz_disabled = payload[6];
	capability->eeprom_used = payload[7];
	memcpy(capability->mac_address, payload + 8,
	       sizeof(capability->mac_address));
	capability->rf_version = payload[16];
	capability->phy_version = payload[17];
	capability->rf_cal_failed = payload[18];
	capability->bb_cal_failed = payload[19];
	capability->feature_flag0 = get_unaligned_le32(payload + 36);
	capability->feature_flag1 = get_unaligned_le32(payload + 40);
	capability->compile_flag0 = get_unaligned_le32(payload + 44);
	capability->compile_flag1 = get_unaligned_le32(payload + 48);
	return 0;
}

static int
mt6797_wifi_capability_reg_read(struct mt6797_wifi_capability_session *irq,
				u32 address, u32 *value)
{
	__le32 data;
	int ret;

	ret = mt6797_wifi_ahb_cmd53_read(irq->owner->hif, address, &data,
					 sizeof(data));
	if (!ret)
		*value = le32_to_cpu(data);
	return ret;
}

static int
mt6797_wifi_capability_reg_write(struct mt6797_wifi_capability_session *irq,
				 u32 address, u32 value)
{
	__le32 data = cpu_to_le32(value);

	return mt6797_wifi_ahb_cmd53_write(irq->owner->hif, address, &data,
					  sizeof(data));
}

static int mt6797_wifi_capability_read_tx_completion(
	struct mt6797_wifi_capability_session *irq)
{
	unsigned int queue;
	int ret;

	for (queue = 0; queue < ARRAY_SIZE(irq->tx_released); queue++) {
		ret = mt6797_wifi_capability_reg_read(irq,
						      MT6797_WIFI_MCR_WTQCR0 + queue * sizeof(u32),
						      &irq->tx_released[queue]);
		if (ret)
			return ret;
	}
	return 0;
}

static void
mt6797_wifi_capability_complete(struct mt6797_wifi_capability_session *irq,
				int result)
{
	irq->result = result;
	irq->done = true;
	complete(&irq->completion);
}

static irqreturn_t mt6797_wifi_capability_irq_thread(void *data)
{
	struct mt6797_wifi_capability_session *irq = data;
	u16 length;
	int ret;

	if (!READ_ONCE(irq->armed) || irq->done)
		return IRQ_HANDLED;
	if (++irq->irq_count > MT6797_WIFI_CAPABILITY_MAX_IRQS) {
		mt6797_wifi_capability_complete(irq, -ELOOP);
		return IRQ_HANDLED;
	}
	ret = mt6797_wifi_capability_reg_read(irq, MT6797_WIFI_MCR_WHISR,
					      &irq->whisr);
	if (ret)
		goto complete;
	if (irq->whisr & (MT6797_WIFI_WHISR_ABNORMAL |
			  MT6797_WIFI_WHISR_D2H_SW)) {
		ret = -EIO;
		goto complete;
	}
	if (irq->whisr & MT6797_WIFI_WHISR_TX_DONE) {
		ret = mt6797_wifi_capability_read_tx_completion(irq);
		if (ret)
			goto complete;
	}
	if (!(irq->whisr & MT6797_WIFI_WHISR_RX1_DONE)) {
		if (READ_ONCE(irq->armed))
			mt6797_wifi_ahb_irq_unmask(irq->owner->hif);
		return IRQ_HANDLED;
	}
	ret = mt6797_wifi_capability_reg_read(irq, MT6797_WIFI_MCR_WRPLR,
					      &irq->wrplr);
	if (ret)
		goto complete;
	length = FIELD_GET(GENMASK(31, 16), irq->wrplr);
	if (length != MT6797_WIFI_CAPABILITY_LENGTH) {
		ret = -EMSGSIZE;
		goto complete;
	}
	ret = mt6797_wifi_ahb_cmd53_read(irq->owner->hif,
					 MT6797_WIFI_MCR_WRDR1,
					 irq->response, sizeof(irq->response));
	if (ret)
		goto complete;
	ret = mt6797_wifi_capability_event_parse(irq->response, length,
						 irq->sequence,
						 &irq->capability);
complete:
	mt6797_wifi_capability_complete(irq, ret);
	return IRQ_HANDLED;
}

int mt6797_wifi_capability_session_init(
	struct mt6797_wifi_capability_session *irq,
	struct mt6797_wifi_hif_irq *owner)
{
	if (!irq || !owner)
		return -EINVAL;
	memset(irq, 0, sizeof(*irq));
	irq->owner = owner;
	init_completion(&irq->completion);
	return 0;
}

static int mt6797_wifi_capability_precheck(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;
	int ret;

	if (mt6797_wifi_hif_irq_mode(irq->owner) !=
	    MT6797_WIFI_HIF_IRQ_QUIESCENT)
		return -EUCLEAN;
	ret = mt6797_wifi_capability_reg_read(irq, MT6797_WIFI_MCR_WRPLR,
					      &irq->wrplr);
	if (ret)
		return ret;
	return FIELD_GET(GENMASK(31, 16), irq->wrplr) ? -EUCLEAN : 0;
}

static int mt6797_wifi_capability_arm(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;
	int ret;

	reinit_completion(&irq->completion);
	memset(&irq->capability, 0, sizeof(irq->capability));
	memset(irq->response, 0, sizeof(irq->response));
	memset(irq->tx_released, 0, sizeof(irq->tx_released));
	irq->whisr = 0;
	irq->irq_count = 0;
	irq->result = -ETIMEDOUT;
	irq->sequence = MT6797_WIFI_CAPABILITY_SEQUENCE;
	irq->done = false;
	irq->whcr_changed = false;
	WRITE_ONCE(irq->armed, true);
	ret = mt6797_wifi_hif_irq_enter_capability(
		irq->owner, mt6797_wifi_capability_irq_thread, irq);
	if (ret)
		WRITE_ONCE(irq->armed, false);
	return ret;
}

static void mt6797_wifi_capability_enable_linux_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	enable_irq(irq->owner->irq);
}

static int mt6797_wifi_capability_enable_device_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;
	u32 whcr;
	int ret;

	ret = mt6797_wifi_capability_reg_read(irq, MT6797_WIFI_MCR_WHCR,
					      &irq->whcr_before);
	if (ret)
		return ret;
	if (irq->whcr_before & MT6797_WIFI_WHCR_INT_WRITE_1_CLEAR) {
		irq->whcr_changed = true;
		whcr = irq->whcr_before & ~MT6797_WIFI_WHCR_INT_WRITE_1_CLEAR;
		ret = mt6797_wifi_capability_reg_write(irq,
						       MT6797_WIFI_MCR_WHCR,
						       whcr);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_capability_reg_write(irq, MT6797_WIFI_MCR_WHIER,
					       MT6797_WIFI_WHIER_CAPABILITY);
	if (ret)
		return ret;
	/* WHISR is now read-to-clear; acknowledge any pre-command status. */
	ret = mt6797_wifi_capability_reg_read(irq, MT6797_WIFI_MCR_WHISR,
					      &irq->whisr);
	if (ret)
		return ret;
	if (irq->whisr & (MT6797_WIFI_WHISR_RX1_DONE |
			  MT6797_WIFI_WHISR_ABNORMAL |
			  MT6797_WIFI_WHISR_D2H_SW))
		return -EUCLEAN;
	if (irq->whisr & MT6797_WIFI_WHISR_TX_DONE) {
		ret = mt6797_wifi_capability_read_tx_completion(irq);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_ahb_irq_gate_update(irq->owner->hif,
					    &irq->owner->cccr_gate, true);
	if (ret)
		return ret;
	return mt6797_wifi_capability_reg_write(irq, MT6797_WIFI_MCR_WHLPCR,
						MT6797_WIFI_WHLPCR_INT_SET);
}

static void mt6797_wifi_capability_unmask_hif_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	mt6797_wifi_ahb_irq_unmask(irq->owner->hif);
}

static int mt6797_wifi_capability_send(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;
	u8 command[MT6797_WIFI_CAPABILITY_LENGTH];
	int ret;

	ret = mt6797_wifi_capability_command_build(command, sizeof(command),
						   irq->sequence);
	if (ret)
		return ret;
	return mt6797_wifi_ahb_cmd53_write(irq->owner->hif,
					  MT6797_WIFI_MCR_WTDR1,
					  command, sizeof(command));
}

static int mt6797_wifi_capability_wait(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	if (!wait_for_completion_timeout(&irq->completion,
					 msecs_to_jiffies(MT6797_WIFI_CAPABILITY_TIMEOUT_MS)))
		return -ETIMEDOUT;
	return irq->result;
}

static void mt6797_wifi_capability_disarm(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	WRITE_ONCE(irq->armed, false);
	mt6797_wifi_hif_irq_leave_capability(irq->owner);
}

static void mt6797_wifi_capability_mask_hif_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	mt6797_wifi_ahb_irq_mask(irq->owner->hif);
}

static int mt6797_wifi_capability_disable_device_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;
	int cleanup = 0;
	int ret;

	ret = mt6797_wifi_capability_reg_write(irq, MT6797_WIFI_MCR_WHLPCR,
					       MT6797_WIFI_WHLPCR_INT_CLR);
	if (ret)
		cleanup = ret;
	ret = mt6797_wifi_capability_reg_write(irq, MT6797_WIFI_MCR_WHIER,
					       MT6797_WIFI_WHIER_DEFAULT);
	if (!cleanup && ret)
		cleanup = ret;
	if (irq->whcr_changed) {
		ret = mt6797_wifi_capability_reg_write(irq, MT6797_WIFI_MCR_WHCR,
						       irq->whcr_before);
		if (!cleanup && ret)
			cleanup = ret;
	}
	ret = mt6797_wifi_ahb_irq_gate_update(irq->owner->hif,
					    &irq->owner->cccr_gate, false);
	if (!cleanup && ret)
		cleanup = ret;
	return cleanup;
}

static void mt6797_wifi_capability_disable_linux_irq(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	disable_irq(irq->owner->irq);
}

static void mt6797_wifi_capability_fence(void *context)
{
	struct mt6797_wifi_capability_session *irq = context;

	mt6797_consys_wifi_fence(irq->boot->lease);
}

int mt6797_wifi_capability_query(struct mt6797_wifi_capability_session *irq,
				 struct mt6797_wifi_boot *boot,
				 struct mt6797_wifi_capability *capability)
{
	struct mt6797_wifi_capability_ops ops = {
		.precheck = mt6797_wifi_capability_precheck,
		.arm = mt6797_wifi_capability_arm,
		.enable_linux_irq = mt6797_wifi_capability_enable_linux_irq,
		.enable_device_irq = mt6797_wifi_capability_enable_device_irq,
		.unmask_hif_irq = mt6797_wifi_capability_unmask_hif_irq,
		.send = mt6797_wifi_capability_send,
		.wait = mt6797_wifi_capability_wait,
		.disarm = mt6797_wifi_capability_disarm,
		.mask_hif_irq = mt6797_wifi_capability_mask_hif_irq,
		.disable_device_irq = mt6797_wifi_capability_disable_device_irq,
		.disable_linux_irq = mt6797_wifi_capability_disable_linux_irq,
		.fence = mt6797_wifi_capability_fence,
		.context = irq,
	};
	int ret;

	if (!irq || !boot || !capability || !irq->owner ||
	    irq->owner->hif != boot->hif ||
	    !boot->online || !boot->transaction.ready || !boot->lease)
		return -EINVAL;
	irq->boot = boot;
	ret = mt6797_wifi_capability_transaction(&ops);
	if (!ret)
		*capability = irq->capability;
	else
		/* The transaction has synchronized the IRQ; inspect saved state only. */
		dev_err(irq->owner->dev,
			"capability result=%d irq_count=%u done=%u irq_result=%d whisr=%#x wrplr=%#x whcr_before=%#x tx_released=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
			ret, irq->irq_count, irq->done, irq->result,
			irq->whisr, irq->wrplr, irq->whcr_before,
			irq->tx_released[0], irq->tx_released[1],
			irq->tx_released[2], irq->tx_released[3],
			irq->tx_released[4], irq->tx_released[5],
			irq->tx_released[6], irq->tx_released[7]);
	irq->boot = NULL;
	return ret;
}
