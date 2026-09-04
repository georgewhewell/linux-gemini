// SPDX-License-Identifier: GPL-2.0-only
/* Persistent post-configuration HIF and single-owner IRQ lifecycle. */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/smp.h>

#include <linux/soc/mediatek/mt6797-consys-wifi.h>

#include "boot.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"

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

static int mt6797_wifi_hif_runtime_clear_status(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;
	u32 released;
	u32 status;
	unsigned int queue;
	int ret;

	ret = mt6797_wifi_hif_reg_read(irq, MT6797_WIFI_MCR_WHISR, &status);
	if (ret)
		return ret;
	if (status & (MT6797_WIFI_WHISR_RX0_DONE |
		      MT6797_WIFI_WHISR_RX1_DONE |
		      MT6797_WIFI_WHISR_ABNORMAL |
		      MT6797_WIFI_WHISR_D2H_SW))
		return -EUCLEAN;
	if (!(status & MT6797_WIFI_WHISR_TX_DONE))
		return 0;
	for (queue = 0; queue < 8; queue++) {
		ret = mt6797_wifi_hif_reg_read(irq,
			MT6797_WIFI_MCR_WTQCR0 + queue * sizeof(u32), &released);
		if (ret)
			return ret;
	}
	return 0;
}

static void mt6797_wifi_hif_runtime_publish(
	void *context, enum mt6797_wifi_hif_irq_mode mode)
{
	struct mt6797_wifi_hif_irq *irq = context;

	/* Release publishes all prepared session state before opening a gate. */
	smp_store_release(&irq->mode, mode);
}

static int mt6797_wifi_hif_runtime_enable_device(void *context)
{
	struct mt6797_wifi_hif_irq *irq = context;

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

	return mt6797_wifi_hif_reg_write(irq, MT6797_WIFI_MCR_WHLPCR,
					 MT6797_WIFI_WHLPCR_INT_CLR);
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
	int ret;

	mutex_lock(&irq->lifecycle_lock);
	if (mt6797_wifi_hif_irq_mode(irq) != MT6797_WIFI_HIF_IRQ_TERMINAL) {
		mutex_unlock(&irq->lifecycle_lock);
		return;
	}
	ret = __mt6797_wifi_hif_runtime_stop(irq, true);
	mutex_unlock(&irq->lifecycle_lock);
	dev_err(irq->dev,
		"unexpected runtime HIF interrupt; WLAN fenced and stopped%s\n",
		ret ? " with cleanup failure" : "");
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
	if (mode == MT6797_WIFI_HIF_IRQ_RUNTIME &&
	    mt6797_wifi_hif_runtime_irq_terminal(&irq->mode))
		schedule_work(&irq->terminal_work);
	return IRQ_HANDLED;
}

bool mt6797_wifi_hif_runtime_irq_terminal(int *mode)
{
	return mode && cmpxchg(mode, MT6797_WIFI_HIF_IRQ_RUNTIME,
			       MT6797_WIFI_HIF_IRQ_TERMINAL) ==
		       MT6797_WIFI_HIF_IRQ_RUNTIME;
}

static irqreturn_t mt6797_wifi_hif_irq_thread(int linux_irq, void *data)
{
	struct mt6797_wifi_hif_irq *irq = data;
	irqreturn_t (*thread)(void *context);
	void *context;

	(void)linux_irq;
	/* Acquire makes the capability callback and its context visible. */
	if (smp_load_acquire(&irq->mode) != MT6797_WIFI_HIF_IRQ_CAPABILITY)
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
	mutex_init(&irq->lifecycle_lock);
	INIT_WORK(&irq->terminal_work, mt6797_wifi_hif_terminal_work);
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
