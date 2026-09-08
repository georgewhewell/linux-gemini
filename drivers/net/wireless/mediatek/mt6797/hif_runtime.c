// SPDX-License-Identifier: GPL-2.0-only
/* Persistent post-configuration HIF and single-owner IRQ lifecycle. */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-consys-wifi.h>

#include "boot.h"
#include "hif.h"
#include "hif_pump.h"
#include "hif_regs.h"
#include "hif_runtime.h"

#define MT6797_WIFI_HIF_RX_APPEND_LENGTH	4U
#define MT6797_WIFI_HIF_PUMP_TIMEOUT_NS		(100ULL * NSEC_PER_MSEC)

static bool mt6797_wifi_hif_runtime_ops_valid(
	const struct mt6797_wifi_hif_runtime_ops *ops)
{
	return ops && ops->precheck && ops->mask_wrapper &&
		ops->save_registers && ops->program_runtime &&
		ops->clear_status && ops->publish_mode &&
		ops->enable_device && ops->enable_linux &&
		ops->unmask_wrapper && ops->disable_device &&
		ops->disable_linux && ops->restore_registers &&
		ops->fence && ops->teardown;
}

static int mt6797_wifi_hif_runtime_cleanup(
	const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction,
	bool terminal)
{
	int cleanup = 0;
	int ret;
	bool fenced = false;

	if (terminal)
		transaction->terminal = true;
	if (transaction->terminal)
		ops->publish_mode(ops->context, MT6797_WIFI_HIF_IRQ_TERMINAL);
	else
		ops->publish_mode(ops->context, MT6797_WIFI_HIF_IRQ_QUIESCING);

	ops->mask_wrapper(ops->context);
	if (transaction->device_touched) {
		ret = ops->disable_device(ops->context);
		if (ret)
			cleanup = ret;
	}
	if (transaction->linux_enabled)
		ops->disable_linux(ops->context);
	/* Close the wrapper again after synchronizing a racing hard handler. */
	ops->mask_wrapper(ops->context);
	if (transaction->registers_saved) {
		ret = ops->restore_registers(ops->context);
		if (!cleanup && ret)
			cleanup = ret;
	}
	if (cleanup)
		transaction->terminal = true;
	if (transaction->terminal || cleanup) {
		ops->fence(ops->context);
		fenced = true;
	}

	/* HIF is quiesced before the aggregate lease and firmware are released. */
	ret = ops->teardown(ops->context);
	if (!cleanup && ret)
		cleanup = ret;
	if (cleanup) {
		transaction->terminal = true;
		if (!fenced)
			ops->fence(ops->context);
	}

	transaction->active = false;
	transaction->linux_enabled = false;
	transaction->device_touched = false;
	transaction->registers_saved = false;
	transaction->registers_dirty = false;
	ops->publish_mode(ops->context, transaction->terminal ?
			  MT6797_WIFI_HIF_IRQ_TERMINAL :
			  MT6797_WIFI_HIF_IRQ_QUIESCENT);
	return cleanup;
}

int mt6797_wifi_hif_runtime_transaction_start(
	const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction)
{
	int cleanup;
	int ret;

	if (!mt6797_wifi_hif_runtime_ops_valid(ops) || !transaction)
		return -EINVAL;
	if (transaction->terminal)
		return -ESHUTDOWN;
	if (transaction->active || transaction->registers_saved ||
	    transaction->registers_dirty || transaction->device_touched ||
	    transaction->linux_enabled)
		return -EALREADY;

	ret = ops->precheck(ops->context);
	if (ret) {
		if (ret == -EUCLEAN) {
			transaction->terminal = true;
			ops->publish_mode(ops->context,
					  MT6797_WIFI_HIF_IRQ_TERMINAL);
			ops->fence(ops->context);
		}
		cleanup = ops->teardown(ops->context);
		if (cleanup) {
			transaction->terminal = true;
			ops->publish_mode(ops->context,
					  MT6797_WIFI_HIF_IRQ_TERMINAL);
			ops->fence(ops->context);
		}
		return ret;
	}

	ops->mask_wrapper(ops->context);
	ret = ops->save_registers(ops->context);
	if (ret)
		goto fail;
	transaction->registers_saved = true;

	/* Any attempted device write may have changed state despite an error. */
	transaction->registers_dirty = true;
	ret = ops->program_runtime(ops->context);
	if (ret)
		goto fail;
	ret = ops->clear_status(ops->context);
	if (ret)
		goto fail;

	/* Publish the consumer before either interrupt gate can expose it. */
	ops->publish_mode(ops->context, MT6797_WIFI_HIF_IRQ_RUNTIME);
	transaction->device_touched = true;
	ret = ops->enable_device(ops->context);
	if (ret)
		goto fail;
	transaction->linux_enabled = true;
	ret = ops->enable_linux(ops->context);
	if (ret)
		goto fail;
	transaction->active = true;
	ops->unmask_wrapper(ops->context);
	return 0;

fail:
	transaction->terminal = transaction->registers_dirty || ret == -EUCLEAN;
	cleanup = mt6797_wifi_hif_runtime_cleanup(ops, transaction,
						 transaction->terminal);
	return ret ?: cleanup;
}

int mt6797_wifi_hif_runtime_transaction_stop(
	const struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction,
	bool terminal)
{
	if (!mt6797_wifi_hif_runtime_ops_valid(ops) || !transaction)
		return -EINVAL;
	if (!transaction->active)
		return transaction->terminal ? -ESHUTDOWN : -EALREADY;

	return mt6797_wifi_hif_runtime_cleanup(ops, transaction, terminal);
}

