// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 connectivity subsystem ownership foundation.
 *
 * This driver is deliberately fail-closed.  Its disabled Device Tree node is
 * the sole owner of the vendor-evidenced EMI -> reset-held identity -> MBIST /
 * AFE / reset-release -> BTIF mandatory/full-STP chronology.  Shared EMI has
 * no inverse, so every failure after its one boot-time claim is terminal.
 */

#include <linux/bits.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/mt6351-regulator.h>
#include <linux/soc/mediatek/mt6797-consys-fw.h>
#include <linux/reset.h>
#include <linux/sizes.h>

#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-bt.h>
#include <linux/soc/mediatek/mt6797-consys-wifi.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-postpatch.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-patch.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#include "mt6797-consys-internal.h"

#define MT6797_CONSYS_MCU_CHIP_ID		0x8
#define MT6797_CONSYS_MCU_CFG_ACR		0x110
#define MT6797_CONSYS_MCU_CFG_ACR_MBIST		BIT(18)
#define MT6797_CONSYS_INFRACFG_SLEEP_MASK	0x350
#define MT6797_CONSYS_SLEEP_MASK_CONN		BIT(8)
#define MT6797_CONSYS_SPM_PWRON_CONFIG_EN	0x0
#define MT6797_CONSYS_SPM_PWRON_CONFIG_VALUE	0x0b160001
#define MT6797_CONSYS_CHIP_ID			0x0279
#define MT6797_CONSYS_BGF_SPI			284
#define MT6797_CONSYS_WDT_SPI			285
#define MT6797_CONSYS_CHIP_POLL_INTERVAL_US	20000
#define MT6797_CONSYS_CHIP_POLL_TIMEOUT_US	200000
#define MT6797_CONSYS_RESET_RELEASE_SETTLE_MS	20
#define MT6797_CONSYS_WMT_TIMEOUT_MS		3000
#define MT6797_CONSYS_VCN33_UV			3300000

struct mt6797_consys_afe_write {
	u8 offset;
	u32 value;
};

/* Vendor mtk_wcn_consys_hw.c:459-482, without its optional GPS setting. */
static const struct mt6797_consys_afe_write mt6797_consys_afe_writes[] = {
	{ 0x20, 0x180b0160 },
	{ 0x50, 0xb8925421 },
	{ 0x54, 0x00006401 },
	{ 0x60, 0xb8925421 },
	{ 0x64, 0x00006401 },
	{ 0x68, 0x08400000 },
	{ 0x6c, 0x0c110200 },
	{ 0x70, 0x012a020a },
	{ 0x84, 0x00006401 },
	{ 0x88, 0x08400000 },
	{ 0x90, 0x012a020a },
};

enum mt6797_consys_owner_state {
	MT6797_CONSYS_OWNER_OFF,
	MT6797_CONSYS_OWNER_FIRMWARE_READY,
	MT6797_CONSYS_OWNER_STARTING,
	MT6797_CONSYS_OWNER_ACTIVE,
	MT6797_CONSYS_OWNER_QUIESCED,
	MT6797_CONSYS_OWNER_ERROR,
};

struct mt6797_consys_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

struct mt6797_consys {
	struct device *dev;
	const struct resource *mcu;
	const struct resource *ap_rgu;
	const struct resource *topckgen;
	const struct resource *spm;
	const struct resource *afe;
	void __iomem *mcu_base;
	void __iomem *afe_base;
	struct regmap *infracfg;
	struct regmap *spm_regmap;
	struct resource shared_mem;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regulator *vcn33_wifi;
	struct reset_control *conn_mcu_reset;
	struct mt6797_consys_fw_bundle *firmware;
	struct device *btif_supplier;
	const struct mt6797_consys_btif_ops *btif_ops;
	void *btif_context;
	struct mt6797_consys_stp_session *stp_session;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	struct mt6797_consys_bt_lease *bt_lease;
	struct mt6797_consys_wifi_lease *wifi_lease;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease runtime_lease;
	struct mt6797_consys_postid_owner postid;
	struct mutex lifecycle_lock;
	enum mt6797_consys_owner_state state;
	u32 chip_id;
	bool vcn18_vote;
	bool vcn28_vote;
	bool conn_domain_vote;
	bool emi_committed;
	bool children_populated;
	int bgf_irq;
	int wdt_irq;
};

struct mt6797_consys_bt_lease {
	struct mt6797_consys *consys;
	mt6797_consys_bt_rx_cb_t receive;
	void *receive_context;
	bool active;
};

struct mt6797_consys_wifi_lease {
	struct mt6797_consys *consys;
	bool active;
};

static const u8 mt6797_consys_bt_on[] = { 0x00, 0x01 };
static const u8 mt6797_consys_bt_off[] = { 0x00, 0x00 };
static const u8 mt6797_consys_wmt_ok[] = { 0x00 };
static const struct mtk_wmt_match mt6797_consys_bt_reply = {
	.opcode = 0x06,
	.data = mt6797_consys_wmt_ok,
	.data_len = sizeof(mt6797_consys_wmt_ok),
};

static int
mt6797_consys_enable_rail(struct regulator *regulator, int microvolts,
			  bool hw_control)
{
	int ret;

	ret = mt6351_regulator_set_hw_control(regulator, hw_control);
	if (ret)
		return ret;
	ret = regulator_set_voltage(regulator, microvolts, microvolts);
	if (ret)
		goto restore_software_control;
	ret = regulator_enable(regulator);
	if (!ret)
		return 0;

restore_software_control:
	mt6351_regulator_set_hw_control(regulator, false);
	return ret;
}