static int mt6797_wifi_hif_reg_read(struct mt6797_wifi_hif_irq *irq,
				    u32 address, u32 *value)
{
	__le32 data;
	int ret;

	ret = mt6797_wifi_ahb_cmd53_read(irq->hif, address, &data,
					 sizeof(data));
	if (!ret)
		*value = le32_to_cpu(data);
	return ret;
}

static int mt6797_wifi_hif_reg_write(struct mt6797_wifi_hif_irq *irq,
				     u32 address, u32 value)
{
	__le32 data = cpu_to_le32(value);

	return mt6797_wifi_ahb_cmd53_write(irq->hif, address, &data,
					  sizeof(data));
}

static int mt6797_wifi_hif_runtime_precheck(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	struct mt6797_wifi_boot *boot = irq->boot;

	if (!boot || !boot->online || !boot->transaction.ready || !boot->lease ||
	    boot->hif != irq->hif)
		return -EPIPE;
	return mt6797_wifi_hif_irq_mode(irq) ==
		MT6797_WIFI_HIF_IRQ_QUIESCENT ? 0 : -EUCLEAN;
}

static void mt6797_wifi_hif_runtime_mask(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	mt6797_wifi_ahb_irq_mask(irq->hif);
}

static int mt6797_wifi_hif_runtime_save(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	int ret;

	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHCR,
				       &irq->whcr_before);
	if (ret)
		return ret;
	return mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHIER,
					&irq->whier_before);
}

static int mt6797_wifi_hif_runtime_program(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	u32 value;
	u32 whcr = irq->whcr_before;
	int ret;

	whcr &= ~(MT6797_WIFI_WHCR_RX_ENHANCE |
		   MT6797_WIFI_WHCR_MAX_RX_LENGTHS |
		   MT6797_WIFI_WHCR_MAILBOX_READ_CLEAR |
		   MT6797_WIFI_WHCR_INT_WRITE_1_CLEAR);
	ret = mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHCR, whcr);
	if (ret)
		return ret;
	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHCR, &value);
	if (ret || value != whcr)
		return ret ?: -EIO;
	ret = mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHIER,
					MT6797_WIFI_WHIER_DEFAULT);
	if (ret)
		return ret;
	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHIER, &value);
	return ret ?: (value == MT6797_WIFI_WHIER_DEFAULT ? 0 : -EIO);
}

static void mt6797_wifi_hif_runtime_reject_packet(
	void *context, const u8 *packet, size_t length, int error)
{
	struct mt6797_wifi_hif_irq *irq = context;

	/* The first four bytes contain length/type, including for reports whose
	 * remaining layout is not an RX descriptor. Do not dump their contents.
	 */
	dev_warn_ratelimited(irq->dev, "RX rejected error=%d available=%zu type=%#x\n",
		error, length, length >= 4 ? get_unaligned_le16(packet + 2) : 0);
}

static int mt6797_wifi_hif_runtime_clear_status(void *context);

static void mt6797_wifi_hif_runtime_publish(
	void *context, enum mt6797_wifi_hif_irq_mode mode)
{
	struct mt6797_wifi_hif_irq *irq = context;
	unsigned long flags;

	/* Release publishes all prepared session state before opening a gate. */
	if (mode == MT6797_WIFI_HIF_IRQ_TERMINAL) {
		spin_lock_irqsave(&irq->terminal_lock, flags);
		if (READ_ONCE(irq->terminal_reason) >= 0)
			WRITE_ONCE(irq->terminal_reason, -ESHUTDOWN);
		/* Status readers acquire the reason with this terminal mode. */
		smp_store_release(&irq->mode, mode);
		spin_unlock_irqrestore(&irq->terminal_lock, flags);
		return;
	}
	smp_store_release(&irq->mode, mode);
}

static int mt6797_wifi_hif_runtime_enable_device(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	int ret;

	ret = mt6797_wifi_ahb_irq_gate_update(irq->hif, &irq->cccr_gate, true);
	if (ret)
		return ret;

	return mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHLPCR,
					 MT6797_WIFI_WHLPCR_INT_SET);
}

static int mt6797_wifi_hif_runtime_enable_linux(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	enable_irq(irq->irq);
	return mt6797_wifi_hif_irq_mode(irq) == MT6797_WIFI_HIF_IRQ_RUNTIME ?
		0 : -EIO;
}

static void mt6797_wifi_hif_runtime_unmask(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	mt6797_wifi_ahb_irq_unmask(irq->hif);
}

static int mt6797_wifi_hif_runtime_disable_device(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	int ret, gate_ret;

	ret = mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHLPCR,
				      MT6797_WIFI_WHLPCR_INT_CLR);
	gate_ret = mt6797_wifi_ahb_irq_gate_update(irq->hif, &irq->cccr_gate, false);
	return ret ?: gate_ret;
}

static void mt6797_wifi_hif_runtime_disable_linux(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	disable_irq(irq->irq);
}

static int mt6797_wifi_hif_runtime_restore(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	u32 value;
	int cleanup = 0;
	int ret;

	ret = mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHIER,
					 irq->whier_before);
	if (ret)
		cleanup = ret;
	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHIER, &value);
	if (!cleanup && (ret || value != irq->whier_before))
		cleanup = ret ?: -EIO;
	ret = mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHCR,
					 irq->whcr_before);
	if (!cleanup && ret)
		cleanup = ret;
	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHCR, &value);
	if (!cleanup && (ret || value != irq->whcr_before))
		cleanup = ret ?: -EIO;
	return cleanup;
}

static void mt6797_wifi_hif_runtime_fence(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	if (irq->boot && irq->boot->lease)
		mt6797_consys_wifi_fence(irq->boot->lease);
}

static int mt6797_wifi_hif_runtime_teardown(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	if (!irq->boot || !irq->boot->online)
		return 0;
	return mt6797_wifi_boot_stop(irq->boot);
}

static void mt6797_wifi_hif_runtime_init_ops(
	struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_hif_runtime_ops *ops)
{
	*ops = (struct mt6797_wifi_hif_runtime_ops) {
		.precheck = mt6797_wifi_hif_runtime_precheck,
		.mask_wrapper = mt6797_wifi_hif_runtime_mask,
		.save_registers = mt6797_wifi_hif_runtime_save,
		.program_runtime = mt6797_wifi_hif_runtime_program,
		.clear_status = mt6797_wifi_hif_runtime_clear_status,
		.publish_mode = mt6797_wifi_hif_runtime_publish,
		.enable_device = mt6797_wifi_hif_runtime_enable_device,
		.enable_linux = mt6797_wifi_hif_runtime_enable_linux,
		.unmask_wrapper = mt6797_wifi_hif_runtime_unmask,
		.disable_device = mt6797_wifi_hif_runtime_disable_device,
		.disable_linux = mt6797_wifi_hif_runtime_disable_linux,
		.restore_registers = mt6797_wifi_hif_runtime_restore,
		.fence = mt6797_wifi_hif_runtime_fence,
		.teardown = mt6797_wifi_hif_runtime_teardown,
		.context = irq,
	};
}

static int __mt6797_wifi_hif_runtime_stop(struct mt6797_wifi_hif_irq *irq,
					  bool terminal)
{
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	if (!irq->transaction.active)
		return 0;
	mt6797_wifi_hif_runtime_init_ops(irq, &ops);
	ret = mt6797_wifi_hif_runtime_transaction_stop(&ops, &irq->transaction,
						 terminal);
	if (irq->boot && !irq->boot->online)
		irq->boot = NULL;
	return ret;
}

static void mt6797_wifi_hif_terminal_work(struct work_struct *work)
{
	struct mt6797_wifi_hif_irq *irq =
		container_of(work, struct mt6797_wifi_hif_irq, terminal_work);
	struct mt6797_wifi_tx_credit credit;
	unsigned long flags;
	unsigned int queue;
	int ret;

	mutex_lock(&irq->lifecycle_lock);
	if (mt6797_wifi_hif_irq_mode(irq) != MT6797_WIFI_HIF_IRQ_TERMINAL) {
		mutex_unlock(&irq->lifecycle_lock);
		return;
	}
	ret = __mt6797_wifi_hif_runtime_stop(irq, true);
	spin_lock_irqsave(&irq->tx_credit_lock, flags);
	credit = irq->tx_credit;
	spin_unlock_irqrestore(&irq->tx_credit_lock, flags);
	/* Software counters only: no further HIF accesses after a terminal fault. */
	dev_err(irq->dev, "terminal TX credits: available=%llu\n", credit.available);
	for (queue = 0; queue < MT6797_WIFI_TX_CREDIT_QUEUES; queue++)
		dev_err(irq->dev,
			"terminal TX queue=%u limit=%u free=%u done=%u preused=%llu released=%llu\n",
			queue, credit.limit[queue], credit.free[queue], credit.done[queue],
			credit.preused[queue],
			READ_ONCE(irq->pump_stats.tx_released[queue]));
	mutex_unlock(&irq->lifecycle_lock);
	dev_err(irq->dev,
		"runtime HIF fault %d; WLAN fenced and stopped%s\n",
		READ_ONCE(irq->terminal_reason),
		ret ? " with cleanup failure" : "");
}

static void mt6797_wifi_hif_runtime_terminal(
	struct mt6797_wifi_hif_irq *irq, int reason)
{
	if (!mt6797_wifi_hif_runtime_publish_terminal(irq, reason))
		return;
	mt6797_wifi_hif_wait_event_abort(&irq->wait_event, reason);
	mt6797_wifi_hif_runtime_sink_abort(irq, reason);
	schedule_work(&irq->terminal_work);
}

static irqreturn_t mt6797_wifi_hif_irq_handler(int linux_irq, void *data)
{
	struct mt6797_wifi_hif_irq *irq = data;
	int mode;

	(void)linux_irq;
	mt6797_wifi_ahb_irq_mask(irq->hif);
	/* Acquire pairs with mode publication before either IRQ gate is opened. */
	mode = smp_load_acquire(&irq->mode);
	if (mode == MT6797_WIFI_HIF_IRQ_CAPABILITY)
		return IRQ_WAKE_THREAD;
	if (mode == MT6797_WIFI_HIF_IRQ_RUNTIME)
		return IRQ_WAKE_THREAD;
	return IRQ_HANDLED;
}

bool mt6797_wifi_hif_runtime_mark_terminal(int *mode)
{
	return mode && cmpxchg(mode, MT6797_WIFI_HIF_IRQ_RUNTIME,
			       MT6797_WIFI_HIF_IRQ_TERMINAL) ==
		       MT6797_WIFI_HIF_IRQ_RUNTIME;
}

bool mt6797_wifi_hif_runtime_publish_terminal(
	struct mt6797_wifi_hif_irq *irq, int reason)
{
	unsigned long flags;
	int previous;
	bool published;

	if (!irq)
		return false;
	reason = reason < 0 ? reason : -EIO;
	spin_lock_irqsave(&irq->terminal_lock, flags);
	previous = READ_ONCE(irq->terminal_reason);
	/* cmpxchg is a full barrier: reason precedes observable TERMINAL mode. */
	WRITE_ONCE(irq->terminal_reason, reason);
	published = mt6797_wifi_hif_runtime_mark_terminal(&irq->mode);
	if (!published)
		WRITE_ONCE(irq->terminal_reason, previous);
	spin_unlock_irqrestore(&irq->terminal_lock, flags);
	return published;
}