static int mt6797_consys_disable_rail(struct regulator *regulator, bool *vote)
{
	int disable_ret;
	int ret;

	ret = mt6351_regulator_set_hw_control(regulator, false);
	if (!*vote)
		return ret;

	disable_ret = regulator_disable(regulator);
	if (!disable_ret)
		*vote = false;
	return ret ?: disable_ret;
}

static int mt6797_consys_enable_vcn18(void *context)
{
	struct mt6797_consys *consys = context;
	int ret;

	ret = mt6797_consys_enable_rail(consys->vcn18, 1800000, false);
	if (!ret)
		consys->vcn18_vote = true;
	return ret;
}

static int mt6797_consys_disable_vcn18(void *context)
{
	struct mt6797_consys *consys = context;

	return mt6797_consys_disable_rail(consys->vcn18, &consys->vcn18_vote);
}

static int mt6797_consys_enable_vcn28(void *context)
{
	struct mt6797_consys *consys = context;
	int ret;

	/* Gemini WMT_SOC.cfg selects co_clock_type == 0. */
	ret = mt6797_consys_enable_rail(consys->vcn28, 2800000, true);
	if (!ret)
		consys->vcn28_vote = true;
	return ret;
}

static int mt6797_consys_disable_vcn28(void *context)
{
	struct mt6797_consys *consys = context;

	return mt6797_consys_disable_rail(consys->vcn28, &consys->vcn28_vote);
}

static int mt6797_consys_configure_sleep_mask(void *context)
{
	struct mt6797_consys *consys = context;

	return regmap_update_bits(consys->infracfg,
				  MT6797_CONSYS_INFRACFG_SLEEP_MASK,
				  MT6797_CONSYS_SLEEP_MASK_CONN,
				  MT6797_CONSYS_SLEEP_MASK_CONN);
}

static int mt6797_consys_assert_reset(void *context)
{
	struct mt6797_consys *consys = context;

	return reset_control_assert(consys->conn_mcu_reset);
}

static int mt6797_consys_configure_pwron(void *context)
{
	struct mt6797_consys *consys = context;

	return regmap_write(consys->spm_regmap,
			    MT6797_CONSYS_SPM_PWRON_CONFIG_EN,
			    MT6797_CONSYS_SPM_PWRON_CONFIG_VALUE);
}

static int mt6797_consys_enable_domain(void *context)
{
	struct mt6797_consys *consys = context;
	int ret;

	ret = pm_runtime_resume_and_get(consys->dev);
	if (!ret)
		consys->conn_domain_vote = true;
	return ret;
}

static int mt6797_consys_disable_domain(void *context)
{
	struct mt6797_consys *consys = context;
	int ret;

	if (!consys->conn_domain_vote)
		return 0;

	ret = pm_runtime_put_sync_suspend(consys->dev);
	if (ret < 0) {
		/* A failed put consumed the usage count; retain the explicit vote. */
		pm_runtime_get_noresume(consys->dev);
		return ret;
	}
	consys->conn_domain_vote = false;
	return 0;
}

static int mt6797_consys_poll(void *context, enum mt6797_consys_poll poll)
{
	struct mt6797_consys *consys = context;

	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;
	consys->chip_id = readl(consys->mcu_base + MT6797_CONSYS_MCU_CHIP_ID);
	return consys->chip_id == MT6797_CONSYS_CHIP_ID ? 0 : -EAGAIN;
}

static void mt6797_consys_delay_us(void *context, u32 min, u32 max)
{
	usleep_range(min, max);
}

static const struct mt6797_consys_ops mt6797_consys_lifecycle_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_enable_vcn18,
	.disable_vcn18 = mt6797_consys_disable_vcn18,
	.enable_vcn28 = mt6797_consys_enable_vcn28,
	.disable_vcn28 = mt6797_consys_disable_vcn28,
	.configure_sleep_mask = mt6797_consys_configure_sleep_mask,
	.assert_reset = mt6797_consys_assert_reset,
	.configure_pwron = mt6797_consys_configure_pwron,
	.enable_scp_sys_conn = mt6797_consys_enable_domain,
	.disable_scp_sys_conn = mt6797_consys_disable_domain,
	.poll = mt6797_consys_poll,
	.delay_us = mt6797_consys_delay_us,
};

static int mt6797_consys_configure_mbist(void *context)
{
	struct mt6797_consys *consys = context;
	u32 value;

	value = readl(consys->mcu_base + MT6797_CONSYS_MCU_CFG_ACR);
	writel(value | MT6797_CONSYS_MCU_CFG_ACR_MBIST,
	       consys->mcu_base + MT6797_CONSYS_MCU_CFG_ACR);
	return readl(consys->mcu_base + MT6797_CONSYS_MCU_CFG_ACR) &
		MT6797_CONSYS_MCU_CFG_ACR_MBIST ? 0 : -EIO;
}

static int mt6797_consys_configure_afe(void *context)
{
	struct mt6797_consys *consys = context;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6797_consys_afe_writes); i++) {
		writel(mt6797_consys_afe_writes[i].value,
		       consys->afe_base + mt6797_consys_afe_writes[i].offset);
		if (readl(consys->afe_base + mt6797_consys_afe_writes[i].offset) !=
		    mt6797_consys_afe_writes[i].value)
			return -EIO;
	}
	return 0;
}

static int mt6797_consys_release_reset(void *context)
{
	struct mt6797_consys *consys = context;

	return reset_control_deassert(consys->conn_mcu_reset);
}