int mt6797_wifi_hif_runtime_status(struct mt6797_wifi_hif_irq *irq)
{
	unsigned long flags;
	int reason;
	int mode;

	if (!irq)
		return -EINVAL;
	spin_lock_irqsave(&irq->terminal_lock, flags);
	/* Pairs with runtime or terminal release publication. */
	mode = smp_load_acquire(&irq->mode);
	reason = READ_ONCE(irq->terminal_reason);
	spin_unlock_irqrestore(&irq->terminal_lock, flags);
	if (mode == MT6797_WIFI_HIF_IRQ_RUNTIME)
		return 0;
	if (mode != MT6797_WIFI_HIF_IRQ_TERMINAL)
		return -EPIPE;
	return reason < 0 ? reason : -ESHUTDOWN;
}

void mt6797_wifi_hif_runtime_fail(struct mt6797_wifi_hif_irq *irq,
				   int reason)
{
	if (irq)
		mt6797_wifi_hif_runtime_terminal(irq, reason);
}

void mt6797_wifi_hif_wait_event_init(
	struct mt6797_wifi_hif_wait_event *wait_event)
{
	if (!wait_event)
		return;
	memset(wait_event, 0, sizeof(*wait_event));
	spin_lock_init(&wait_event->lock);
	init_completion(&wait_event->completion);
}

int mt6797_wifi_hif_wait_event_arm(
	struct mt6797_wifi_hif_wait_event *wait_event, u8 id,
	const u8 *match, size_t match_length)
{
	unsigned long flags;
	int ret = 0;

	if (!wait_event || !id || match_length > sizeof(wait_event->match) ||
	    (match_length && !match))
		return -EINVAL;
	spin_lock_irqsave(&wait_event->lock, flags);
	if (wait_event->armed || wait_event->ready) {
		ret = -EBUSY;
		goto out;
	}
	reinit_completion(&wait_event->completion);
	wait_event->id = id;
	wait_event->match_length = match_length;
	if (match_length)
		memcpy(wait_event->match, match, match_length);
	wait_event->armed = true;
out:
	spin_unlock_irqrestore(&wait_event->lock, flags);
	return ret;
}

int mt6797_wifi_hif_wait_event_offer(
	struct mt6797_wifi_hif_wait_event *wait_event,
	const struct mt6797_wifi_event_view *event)
{
	unsigned long flags;
	size_t copied;
	int ret = 0;

	if (!wait_event || !event)
		return -EINVAL;
	spin_lock_irqsave(&wait_event->lock, flags);
	if (!wait_event->armed || event->id != wait_event->id)
		goto out;
	/* A short matching-ID event cannot safely be classified as foreign. */
	if (event->payload &&
	    event->payload_length >= wait_event->match_length &&
	    wait_event->match_length &&
	    memcmp(event->payload, wait_event->match,
		   wait_event->match_length))
		goto out;
	if (wait_event->ready) {
		ret = -EOVERFLOW;
		goto out;
	}

	wait_event->id = event->id;
	wait_event->sequence = event->sequence;
	wait_event->payload_length = event->payload_length;
	wait_event->packet_length = event->packet_length;
	wait_event->error = event->payload_length > sizeof(wait_event->payload) ?
		-EMSGSIZE : 0;
	copied = min(event->payload_length, sizeof(wait_event->payload));
	if (copied) {
		if (!event->payload) {
			wait_event->error = -EPROTO;
			copied = 0;
		} else {
			memcpy(wait_event->payload, event->payload, copied);
		}
	}
	wait_event->ready = true;
	/* Complete while locked so disarm cannot race a stale completion. */
	complete(&wait_event->completion);
out:
	spin_unlock_irqrestore(&wait_event->lock, flags);
	return ret;
}

int mt6797_wifi_hif_wait_event_take(
	struct mt6797_wifi_hif_wait_event *wait_event,
	struct mt6797_wifi_hif_event_copy *copy)
{
	unsigned long flags;
	int ret = 0;

	if (!wait_event || !copy)
		return -EINVAL;
	spin_lock_irqsave(&wait_event->lock, flags);
	if (!wait_event->armed) {
		ret = -ENOTCONN;
		goto out;
	}
	if (!wait_event->ready) {
		ret = -EAGAIN;
		goto out;
	}
	memset(copy, 0, sizeof(*copy));
	copy->id = wait_event->id;
	copy->sequence = wait_event->sequence;
	copy->payload_length = wait_event->payload_length;
	copy->packet_length = wait_event->packet_length;
	copy->error = wait_event->error;
	memcpy(copy->payload, wait_event->payload, sizeof(copy->payload));
	wait_event->ready = false;
	wait_event->armed = false;
out:
	spin_unlock_irqrestore(&wait_event->lock, flags);
	return ret;
}

void mt6797_wifi_hif_wait_event_abort(
	struct mt6797_wifi_hif_wait_event *wait_event, int reason)
{
	unsigned long flags;

	if (!wait_event)
		return;
	spin_lock_irqsave(&wait_event->lock, flags);
	if (wait_event->armed && !wait_event->ready) {
		wait_event->error = reason < 0 ? reason : -EIO;
		wait_event->ready = true;
		complete(&wait_event->completion);
	}
	spin_unlock_irqrestore(&wait_event->lock, flags);
}