static const struct mt6797_consys_postid_ops mt6797_consys_postid_ops = {
	.abi = MT6797_CONSYS_POSTID_ABI,
	.configure_mbist = mt6797_consys_configure_mbist,
	.configure_afe = mt6797_consys_configure_afe,
	.release_reset = mt6797_consys_release_reset,
	.assert_reset = mt6797_consys_assert_reset,
};

static int mt6797_consys_stop_runtime(struct mt6797_consys *consys)
{
	int release_ret = 0;
	int ret = 0;

	if (mt6797_consys_postid_state(&consys->postid) !=
	    MT6797_CONSYS_POSTID_IDLE)
		ret = mt6797_consys_postid_stop(&consys->postid);
	if (mt6797_consys_runtime_lease_active(&consys->runtime_lease))
		release_ret =
			mt6797_consys_runtime_lease_release(&consys->runtime_lease);
	return ret ?: release_ret;
}

struct mt6797_consys_postpatch_context {
	struct mt6797_consys *consys;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
};

static struct regulator *mt6797_consys_vcn33(struct mt6797_consys *consys,
					      bool wifi)
{
	return wifi ? consys->vcn33_wifi : consys->vcn33_bt;
}

static int mt6797_consys_postpatch_get_voltage(void *context, bool wifi)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return regulator_get_voltage(mt6797_consys_vcn33(stage->consys, wifi));
}

static int mt6797_consys_postpatch_is_enabled(void *context, bool wifi)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return regulator_is_enabled(mt6797_consys_vcn33(stage->consys, wifi));
}

static int mt6797_consys_postpatch_set_voltage(void *context, bool wifi,
					       int microvolts)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return regulator_set_voltage(mt6797_consys_vcn33(stage->consys, wifi),
				     microvolts, microvolts);
}

static int mt6797_consys_postpatch_enable(void *context, bool wifi)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return regulator_enable(mt6797_consys_vcn33(stage->consys, wifi));
}

static int mt6797_consys_postpatch_disable(void *context, bool wifi)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return regulator_disable(mt6797_consys_vcn33(stage->consys, wifi));
}

static int mt6797_consys_postpatch_wmt(void *context, u8 opcode,
		const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction(stage->lease,
		opcode, data, data_len, expected,
		msecs_to_jiffies(MT6797_CONSYS_WMT_TIMEOUT_MS));
}

static int mt6797_consys_postpatch_wmt_capture(void *context, u8 opcode,
		const u8 *data, size_t data_len, u8 expected_opcode,
		size_t min_data_len, size_t max_data_len, u8 *reply,
		size_t reply_len, size_t *actual_data_len)
{
	struct mt6797_consys_postpatch_context *stage = context;

	return mt6797_btif_stp_bootstrap_full_wmt_transaction_capture_range(
		stage->lease, opcode, data, data_len, expected_opcode,
		min_data_len, max_data_len, reply, reply_len, actual_data_len,
		msecs_to_jiffies(MT6797_CONSYS_WMT_TIMEOUT_MS));
}

static void mt6797_consys_postpatch_delay(void *context,
					 unsigned int milliseconds)
{
	msleep(milliseconds);
}

static int mt6797_consys_bootstrap_stage(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_consys *consys = context;
	struct mt6797_consys_postpatch_context stage = {
		.consys = consys,
		.lease = lease,
	};
	const struct mt6797_consys_postpatch_ops ops = {
		.get_voltage = mt6797_consys_postpatch_get_voltage,
		.is_enabled = mt6797_consys_postpatch_is_enabled,
		.set_voltage = mt6797_consys_postpatch_set_voltage,
		.enable = mt6797_consys_postpatch_enable,
		.disable = mt6797_consys_postpatch_disable,
		.wmt = mt6797_consys_postpatch_wmt,
		.wmt_capture = mt6797_consys_postpatch_wmt_capture,
		.delay_ms = mt6797_consys_postpatch_delay,
		.context = &stage,
	};
	int ret;

	ret = mt6797_consys_wmt_patch_download(consys->firmware, lease);
	if (ret)
		return ret;
	return mt6797_consys_postpatch_run(&ops);
}

static int mt6797_consys_bt_session_receive(void *context, const u8 *payload,
					     size_t payload_length)
{
	struct mt6797_consys_bt_lease *lease = context;

	if (!READ_ONCE(lease->active))
		return -ESHUTDOWN;
	return lease->receive(lease->receive_context, payload, payload_length);
}

static int mt6797_consys_bt_wmt(struct mt6797_consys *consys, bool enable)
{
	const u8 *command = enable ? mt6797_consys_bt_on : mt6797_consys_bt_off;

	return consys->btif_ops->wmt_transaction(consys->btif_context,
		consys->stp_lease, 0x06, command, sizeof(mt6797_consys_bt_on),
		&mt6797_consys_bt_reply,
		msecs_to_jiffies(MT6797_CONSYS_WMT_TIMEOUT_MS));
}

static void mt6797_consys_radio_fence(struct mt6797_consys *consys)
{
	consys->state = MT6797_CONSYS_OWNER_ERROR;
	mt6797_consys_stp_session_quiesce(consys->stp_session);
	mt6797_consys_emi_mpu_latch_fatal();
}