void mt6797_wifi_hif_wait_event_disarm(
	struct mt6797_wifi_hif_wait_event *wait_event)
{
	unsigned long flags;

	if (!wait_event)
		return;
	spin_lock_irqsave(&wait_event->lock, flags);
	wait_event->armed = false;
	wait_event->ready = false;
	wait_event->error = 0;
	wait_event->match_length = 0;
	wait_event->payload_length = 0;
	wait_event->packet_length = 0;
	reinit_completion(&wait_event->completion);
	spin_unlock_irqrestore(&wait_event->lock, flags);
}

static bool mt6797_wifi_hif_runtime_pump_expired(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

	return ktime_after(ktime_get(), irq->pump_deadline);
}

static int mt6797_wifi_hif_runtime_pump_status(void *context, u32 *status)
{
	struct mt6797_wifi_hif_irq *irq = context;

	return mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHISR, status);
}

int mt6797_wifi_hif_runtime_send(struct mt6797_wifi_hif_irq *irq,
	u32 address, const u8 *packet, size_t length)
{
	unsigned long flags;
	unsigned int queue;
	u16 wire_length;
	int ret;

	if (!irq || !packet || length < MT6797_WIFI_COMMAND_HEADER_LENGTH ||
	    length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
		return -EINVAL;
	lockdep_assert_held(&irq->lifecycle_lock);
	wire_length = get_unaligned_le16(packet);
	if (wire_length < MT6797_WIFI_COMMAND_HEADER_LENGTH || wire_length > length ||
	    length - wire_length > 3)
		return -EINVAL;
	/* GEN3 uses WTDR1 for both LMAC and MCU; the descriptor selects queue. */
	if (address != MT6797_WIFI_MCR_WTDR1)
		return -EINVAL;
	if (get_unaligned_le16(packet + 2) == 0x8000 ||
	    (length >= MT6797_WIFI_TX_DESCRIPTOR_LENGTH && packet[3] == 0x88))
		queue = MT6797_WIFI_TX_CREDIT_CPU;
	else if (length >= MT6797_WIFI_TX_DESCRIPTOR_LENGTH &&
		 packet[3] == (4U << 3))
		queue = MT6797_WIFI_TX_CREDIT_DATA;
	else
		return -EINVAL;
	ret = mt6797_wifi_hif_runtime_status(irq);
	if (ret)
		return ret;
	spin_lock_irqsave(&irq->tx_credit_lock, flags);
	ret = mt6797_wifi_tx_credit_acquire(&irq->tx_credit, queue, wire_length);
	if (ret == -EAGAIN)
		reinit_completion(&irq->tx_credit_completion);
	spin_unlock_irqrestore(&irq->tx_credit_lock, flags);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd53_write(irq->hif, address, packet, length);
	if (ret)
		mt6797_wifi_hif_runtime_fail(irq, ret);
	return ret;
}

static int mt6797_wifi_hif_runtime_pump_tx(
	void *context, u16 released[MT6797_WIFI_HIF_TX_QUEUES])
{
	struct mt6797_wifi_hif_irq *irq = context;
	unsigned int register_index;
	unsigned long flags;
	u32 value;
	int ret;

	for (register_index = 0; register_index < 8; register_index++) {
		if (mt6797_wifi_hif_runtime_pump_expired(irq))
			return -ETIMEDOUT;
		ret = mt6797_wifi_hif_reg_read(
			irq, MT6797_WIFI_MCR_WTQCR0 +
			register_index * sizeof(u32), &value);
		if (ret)
			return ret;
		released[register_index * 2] = value;
		released[register_index * 2 + 1] = value >> 16;
	}
	spin_lock_irqsave(&irq->tx_credit_lock, flags);
	ret = mt6797_wifi_tx_credit_release(&irq->tx_credit, released);
	if (!ret)
		complete(&irq->tx_credit_completion);
	spin_unlock_irqrestore(&irq->tx_credit_lock, flags);
	return ret;
}

static int mt6797_wifi_hif_runtime_pump_lengths(void *context,
						u16 lengths[2])
{
	struct mt6797_wifi_hif_irq *irq = context;
	u32 value;
	int ret;

	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WRPLR, &value);
	if (ret)
		return ret;
	lengths[0] = value;
	lengths[1] = value >> 16;
	return 0;
}

static int mt6797_wifi_hif_runtime_pump_packet(void *context,
					       unsigned int port, u16 length,
					       const u8 **packet)
{
	struct mt6797_wifi_hif_irq *irq = context;
	size_t wire_length;
	u32 address;
	int ret;

	if (port >= ARRAY_SIZE(irq->rx_buffer) || !packet)
		return -EINVAL;
	wire_length = ALIGN((size_t)length + MT6797_WIFI_HIF_RX_APPEND_LENGTH,
			    sizeof(u32));
	if (wire_length > MT6797_WIFI_AHB_MAX_TRANSFER_SIZE)
		return -EMSGSIZE;
	address = port ? MT6797_WIFI_MCR_WRDR1 : MT6797_WIFI_MCR_WRDR0;
	ret = mt6797_wifi_ahb_cmd53_read(irq->hif, address,
					irq->rx_buffer[port], wire_length);
	if (ret)
		return ret;
	*packet = irq->rx_buffer[port];
	return 0;
}

int mt6797_wifi_hif_runtime_route_event(
	struct mt6797_wifi_scan_transaction *transaction,
	const struct mt6797_wifi_event_view *event)
{
	enum mt6797_wifi_scan_event_result result;

	if (!transaction || !event)
		return -EINVAL;
	if (event->id != MT6797_WIFI_SCAN_DONE_EVENT_ID)
		return 0;
	return mt6797_wifi_scan_event(transaction, event, &result);
}