const struct mt6797_consys_bt_lease *
mt6797_consys_bt_acquire(struct device *consumer,
			 mt6797_consys_bt_rx_cb_t receive,
			 void *receive_context)
{
	struct mt6797_consys_bt_lease *lease;
	struct mt6797_consys *consys;
	bool rail_dirty = false;
	bool function_dirty = false;
	bool terminal = false;
	int cleanup_ret = 0;
	int ret;

	if (!consumer || !consumer->parent || !receive)
		return ERR_PTR(-EINVAL);
	consys = dev_get_drvdata(consumer->parent);
	if (!consys)
		return ERR_PTR(-EPROBE_DEFER);
	lease = kzalloc_obj(*lease, GFP_KERNEL);
	if (!lease)
		return ERR_PTR(-ENOMEM);
	lease->consys = consys;
	lease->receive = receive;
	lease->receive_context = receive_context;

	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_ACTIVE || !consys->stp_lease ||
	    !consys->btif_ops || !consys->btif_ops->wmt_transaction) {
		ret = -ESHUTDOWN;
		goto unlock_free;
	}
	if (consys->bt_lease) {
		ret = -EBUSY;
		goto unlock_free;
	}
	ret = mt6797_consys_stp_session_bind_bt(consys->stp_lease,
		mt6797_consys_bt_session_receive, lease);
	if (ret)
		goto unlock_free;
	ret = regulator_set_voltage(consys->vcn33_bt, MT6797_CONSYS_VCN33_UV,
				    MT6797_CONSYS_VCN33_UV);
	if (ret)
		goto unbind;
	rail_dirty = true;
	ret = regulator_enable(consys->vcn33_bt);
	if (ret) {
		int enabled = regulator_is_enabled(consys->vcn33_bt);

		if (enabled <= 0)
			rail_dirty = false;
		goto unbind;
	}
	function_dirty = true;
	ret = mt6797_consys_bt_wmt(consys, true);
	if (ret) {
		terminal = true;
		goto unbind;
	}
	lease->active = true;
	consys->bt_lease = lease;
	mutex_unlock(&consys->lifecycle_lock);
	return lease;

unbind:
	mt6797_consys_stp_session_unbind_bt(consys->stp_lease,
		mt6797_consys_bt_session_receive, lease);
	if (function_dirty) {
		cleanup_ret = mt6797_consys_bt_wmt(consys, false);
		if (cleanup_ret)
			terminal = true;
	}
	if (rail_dirty) {
		int rail_ret = regulator_disable(consys->vcn33_bt);

		if (!cleanup_ret)
			cleanup_ret = rail_ret;
		if (rail_ret)
			terminal = true;
	}
	if (terminal)
		mt6797_consys_radio_fence(consys);
unlock_free:
	mutex_unlock(&consys->lifecycle_lock);
	if (terminal)
		consys->btif_ops->stop(consys->btif_context);
	kfree(lease);
	return ERR_PTR(ret ?: cleanup_ret);
}
EXPORT_SYMBOL_GPL(mt6797_consys_bt_acquire);

int mt6797_consys_bt_transmit(const struct mt6797_consys_bt_lease *lease,
			     const u8 *frame, size_t frame_length)
{
	struct mt6797_consys *consys;
	bool terminal = false;
	int ret;

	if (!lease || !frame || !frame_length)
		return -EINVAL;
	consys = lease->consys;
	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_ACTIVE ||
	    consys->bt_lease != lease || !lease->active)
		ret = -ESHUTDOWN;
	else {
		ret = mt6797_consys_stp_session_transmit(consys->stp_lease,
						  MTK_STP_BT, frame,
						  frame_length);
		if (!ret)
			ret = consys->btif_ops->arm_session_timeout(
				consys->btif_context, consys->stp_lease);
		if (ret && ret != -EAGAIN) {
			mt6797_consys_radio_fence(consys);
			terminal = true;
		}
	}
	mutex_unlock(&consys->lifecycle_lock);
	if (terminal)
		consys->btif_ops->stop(consys->btif_context);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_bt_transmit);

int mt6797_consys_bt_release(const struct mt6797_consys_bt_lease *lease_arg)
{
	struct mt6797_consys_bt_lease *lease =
		(struct mt6797_consys_bt_lease *)lease_arg;
	struct mt6797_consys *consys;
	bool owned = false;
	bool terminal = false;
	int rail_ret;
	int ret;

	if (!lease || !lease->consys)
		return -EINVAL;
	consys = lease->consys;
	mutex_lock(&consys->lifecycle_lock);
	if (consys->bt_lease != lease) {
		ret = -ENOENT;
		goto unlock;
	}
	owned = true;
	WRITE_ONCE(lease->active, false);
	ret = mt6797_consys_stp_session_unbind_bt(consys->stp_lease,
		mt6797_consys_bt_session_receive, lease);
	if (ret)
		terminal = true;
	if (!ret)
		ret = mt6797_consys_bt_wmt(consys, false);
	if (ret)
		terminal = true;
	rail_ret = regulator_disable(consys->vcn33_bt);
	if (!ret)
		ret = rail_ret;
	if (rail_ret)
		terminal = true;
	consys->bt_lease = NULL;
	if (terminal)
		mt6797_consys_radio_fence(consys);
unlock:
	mutex_unlock(&consys->lifecycle_lock);
	if (terminal)
		consys->btif_ops->stop(consys->btif_context);
	if (owned)
		kfree(lease);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_bt_release);

const struct mt6797_consys_wifi_lease *
mt6797_consys_wifi_acquire(struct device *consumer)
{
	struct mt6797_consys_wifi_lease *lease;
	struct mt6797_consys *consys;
	bool rail_dirty = false;
	bool terminal = false;
	int cleanup_ret = 0;
	int ret;

	if (!consumer || !consumer->parent)
		return ERR_PTR(-EINVAL);
	consys = dev_get_drvdata(consumer->parent);
	if (!consys)
		return ERR_PTR(-EPROBE_DEFER);
	lease = kzalloc_obj(*lease, GFP_KERNEL);
	if (!lease)
		return ERR_PTR(-ENOMEM);
	lease->consys = consys;

	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_ACTIVE || !consys->stp_lease) {
		ret = -ESHUTDOWN;
		goto unlock_free;
	}
	if (consys->wifi_lease) {
		ret = -EBUSY;
		goto unlock_free;
	}
	ret = regulator_set_voltage(consys->vcn33_wifi,
				    MT6797_CONSYS_VCN33_UV,
				    MT6797_CONSYS_VCN33_UV);
	if (ret)
		goto unlock_free;
	rail_dirty = true;
	ret = regulator_enable(consys->vcn33_wifi);
	if (ret) {
		int enabled = regulator_is_enabled(consys->vcn33_wifi);

		if (!enabled)
			rail_dirty = false;
		else if (enabled < 0) {
			rail_dirty = false;
			cleanup_ret = enabled;
			terminal = true;
		}
		goto disable_rail;
	}
	lease->active = true;
	consys->wifi_lease = lease;
	mutex_unlock(&consys->lifecycle_lock);
	return lease;

disable_rail:
	if (rail_dirty)
		cleanup_ret = regulator_disable(consys->vcn33_wifi);
	if (cleanup_ret) {
		mt6797_consys_radio_fence(consys);
		terminal = true;
	}
unlock_free:
	mutex_unlock(&consys->lifecycle_lock);
	if (terminal)
		consys->btif_ops->stop(consys->btif_context);
	kfree(lease);
	return ERR_PTR(ret ?: cleanup_ret);
}
EXPORT_SYMBOL_GPL(mt6797_consys_wifi_acquire);

int mt6797_consys_wifi_release(const struct mt6797_consys_wifi_lease *lease_arg)
{
	struct mt6797_consys_wifi_lease *lease =
		(struct mt6797_consys_wifi_lease *)lease_arg;
	struct mt6797_consys *consys;
	bool terminal = false;
	int ret;

	if (!lease || !lease->consys)
		return -EINVAL;
	consys = lease->consys;
	mutex_lock(&consys->lifecycle_lock);
	if (consys->wifi_lease != lease) {
		ret = -ENOENT;
		goto unlock;
	}
	WRITE_ONCE(lease->active, false);
	ret = regulator_disable(consys->vcn33_wifi);
	consys->wifi_lease = NULL;
	if (ret) {
		mt6797_consys_radio_fence(consys);
		terminal = true;
	}
unlock:
	mutex_unlock(&consys->lifecycle_lock);
	if (terminal)
		consys->btif_ops->stop(consys->btif_context);
	if (ret != -ENOENT)
		kfree(lease);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_wifi_release);

static int mt6797_consys_start(struct mt6797_consys *consys)
{
	const struct mt6797_consys_stp_session_lease *stp_lease;
	const struct mt6797_consys_btif_ops *ops;
	void *context;
	int cleanup_ret;
	int ret;

	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_FIRMWARE_READY ||
	    !consys->btif_ops || !consys->stp_session) {
		ret = -EAGAIN;
		goto unlock;
	}
	consys->state = MT6797_CONSYS_OWNER_STARTING;
	ops = consys->btif_ops;
	context = consys->btif_context;
	mutex_unlock(&consys->lifecycle_lock);

	/* Vendor hw_init() performs this irreversible transaction first. */
	ret = mt6797_consys_emi_mpu_claim(consys->dev);
	if (ret)
		goto restore_ready;
	consys->emi_committed = true;

	ret = mt6797_consys_runtime_lease_acquire(&consys->lifecycle,
						  &consys->runtime_lease);
	if (ret)
		goto fail_terminal;
	ret = mt6797_consys_postid_start(&consys->postid,
					 &consys->runtime_lease, true);
	if (ret)
		goto fail_terminal;
	/* Vendor power-on waits this long after releasing CONMCU reset. */
	msleep(MT6797_CONSYS_RESET_RELEASE_SETTLE_MS);
	ret = ops->start(context, &consys->runtime_lease,
			 mt6797_consys_bootstrap_stage, consys,
			 &stp_lease);
	if (ret)
		goto fail_terminal;
	if (!mt6797_consys_stp_session_owns_lease(consys->stp_session, stp_lease)) {
		ret = -EXDEV;
		ops->stop(context);
		goto fail_terminal;
	}

	mutex_lock(&consys->lifecycle_lock);
	consys->stp_lease = stp_lease;
	consys->state = MT6797_CONSYS_OWNER_ACTIVE;
	mutex_unlock(&consys->lifecycle_lock);
	return 0;

fail_terminal:
	/* The supplier self-unwinds failed starts; stop() is idempotent. */
	ops->stop(context);
	cleanup_ret = mt6797_consys_stop_runtime(consys);
	if (consys->emi_committed)
		mt6797_consys_emi_mpu_latch_fatal();
	mutex_lock(&consys->lifecycle_lock);
	consys->state = MT6797_CONSYS_OWNER_ERROR;
	mutex_unlock(&consys->lifecycle_lock);
	if (cleanup_ret)
		dev_err(consys->dev,
			"terminal startup cleanup failed (%d); reboot required\n",
			cleanup_ret);
	return ret;

restore_ready:
	mutex_lock(&consys->lifecycle_lock);
	consys->state = MT6797_CONSYS_OWNER_FIRMWARE_READY;
	mutex_unlock(&consys->lifecycle_lock);
	return ret;