int mt6797_wifi_hif_runtime_sink_attach(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_hif_runtime_sink *sink)
{
	unsigned long flags;
	int mode;
	int ret = 0;

	if (!irq || !sink || !sink->event || !sink->frame || !sink->abort ||
	    !sink->context)
		return -EINVAL;
	lockdep_assert_held(&irq->lifecycle_lock);
	/*
	 * Hold terminal_lock through installation. A terminal publisher either
	 * follows this section and aborts the installed sink, or precedes it and
	 * makes attachment fail; it can never abort an empty slot then lose a
	 * newly installed sink.
	 */
	spin_lock_irqsave(&irq->terminal_lock, flags);
	mode = READ_ONCE(irq->mode);
	if (mode != MT6797_WIFI_HIF_IRQ_RUNTIME) {
		ret = mode == MT6797_WIFI_HIF_IRQ_TERMINAL &&
		      READ_ONCE(irq->terminal_reason) < 0 ?
			READ_ONCE(irq->terminal_reason) : -EPIPE;
		goto out;
	}
	spin_lock(&irq->sink_lock);
	if (irq->sink.context)
		ret = -EBUSY;
	else
		irq->sink = *sink;
	spin_unlock(&irq->sink_lock);
out:
	spin_unlock_irqrestore(&irq->terminal_lock, flags);
	return ret;
}

int mt6797_wifi_hif_runtime_sink_detach(
	struct mt6797_wifi_hif_irq *irq, void *context)
{
	unsigned long flags;
	int ret = 0;

	if (!irq || !context)
		return -EINVAL;
	lockdep_assert_held(&irq->lifecycle_lock);
	spin_lock_irqsave(&irq->sink_lock, flags);
	if (irq->sink.context != context)
		ret = -ESTALE;
	else
		memset(&irq->sink, 0, sizeof(irq->sink));
	spin_unlock_irqrestore(&irq->sink_lock, flags);
	return ret;
}

int mt6797_wifi_hif_runtime_sink_event(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_event_view *event)
{
	unsigned long flags;
	int ret = 0;

	if (!irq || !event)
		return -EINVAL;
	spin_lock_irqsave(&irq->sink_lock, flags);
	if (irq->sink.context)
		ret = irq->sink.event(irq->sink.context, event);
	spin_unlock_irqrestore(&irq->sink_lock, flags);
	return ret;
}

int mt6797_wifi_hif_runtime_sink_frame(
	struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_rx_view *frame)
{
	unsigned long flags;
	int ret = 0;

	if (!irq || !frame)
		return -EINVAL;
	spin_lock_irqsave(&irq->sink_lock, flags);
	if (irq->sink.context)
		ret = irq->sink.frame(irq->sink.context, frame);
	spin_unlock_irqrestore(&irq->sink_lock, flags);
	return ret;
}

void mt6797_wifi_hif_runtime_sink_abort(
	struct mt6797_wifi_hif_irq *irq, int reason)
{
	unsigned long flags;

	if (!irq)
		return;
	spin_lock_irqsave(&irq->sink_lock, flags);
	if (irq->sink.context)
		irq->sink.abort(irq->sink.context, reason);
	spin_unlock_irqrestore(&irq->sink_lock, flags);
}

static int mt6797_wifi_hif_runtime_pump_event(
	void *context, const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_hif_irq *irq = context;
	int ret;

	ret = mt6797_wifi_hif_wait_event_offer(&irq->wait_event, event);
	if (ret)
		return ret;
	return mt6797_wifi_hif_runtime_sink_event(irq, event);
}

static int mt6797_wifi_hif_runtime_pump_frame(
	void *context, const struct mt6797_wifi_rx_view *frame)
{
	return mt6797_wifi_hif_runtime_sink_frame(context, frame);
}

static int mt6797_wifi_hif_runtime_clear_status(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	struct mt6797_wifi_hif_pump_ops ops = {
		.read_status = mt6797_wifi_hif_runtime_pump_status,
		.read_tx_released = mt6797_wifi_hif_runtime_pump_tx,
		.read_lengths = mt6797_wifi_hif_runtime_pump_lengths,
		.read_packet = mt6797_wifi_hif_runtime_pump_packet,
		.deliver_event = mt6797_wifi_hif_runtime_pump_event,
		.deliver_frame = mt6797_wifi_hif_runtime_pump_frame,
		.reject_packet = mt6797_wifi_hif_runtime_reject_packet,
		.expired = mt6797_wifi_hif_runtime_pump_expired,
		.allow_empty_status = true,
		.context = irq,
	};
	int ret;

	/*
	 * Firmware can queue RX before the host opens its interrupt gates.
	 * Consume it with the ordinary bounded parser while Linux IRQ and the
	 * wrapper remain masked; do not discard read-clear RX indications.
	 */
	memset(irq->rx_buffer[0], 0, MT6797_WIFI_EVENT_HEADER_LENGTH);
	memset(irq->rx_buffer[1], 0, MT6797_WIFI_EVENT_HEADER_LENGTH);
	irq->pump_deadline = ktime_add_ns(ktime_get(),
		MT6797_WIFI_HIF_PUMP_TIMEOUT_NS);
	ret = mt6797_wifi_hif_pump_run(&ops, &irq->pump_stats,
				     &irq->acknowledged_status);
	dev_info(irq->dev, "HIF startup result=%d status=%#x rx=%llu events=%llu rx0_header=%8ph rx1_header=%8ph\n",
		 ret, irq->acknowledged_status, irq->pump_stats.rx_packets,
		 irq->pump_stats.events, irq->rx_buffer[0], irq->rx_buffer[1]);
	return ret;
}