unlock:
	mutex_unlock(&consys->lifecycle_lock);
	return ret;
}

static void mt6797_consys_stop(struct mt6797_consys *consys)
{
	int ret;

	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_ACTIVE) {
		mutex_unlock(&consys->lifecycle_lock);
		return;
	}
	consys->stp_lease = NULL;
	mutex_unlock(&consys->lifecycle_lock);

	/* The component has already synchronously stopped BTIF and its IRQ. */
	ret = mt6797_consys_stop_runtime(consys);
	if (ret && consys->emi_committed)
		mt6797_consys_emi_mpu_latch_fatal();

	mutex_lock(&consys->lifecycle_lock);
	consys->state = ret ? MT6797_CONSYS_OWNER_ERROR :
		MT6797_CONSYS_OWNER_QUIESCED;
	mutex_unlock(&consys->lifecycle_lock);
	if (ret)
		dev_err(consys->dev,
			"CONSYS teardown failed (%d); WDT recovery is unsupported, reboot required\n",
			ret);
}

static int mt6797_consys_component_bind(struct device *dev)
{
	struct mt6797_consys *consys = dev_get_drvdata(dev);
	int ret;

	ret = component_bind_all(dev, consys);
	if (ret)
		return ret;

	mutex_lock(&consys->lifecycle_lock);
	ret = consys->btif_supplier && consys->stp_session ? 0 : -ENODEV;
	mutex_unlock(&consys->lifecycle_lock);
	if (ret) {
		component_unbind_all(dev, consys);
		return ret;
	}

	ret = mt6797_consys_start(consys);
	if (ret) {
		component_unbind_all(dev, consys);
		return dev_err_probe(dev, ret,
				     "failed terminal CONSYS/STP bring-up\n");
	}
	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		component_unbind_all(dev, consys);
		mt6797_consys_stop(consys);
		return dev_err_probe(dev, ret,
				     "failed to populate radio consumers\n");
	}
	consys->children_populated = true;

	dev_info(dev,
		 "CONSYS powered; chip %#x and firmware patched; radios disabled\n",
		 consys->chip_id);
	return 0;
}

static void mt6797_consys_component_unbind(struct device *dev)
{
	const struct mt6797_consys_btif_ops *ops = NULL;
	struct mt6797_consys_stp_session *session = NULL;
	struct mt6797_consys *consys = dev_get_drvdata(dev);
	void *context = NULL;

	if (consys->children_populated) {
		of_platform_depopulate(dev);
		consys->children_populated = false;
	}
	component_unbind_all(dev, consys);

	/* A component must detach, but keep aggregate teardown fail-safe. */
	mutex_lock(&consys->lifecycle_lock);
	if (consys->stp_session) {
		session = consys->stp_session;
		ops = consys->btif_ops;
		context = consys->btif_context;
		consys->stp_session = NULL;
		consys->btif_supplier = NULL;
		consys->btif_ops = NULL;
		consys->btif_context = NULL;
	}
	mutex_unlock(&consys->lifecycle_lock);
	if (ops)
		ops->stop(context);
	if (ops)
		ops->unbind_session(context, session);
	mt6797_consys_stp_session_destroy(session);
	mt6797_consys_stop(consys);
}

static const struct component_master_ops mt6797_consys_component_ops = {
	.bind = mt6797_consys_component_bind,
	.unbind = mt6797_consys_component_unbind,
};

static void mt6797_consys_component_del(void *data)
{
	component_master_del(data, &mt6797_consys_component_ops);
}

static int mt6797_consys_stp_receive(void *context,
				     enum mtk_stp_channel channel,
				     const u8 *payload, size_t payload_length)
{
	(void)context;
	(void)channel;
	(void)payload;
	(void)payload_length;

	/*
	 * Full-STP delivery is deliberately closed until the WMT/GNSS endpoint
	 * registrations have their own reviewed lifetimes. Returning an error
	 * prevents the session from acknowledging data which nobody consumed.
	 */
	return -ENOTCONN;
}

int mt6797_consys_btif_attach(struct device *master, struct device *supplier,
			      const struct mt6797_consys_btif_ops *ops,
			      void *context)
{
	struct mt6797_consys_stp_session_ops session_ops;
	struct mt6797_consys_stp_session *session;
	struct mt6797_consys *consys;
	int ret = 0;

	if (!master || !supplier || !ops || !ops->transmit ||
	    !ops->bind_session || !ops->unbind_session || !ops->start ||
	    !ops->wmt_transaction || !ops->arm_session_timeout || !ops->stop)
		return -EINVAL;

	consys = dev_get_drvdata(master);
	if (!consys)
		return -ENODEV;

	session_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = ops->transmit,
		.receive = mt6797_consys_stp_receive,
		.transmit_context = context,
		.receive_context = consys,
	};
	session = mt6797_consys_stp_session_create(&session_ops);
	if (IS_ERR(session))
		return PTR_ERR(session);

	mutex_lock(&consys->lifecycle_lock);
	if (consys->state != MT6797_CONSYS_OWNER_FIRMWARE_READY) {
		ret = -EAGAIN;
	} else if (consys->btif_supplier) {
		ret = -EBUSY;
	} else {
		ret = ops->bind_session(context, session);
		if (!ret) {
			consys->btif_supplier = supplier;
			consys->btif_ops = ops;
			consys->btif_context = context;
			consys->stp_session = session;
		}
	}
	mutex_unlock(&consys->lifecycle_lock);

	if (ret)
		mt6797_consys_stp_session_destroy(session);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_btif_attach);

void mt6797_consys_btif_detach(struct device *master, struct device *supplier)
{
	const struct mt6797_consys_btif_ops *ops = NULL;
	struct mt6797_consys_stp_session *session = NULL;
	struct mt6797_consys *consys;
	void *context = NULL;

	if (!master || !supplier)
		return;
	consys = dev_get_drvdata(master);
	if (!consys)
		return;

	mutex_lock(&consys->lifecycle_lock);
	if (consys->btif_supplier == supplier) {
		session = consys->stp_session;
		ops = consys->btif_ops;
		context = consys->btif_context;
		consys->stp_session = NULL;
		consys->btif_supplier = NULL;
		consys->btif_ops = NULL;
		consys->btif_context = NULL;
	}
	mutex_unlock(&consys->lifecycle_lock);

	if (ops) {
		ops->stop(context);
		ops->unbind_session(context, session);
	}
	mt6797_consys_stp_session_destroy(session);
}
EXPORT_SYMBOL_GPL(mt6797_consys_btif_detach);

static const struct mt6797_consys_resource mt6797_consys_resources[] = {
	{
		.name = "mcu",
		.start = 0x18070000,
		.size = SZ_512,
	}, {
		.name = "ap-rgu",
		.start = 0x10007000,
		.size = SZ_256,
	}, {
		.name = "topckgen",
		.start = 0x10000000,
		.size = SZ_8K,
	}, {
		.name = "spm",
		.start = 0x10006000,
		.size = SZ_4K,
	}, {
		.name = "afe",
		.start = 0x180b6000,
		.size = SZ_256,
	},
};

static_assert(ARRAY_SIZE(mt6797_consys_resources) == 5);

static int mt6797_consys_get_resource(struct platform_device *pdev,
				      const struct mt6797_consys_resource *expected,
				      const struct resource **resource)
{
	const struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing %s resource\n", expected->name);

	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid %s resource\n", expected->name);

	*resource = res;
	return 0;
}

static bool mt6797_consys_irq_spec_valid(const struct of_phandle_args *spec,
					 unsigned int spi)
{
	return spec && spec->args_count == 3 && spec->args[0] == GIC_SPI &&
		spec->args[1] == spi && spec->args[2] == IRQ_TYPE_LEVEL_LOW;
}

static int
mt6797_consys_validate_of_irq(struct device_node *node, int index,
			      unsigned int spi)
{
	struct of_phandle_args spec;
	int ret;

	ret = of_irq_parse_one(node, index, &spec);
	if (ret)
		return ret;
	ret = mt6797_consys_irq_spec_valid(&spec, spi) ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
}

static int
mt6797_consys_validate_irqs(struct platform_device *pdev,
			    struct mt6797_consys *consys)
{
	struct device_node *node = pdev->dev.of_node;
	int count;
	int ret;

	count = platform_irq_count(pdev);
	if (count != 2)
		return dev_err_probe(&pdev->dev, count < 0 ? count : -EINVAL,
				     "CONSYS needs exactly BGF and WDT IRQs\n");
	ret = mt6797_consys_validate_of_irq(node, 0, MT6797_CONSYS_BGF_SPI);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid BGF IRQ specification\n");
	ret = mt6797_consys_validate_of_irq(node, 1, MT6797_CONSYS_WDT_SPI);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid WDT IRQ specification\n");