static irqreturn_t mt6797_wifi_hif_irq_thread(int linux_irq, void *data)
{
	struct mt6797_wifi_hif_irq *irq = data;
	irqreturn_t (*thread)(void *context);
	struct mt6797_wifi_hif_pump_ops ops;
	u64 interrupts;
	void *context;
	int ret;

	(void)linux_irq;
	/* Pairs with release publication before either IRQ gate is opened. */
	ret = smp_load_acquire(&irq->mode);
	if (ret == MT6797_WIFI_HIF_IRQ_RUNTIME) {
		if (check_add_overflow(irq->pump_stats.interrupts, 1ULL,
				       &interrupts)) {
			mt6797_wifi_hif_runtime_terminal(irq, -EOVERFLOW);
			return IRQ_HANDLED;
		}
		irq->pump_stats.interrupts = interrupts;
		irq->pump_deadline = ktime_add_ns(ktime_get(),
			MT6797_WIFI_HIF_PUMP_TIMEOUT_NS);
		ops = (struct mt6797_wifi_hif_pump_ops) {
			.read_status = mt6797_wifi_hif_runtime_pump_status,
			.read_tx_released = mt6797_wifi_hif_runtime_pump_tx,
			.read_lengths = mt6797_wifi_hif_runtime_pump_lengths,
			.read_packet = mt6797_wifi_hif_runtime_pump_packet,
			.deliver_event = mt6797_wifi_hif_runtime_pump_event,
			.deliver_frame = mt6797_wifi_hif_runtime_pump_frame,
			.reject_packet = mt6797_wifi_hif_runtime_reject_packet,
			.expired = mt6797_wifi_hif_runtime_pump_expired,
			.context = irq,
		};
		ret = mt6797_wifi_hif_pump_run(&ops, &irq->pump_stats,
						&irq->acknowledged_status);
		if (ret) {
			mt6797_wifi_hif_runtime_terminal(irq, ret);
			return IRQ_HANDLED;
		}
		dev_dbg(irq->dev,
			"HIF irq status=%#x total=%llu rx=%llu events=%llu\n",
			irq->acknowledged_status, irq->pump_stats.interrupts,
			irq->pump_stats.rx_packets, irq->pump_stats.events);
		if (mt6797_wifi_hif_irq_mode(irq) ==
		    MT6797_WIFI_HIF_IRQ_RUNTIME)
			mt6797_wifi_ahb_irq_unmask(irq->hif);
		return IRQ_HANDLED;
	}
	if (ret != MT6797_WIFI_HIF_IRQ_CAPABILITY)
		return IRQ_HANDLED;
	thread = READ_ONCE(irq->capability_thread);
	context = READ_ONCE(irq->capability_context);
	return thread ? thread(context) : IRQ_HANDLED;
}

int mt6797_wifi_hif_irq_init(struct mt6797_wifi_hif_irq *irq,
			     struct device *dev,
			     struct mt6797_wifi_ahb_hif *hif,
			     int linux_irq)
{
	if (!irq || !dev || !hif || linux_irq < 0)
		return -EINVAL;
	memset(irq, 0, sizeof(*irq));
	irq->dev = dev;
	irq->hif = hif;
	irq->irq = linux_irq;
	irq->rx_buffer[0] = devm_kmalloc(dev,
					 MT6797_WIFI_AHB_MAX_TRANSFER_SIZE,
					 GFP_KERNEL);
	if (!irq->rx_buffer[0])
		return -ENOMEM;
	irq->rx_buffer[1] = devm_kmalloc(dev,
					 MT6797_WIFI_AHB_MAX_TRANSFER_SIZE,
					 GFP_KERNEL);
	if (!irq->rx_buffer[1])
		return -ENOMEM;
	mutex_init(&irq->lifecycle_lock);
	spin_lock_init(&irq->sink_lock);
	spin_lock_init(&irq->terminal_lock);
	spin_lock_init(&irq->tx_credit_lock);
	init_completion(&irq->tx_credit_completion);
	mt6797_wifi_tx_credit_init(&irq->tx_credit);
	INIT_WORK(&irq->terminal_work, mt6797_wifi_hif_terminal_work);
	mt6797_wifi_hif_wait_event_init(&irq->wait_event);
	irq->mode = MT6797_WIFI_HIF_IRQ_QUIESCENT;
	return devm_request_threaded_irq(dev, linux_irq,
					  mt6797_wifi_hif_irq_handler,
					  mt6797_wifi_hif_irq_thread,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN,
					  dev_name(dev), irq);
}

int mt6797_wifi_hif_irq_enter_capability(
	struct mt6797_wifi_hif_irq *irq,
	irqreturn_t (*thread)(void *context), void *context)
{
	if (!irq || !thread || !context)
		return -EINVAL;
	if (mt6797_wifi_hif_irq_mode(irq) != MT6797_WIFI_HIF_IRQ_QUIESCENT)
		return -EBUSY;
	WRITE_ONCE(irq->capability_thread, thread);
	WRITE_ONCE(irq->capability_context, context);
	/* Publish callback state before an enabled hard IRQ can observe the mode. */
	smp_store_release(&irq->mode, MT6797_WIFI_HIF_IRQ_CAPABILITY);
	return 0;
}

void mt6797_wifi_hif_irq_leave_capability(struct mt6797_wifi_hif_irq *irq)
{
	if (!irq)
		return;
	/* Publish the caller's disarm before hiding the capability callback. */
	smp_store_release(&irq->mode, MT6797_WIFI_HIF_IRQ_QUIESCENT);
	WRITE_ONCE(irq->capability_thread, NULL);
	WRITE_ONCE(irq->capability_context, NULL);
}