	consys->bgf_irq = platform_get_irq_byname(pdev, "bgf");
	if (consys->bgf_irq < 0)
		return consys->bgf_irq;
	consys->wdt_irq = platform_get_irq_byname(pdev, "wdt");
	if (consys->wdt_irq < 0)
		return consys->wdt_irq;
	if ((irq_get_trigger_type(consys->bgf_irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW ||
	    (irq_get_trigger_type(consys->wdt_irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "CONSYS IRQs must resolve level-low\n");

	/* Neither line is requested until its recovery/consumer ABI is proven. */
	return 0;
}

static void mt6797_consys_disable_runtime_pm(void *data)
{
	pm_runtime_disable(data);
}

static int mt6797_consys_get_supply(struct device *dev, const char *name,
				    struct regulator **supply)
{
	*supply = devm_regulator_get(dev, name);
	if (IS_ERR(*supply)) {
		return dev_err_probe(dev, PTR_ERR(*supply),
				     "failed to acquire %s supply\n", name);
	}

	return 0;
}

static int
mt6797_consys_validate_shared_mem(struct device *dev,
				  struct resource *shared_mem)
{
	struct device_node *memory_region;
	int ret;

	memory_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!memory_region)
		return dev_err_probe(dev, -EINVAL, "missing shared memory region\n");

	if (!of_property_read_bool(memory_region, "no-map")) {
		of_node_put(memory_region);
		return dev_err_probe(dev, -EINVAL,
				     "shared memory region must be no-map\n");
	}

	of_node_put(memory_region);

	ret = of_reserved_mem_region_to_resource_byname(dev->of_node, "shared", shared_mem);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire shared memory\n");

	if (!mt6797_consys_shared_mem_valid(shared_mem->start,
					 resource_size(shared_mem)))
		return dev_err_probe(dev, -EINVAL,
				     "invalid shared memory geometry\n");

	return 0;
}

static void mt6797_consys_release_firmware(void *data)
{
	struct mt6797_consys *consys = data;
	struct mt6797_consys_fw_bundle *firmware;

	mutex_lock(&consys->lifecycle_lock);
	firmware = consys->firmware;
	consys->firmware = NULL;
	consys->state = MT6797_CONSYS_OWNER_OFF;
	mutex_unlock(&consys->lifecycle_lock);

	mt6797_consys_fw_release(firmware);
}

static int mt6797_consys_probe(struct platform_device *pdev)
{
	const struct mt6797_consys_timeouts timeouts = {
		.poll_interval_us = MT6797_CONSYS_CHIP_POLL_INTERVAL_US,
		.chip_ready_timeout_us = MT6797_CONSYS_CHIP_POLL_TIMEOUT_US,
	};
	const struct mt6797_consys_ops *lifecycle_ops =
		&mt6797_consys_lifecycle_ops;
	const struct mt6797_consys_postid_ops *postid_ops =
		&mt6797_consys_postid_ops;
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct mt6797_consys *consys;
	struct mt6797_consys_lifecycle *lifecycle;
	struct device_node *btif;
	struct device_node *emi_mpu;
	int ret;

	consys = devm_kzalloc(dev, sizeof(*consys), GFP_KERNEL);
	if (!consys)
		return -ENOMEM;
	consys->dev = dev;
	lifecycle = &consys->lifecycle;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[0],
					 &consys->mcu);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[1],
					 &consys->ap_rgu);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[2],
					 &consys->topckgen);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[3],
					 &consys->spm);
	if (ret)
		return ret;
	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[4],
					 &consys->afe);
	if (ret)
		return ret;

	consys->mcu_base = devm_ioremap_resource(dev, consys->mcu);
	if (IS_ERR(consys->mcu_base))
		return PTR_ERR(consys->mcu_base);
	consys->afe_base = devm_ioremap_resource(dev, consys->afe);
	if (IS_ERR(consys->afe_base))
		return PTR_ERR(consys->afe_base);

	ret = mt6797_consys_validate_shared_mem(dev, &consys->shared_mem);
	if (ret)
		return ret;
	consys->infracfg =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(consys->infracfg))
		return dev_err_probe(dev, PTR_ERR(consys->infracfg),
				     "failed to acquire INFRACFG regmap\n");
	consys->spm_regmap =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(consys->spm_regmap))
		return dev_err_probe(dev, PTR_ERR(consys->spm_regmap),
				     "failed to acquire SPM regmap\n");

	ret = mt6797_consys_get_supply(dev, "vcn18", &consys->vcn18);
	if (ret)
		return ret;

	ret = mt6797_consys_get_supply(dev, "vcn28", &consys->vcn28);
	if (ret)
		return ret;
	ret = mt6797_consys_get_supply(dev, "vcn33-bt", &consys->vcn33_bt);
	if (ret)
		return ret;
	ret = mt6797_consys_get_supply(dev, "vcn33-wifi",
				       &consys->vcn33_wifi);
	if (ret)
		return ret;

	consys->conn_mcu_reset =
		devm_reset_control_get_exclusive(dev, "conn-mcu");
	if (IS_ERR(consys->conn_mcu_reset))
		return dev_err_probe(dev, PTR_ERR(consys->conn_mcu_reset),
				     "failed to acquire CONN MCU reset\n");

	ret = mt6797_consys_validate_irqs(pdev, consys);
	if (ret)
		return ret;

	mutex_init(&consys->lifecycle_lock);
	consys->state = MT6797_CONSYS_OWNER_OFF;
	ret = mt6797_consys_lifecycle_init(lifecycle, lifecycle_ops, consys, &timeouts);
	if (ret)
		return ret;
	ret = mt6797_consys_postid_init(&consys->postid, lifecycle, postid_ops, consys);
	if (ret)
		return ret;

	/* The device core already attached the standard CONN power-domain link. */
	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev, mt6797_consys_disable_runtime_pm, dev);
	if (ret)
		return ret;

	ret = mt6797_consys_fw_request(dev, &consys->firmware);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to acquire verified firmware\n");
	consys->state = MT6797_CONSYS_OWNER_FIRMWARE_READY;

	ret = devm_add_action_or_reset(dev, mt6797_consys_release_firmware,
				       consys);
	if (ret)
		return ret;

	btif = of_parse_phandle(dev->of_node, "mediatek,btif", 0);
	if (!btif)
		return dev_err_probe(dev, -EINVAL, "missing BTIF supplier\n");
	emi_mpu = of_parse_phandle(dev->of_node, "mediatek,emi-mpu", 0);
	if (!emi_mpu) {
		of_node_put(btif);
		return dev_err_probe(dev, -EINVAL, "missing EMI MPU supplier\n");
	}
	component_match_add_release(dev, &match, component_release_of,
				    component_compare_of, emi_mpu);
	component_match_add_release(dev, &match, component_release_of,
				    component_compare_of, btif);
	platform_set_drvdata(pdev, consys);
	ret = component_master_add_with_match(dev, &mt6797_consys_component_ops,
					      match);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		return dev_err_probe(dev, ret,
				     "failed to register BTIF aggregate\n");
	}
	ret = devm_add_action_or_reset(dev, mt6797_consys_component_del, dev);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		return ret;
	}

	dev_info(dev,
		 "verified firmware ready; disabled DT is the CONSYS activation gate\n");

	return 0;
}

static const struct of_device_id mt6797_consys_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_of_match);

static struct platform_driver mt6797_consys_driver = {
	.probe = mt6797_consys_probe,
	.driver = {
		.name = "mt6797-consys",
		.of_match_table = mt6797_consys_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CONSYS ownership foundation");
MODULE_LICENSE("GPL");