enum mt6797_wifi_hif_irq_mode
mt6797_wifi_hif_irq_mode(const struct mt6797_wifi_hif_irq *irq)
{
	return irq ? READ_ONCE(irq->mode) :
		MT6797_WIFI_HIF_IRQ_TERMINAL;
}

static int mt6797_wifi_hif_runtime_read_rxinf(struct mt6797_wifi_hif_irq *irq,
	u8 sequence, u32 *value)
{
	struct mt6797_wifi_hif_event_copy event;
	u8 command[16];
	int ret;

	lockdep_assert_held(&irq->lifecycle_lock);
	*value = 0;
	ret = mt6797_wifi_hif_runtime_status(irq);
	if (ret)
		goto out;
	/* GEN3 CMD_ID_ACCESS_REG, query, private startup sequence. */
	ret = mt6797_wifi_command_init(command, sizeof(command), 8, 0xc0, false, sequence);
	if (ret)
		goto out;
	put_unaligned_le32(0x800c0068, command + 8);
	ret = mt6797_wifi_hif_wait_event_arm(&irq->wait_event, 0x05, NULL, 0);
	if (ret)
		goto out;
	ret = mt6797_wifi_hif_runtime_send(irq, MT6797_WIFI_MCR_WTDR1,
		command, sizeof(command));
	if (ret)
		goto disarm;
	if (!wait_for_completion_timeout(&irq->wait_event.completion,
		msecs_to_jiffies(1000))) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	ret = mt6797_wifi_hif_wait_event_take(&irq->wait_event, &event);
	if (ret)
		goto fail;
	ret = event.error;
	if (ret)
		goto fail;
	if (event.id != 0x05 || event.sequence != sequence ||
	    event.packet_length != 16 || event.payload_length != 8 ||
	    get_unaligned_le32(event.payload) != 0x800c0068) {
		ret = -EPROTO;
		goto fail;
	}
	*value = get_unaligned_le32(event.payload + 4);
	goto disarm;
fail:
	mt6797_wifi_hif_runtime_fail(irq, ret);
disarm:
	mt6797_wifi_hif_wait_event_disarm(&irq->wait_event);
out:
	return ret;
}

/* Probe-time only, before registering any command consumer. The exact GEN3
 * firmware exposes this HIF client register through CMD_ACCESS_REG; M18 read
 * zero while CCMP RX omitted group1. Test the mt7603 RXINF group1 bit while
 * preserving every other bit. Readback qualifies the write; actual group1
 * descriptors must still be present before the data decoder accepts CCMP.
 */
int mt6797_wifi_hif_runtime_configure_rxinf(struct mt6797_wifi_hif_irq *irq,
	u32 *before, u32 *after)
{
	u8 command[16];
	u32 requested;
	int ret;

	if (!irq || !before || !after || before == after)
		return -EINVAL;
	*before = 0;
	*after = 0;
	mutex_lock(&irq->lifecycle_lock);
	ret = mt6797_wifi_hif_runtime_read_rxinf(irq, 0xf0, before);
	if (ret)
		goto unlock;
	requested = *before | BIT(0); /* RX status group1: packet number. */
	if (requested == *before) {
		*after = *before;
		goto unlock;
	}
	/* A SET has no required ACK in GEN3. A subsequent query of the same
	 * register, on the same firmware command queue, verifies completion.
	 */
	ret = mt6797_wifi_command_init(command, sizeof(command), 8, 0xc0, true, 0xf1);
	if (ret)
		goto unlock;
	put_unaligned_le32(0x800c0068, command + 8);
	put_unaligned_le32(requested, command + 12);
	ret = mt6797_wifi_hif_runtime_send(irq, MT6797_WIFI_MCR_WTDR1,
		command, sizeof(command));
	if (ret)
		goto fail;
	ret = mt6797_wifi_hif_runtime_read_rxinf(irq, 0xf2, after);
	if (ret)
		goto fail;
	if (*after != requested) {
		ret = -EIO;
		goto fail;
	}
	goto unlock;
fail:
	/* Never expose a consumer after an ambiguous configuration attempt. */
	mt6797_wifi_hif_runtime_fail(irq, ret);
unlock:
	mutex_unlock(&irq->lifecycle_lock);
	return ret;
}

int mt6797_wifi_hif_runtime_start(struct mt6797_wifi_hif_irq *irq,
				   struct mt6797_wifi_boot *boot)
{
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	if (!irq || !boot || irq->hif != boot->hif)
		return -EINVAL;
	mutex_lock(&irq->lifecycle_lock);
	irq->boot = boot;
	mt6797_wifi_hif_runtime_init_ops(irq, &ops);
	ret = mt6797_wifi_hif_runtime_transaction_start(&ops,
							&irq->transaction);
	if (!boot->online)
		irq->boot = NULL;
	mutex_unlock(&irq->lifecycle_lock);
	return ret;
}

int mt6797_wifi_hif_runtime_stop(struct mt6797_wifi_hif_irq *irq)
{
	int ret;

	if (!irq)
		return -EINVAL;
	mutex_lock(&irq->lifecycle_lock);
	ret = __mt6797_wifi_hif_runtime_stop(
		irq, mt6797_wifi_hif_irq_mode(irq) ==
		     MT6797_WIFI_HIF_IRQ_TERMINAL);
	mutex_unlock(&irq->lifecycle_lock);
	/* A queued terminal worker now observes inactive state and becomes a no-op. */
	cancel_work_sync(&irq->terminal_work);
	return ret;
}
