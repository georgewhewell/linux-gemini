// SPDX-License-Identifier: GPL-2.0-only
/* Candidate-only one-shot MT6797 CPU8 admission controller. */

#include <asm/late_cpu_profile.h>
#include <asm/mt6797_a72_membership.h>

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt6797-a72-binder.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "mt6797-a72-admission-controller-internal.h"
#include "mt6797-a72-physical-source-observer-internal.h"

#define MT6797_A72_ADMISSION_TAG "GEMINI_A72_ADMISSION_V1"
#define MT6797_A72_ADMISSION_LIVE_TAG "GEMINI_A72_ADMISSION_LIVE_V1"
#define MT6797_A72_ADMISSION_CPU 8

struct mt6797_a72_admission_controller {
	struct device *dev;
	struct device *binder;
	struct mt6797_a72_physical_source_context source;
	struct mt6797_a72_admission_controller_state state;
	struct mt6797_a72_admission_trigger_state trigger;
};

static bool
mt6797_a72_admission_ops_valid(const struct mt6797_a72_admission_controller_ops *ops)
{
	return ops && ops->binder_ready && ops->ready_token &&
		ops->trace_entry && ops->trace_zero_request &&
		ops->source_register && ops->source_unregister &&
		ops->derive_cpu8 && ops->publish_up && ops->add_cpu;
}

void
mt6797_a72_admission_state_init(struct mt6797_a72_admission_controller_state *state)
{
	memset(state, 0, sizeof(*state));
	atomic_set(&state->consumed, 0);
}

void mt6797_a72_admission_trigger_state_init(struct mt6797_a72_admission_trigger_state *state)
{
	memset(state, 0, sizeof(*state));
	atomic_set(&state->consumed, 0);
	state->operation_ret = -EINPROGRESS;
}

int mt6797_a72_admission_trigger_run(struct mt6797_a72_admission_trigger_state *state,
				     const struct mt6797_a72_admission_trigger_ops *ops,
				     void *context, const char *buf, size_t count)
{
	int ret;

	if (!state || !ops || !ops->execute || !buf)
		return -EINVAL;
	if (count != sizeof(MT6797_A72_ADMISSION_TRIGGER_TOKEN) - 1 ||
	    memcmp(buf, MT6797_A72_ADMISSION_TRIGGER_TOKEN, count))
		return -EINVAL;
	if (atomic_cmpxchg(&state->consumed, 0, 1))
		return -EALREADY;

	WRITE_ONCE(state->executions, 1);
	ret = ops->execute(context);
	WRITE_ONCE(state->operation_ret, ret);
	/* Publish the terminal result before complete becomes visible. */
	smp_store_release(&state->complete, true);
	return 0;
}

int
mt6797_a72_admission_run(struct mt6797_a72_admission_controller_state *state,
			 const struct mt6797_a72_admission_controller_ops *ops,
			 void *context)
{
	const struct arm64_late_cpu_ready_token *ready;
	enum gemini_admission_trace_zero_result zero_result = 0;
	bool source_registered = false;
	int ret;

	if (!state || !mt6797_a72_admission_ops_valid(ops))
		return -EINVAL;
	if (atomic_read(&state->consumed))
		return -EALREADY;
	state->trace_entry_ret = ops->trace_entry(context);
	if (state->trace_entry_ret && !ops->allow_trace_failure)
		return state->trace_entry_ret;
	if (!ops->binder_ready(context))
		return -EPROBE_DEFER;
	ready = ops->ready_token(context);
	if (!ready)
		return -EAGAIN;
	if (atomic_cmpxchg(&state->consumed, 0, 1))
		return -EALREADY;

	ret = ops->source_register(context);
	if (ret) {
		zero_result = GEMINI_ADMISSION_TRACE_ZERO_SOURCE_REGISTER;
		goto out_terminal;
	}
	source_registered = true;
	ret = ops->derive_cpu8(context, ready, &state->transaction);
	if (ret) {
		zero_result = GEMINI_ADMISSION_TRACE_ZERO_DERIVE;
		goto out_unregister;
	}
	ret = ops->publish_up(context, &state->transaction);
	if (ret) {
		zero_result = GEMINI_ADMISSION_TRACE_ZERO_PUBLISH;
		goto out_unregister;
	}
	state->cpu_requests++;
	ret = ops->add_cpu(context, MT6797_A72_ADMISSION_CPU);

out_unregister:
	if (source_registered)
		ops->source_unregister(context);
out_terminal:
	state->failure_stage = zero_result;
	if (!state->cpu_requests && zero_result) {
		state->trace_ret = ops->trace_zero_request(context, zero_result);
		if (state->trace_ret && !ops->allow_trace_failure)
			ret = state->trace_ret;
	}
	state->operation_ret = ret;
	return ret;
}

static int mt6797_a72_admission_trace_entry(void *context)
{
	(void)context;
	return gemini_admission_trace_entry();
}

static int
mt6797_a72_admission_trace_zero_request(void *context,
					enum gemini_admission_trace_zero_result result)
{
	(void)context;
	return gemini_admission_trace_zero_request(result);
}

static bool mt6797_a72_admission_binder_ready(void *context)
{
	(void)context;
	return mt6797_a72_binder_available();
}

static const struct arm64_late_cpu_ready_token *
mt6797_a72_admission_ready_token(void *context)
{
	(void)context;
	return arm64_get_late_cpu_ready_token();
}

static int mt6797_a72_admission_source_register(void *context)
{
	struct mt6797_a72_admission_controller *controller = context;

	return mt6797_a72_source_register(&controller->source);
}

static void mt6797_a72_admission_source_unregister(void *context)
{
	struct mt6797_a72_admission_controller *controller = context;

	mt6797_a72_source_unregister(&controller->source);
}

static int
mt6797_a72_admission_derive_cpu8(void *context,
				 const struct arm64_late_cpu_ready_token *ready,
				 struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_admission_controller *controller = context;

	return mt6797_a72_membership_derive_cpu8_diagnostic(ready, transaction,
							    &controller->state.derive_stage);
}

static int
mt6797_a72_admission_publish_up(void *context,
				struct mt6797_a72_transaction *transaction)
{
	(void)context;
	return mt6797_a72_membership_publish_up(transaction);
}

static int mt6797_a72_admission_add_cpu(void *context, unsigned int cpu)
{
	(void)context;
	return add_cpu(cpu);
}

static const struct mt6797_a72_admission_controller_ops
mt6797_a72_admission_production_ops = {
	.allow_trace_failure =
		IS_ENABLED(CONFIG_MTK_MT6797_A72_ADMISSION_LIVE_TRIGGER),
	.binder_ready = mt6797_a72_admission_binder_ready,
	.ready_token = mt6797_a72_admission_ready_token,
	.trace_entry = mt6797_a72_admission_trace_entry,
	.trace_zero_request = mt6797_a72_admission_trace_zero_request,
	.source_register = mt6797_a72_admission_source_register,
	.source_unregister = mt6797_a72_admission_source_unregister,
	.derive_cpu8 = mt6797_a72_admission_derive_cpu8,
	.publish_up = mt6797_a72_admission_publish_up,
	.add_cpu = mt6797_a72_admission_add_cpu,
};

static void mt6797_a72_admission_put_device(void *data)
{
	put_device(data);
}

static int
mt6797_a72_admission_resolve(struct device *dev, const char *property,
			     struct device **supplier)
{
	struct platform_device *pdev;
	struct device_node *node;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return -EINVAL;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	if (!device_is_bound(&pdev->dev)) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&pdev->dev);
		return -EINVAL;
	}
	ret = devm_add_action_or_reset(dev, mt6797_a72_admission_put_device,
				       &pdev->dev);
	if (ret)
		return ret;
	*supplier = &pdev->dev;
	return 0;
}

static int mt6797_a72_admission_prepare(struct mt6797_a72_admission_controller *controller)
{
	struct device *bigidvfs;
	struct device *platform;
	struct device *clock;
	struct device *dev = controller->dev;
	int ret;

	ret = mt6797_a72_admission_resolve(dev, "mediatek,binder",
					   &controller->binder);
	if (ret)
		return dev_err_probe(dev, ret, "binder unavailable\n");
	ret = mt6797_a72_admission_resolve(dev, "mediatek,platform-state",
					   &platform);
	if (ret)
		return dev_err_probe(dev, ret, "platform-state unavailable\n");
	ret = mt6797_a72_admission_resolve(dev, "mediatek,clock-backend",
					   &clock);
	if (ret)
		return dev_err_probe(dev, ret, "clock backend unavailable\n");
	ret = mt6797_a72_admission_resolve(dev, "mediatek,bigidvfs-backend",
					   &bigidvfs);
	if (ret)
		return dev_err_probe(dev, ret, "BigiDVFS backend unavailable\n");
	mt6797_a72_source_context_init(&controller->source, platform, clock,
				       bigidvfs);
	return 0;
}

static int mt6797_a72_admission_execute(void *context)
{
	struct mt6797_a72_admission_controller *controller = context;
	int ret;

	ret = mt6797_a72_admission_prepare(controller);
	if (ret)
		return ret;
	return mt6797_a72_admission_run(&controller->state,
					&mt6797_a72_admission_production_ops,
					controller);
}

static const struct mt6797_a72_admission_trigger_ops
mt6797_a72_admission_trigger_production_ops = {
	.execute = mt6797_a72_admission_execute,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	struct mt6797_a72_binder_diagnostic diagnostic;
	bool complete;
	bool consumed;
	const char *trigger_state;
	int diagnostic_ret;
	ssize_t len;

	(void)attr;
	/* Pair with terminal publication in the trigger runner. */
	complete = smp_load_acquire(&controller->trigger.complete);
	consumed = atomic_read(&controller->trigger.consumed);
	if (!consumed)
		trigger_state = "armed";
	else if (!complete)
		trigger_state = "running";
	else
		trigger_state = "terminal";
	diagnostic_ret = mt6797_a72_binder_diagnostic_snapshot(&diagnostic);
	len = sysfs_emit(buf, "%s state=%s ",
			 MT6797_A72_ADMISSION_LIVE_TAG, trigger_state);
	len += sysfs_emit_at(buf, len,
			     "trigger_consumed=%u trigger_executions=%u ",
			     consumed, READ_ONCE(controller->trigger.executions));
	len += sysfs_emit_at(buf, len,
			     "operation_ret=%d core_consumed=%d ",
			     READ_ONCE(controller->trigger.operation_ret),
			     atomic_read(&controller->state.consumed));
	len += sysfs_emit_at(buf, len,
			     "entry_trace_ret=%d terminal_trace_ret=%d ",
			     READ_ONCE(controller->state.trace_entry_ret),
			     READ_ONCE(controller->state.trace_ret));
	len += sysfs_emit_at(buf, len,
			     "failure_stage=%u derive_stage=%u ",
			     READ_ONCE(controller->state.failure_stage),
			     READ_ONCE(controller->state.derive_stage));
	len += sysfs_emit_at(buf, len,
			     "cpu_requests=%u cpu9_requests=0 ",
			     READ_ONCE(controller->state.cpu_requests));
	len += sysfs_emit_at(buf, len, "cpu_off_requests=0 retries=0 ");
	len += sysfs_emit_at(buf, len, "binder_snapshot_ret=%d binder_abi=%u ",
			     diagnostic_ret, diagnostic.abi);
	len += sysfs_emit_at(buf, len, "lifecycle=%u terminal=%u last_stage=%u ",
			     diagnostic.lifecycle, diagnostic.terminal,
			     diagnostic.last_stage);
	len += sysfs_emit_at(buf, len, "stage_errno=%d rollback_errno=%d ",
			     diagnostic.stage_errno, diagnostic.rollback_errno);
	len += sysfs_emit_at(buf, len, "checkpoint_errno=%d attempted=%u ",
			     diagnostic.checkpoint_errno, diagnostic.attempted);
	len += sysfs_emit_at(buf, len, "watchdog_armed=%u p27_owned=%u ",
			     diagnostic.watchdog_armed, diagnostic.p27_owned);
	len += sysfs_emit_at(buf, len, "rollback_mask=0x%x retained_mask=0x%x ",
			     diagnostic.rollback_mask, diagnostic.retained_mask);
	len += sysfs_emit_at(buf, len, "p27a_op=%u p27a_error=%d ",
			     diagnostic.p27_acquire_operation,
			     diagnostic.p27_acquire_error);
	len += sysfs_emit_at(buf, len, "p27a_attempted=0x%x p27a_completed=0x%x ",
			     diagnostic.p27_acquire_attempted,
			     diagnostic.p27_acquire_completed);
	len += sysfs_emit_at(buf, len, "p27a_spm_before=0x%x p27a_spm_after=0x%x ",
			     diagnostic.p27_acquire_spm_before,
			     diagnostic.p27_acquire_spm_after);
	len += sysfs_emit_at(buf, len, "p27a_bpll=0x%x p27a_owned=%u p27a_sealed=%u ",
			     diagnostic.p27_acquire_bpll,
			     diagnostic.p27_acquire_owned,
			     diagnostic.p27_acquire_sealed);
	len += sysfs_emit_at(buf, len, "p27r_op=%u p27r_error=%d ",
			     diagnostic.p27_release_operation,
			     diagnostic.p27_release_error);
	len += sysfs_emit_at(buf, len, "p27r_attempted=0x%x p27r_completed=0x%x ",
			     diagnostic.p27_release_attempted,
			     diagnostic.p27_release_completed);
	len += sysfs_emit_at(buf, len, "p27r_spm_before=0x%x p27r_spm_after=0x%x ",
			     diagnostic.p27_release_spm_before,
			     diagnostic.p27_release_spm_after);
	return len + sysfs_emit_at(buf, len,
				   "p27r_bpll=0x%x p27r_owned=%u p27r_sealed=%u\n",
				   diagnostic.p27_release_bpll,
				   diagnostic.p27_release_owned,
				   diagnostic.p27_release_sealed);
}

static ssize_t trigger_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	int ret;

	(void)attr;
	ret = mt6797_a72_admission_trigger_run(&controller->trigger,
					       &mt6797_a72_admission_trigger_production_ops,
					       controller, buf, count);
	if (ret)
		return ret;
	dev_info(dev, MT6797_A72_ADMISSION_LIVE_TAG
		 " state=terminal operation_ret=%d core_consumed=%d entry_trace_ret=%d terminal_trace_ret=%d failure_stage=%u derive_stage=%u requests=%u/0/0 retries=0\n",
		 READ_ONCE(controller->trigger.operation_ret),
		 atomic_read(&controller->state.consumed),
		 READ_ONCE(controller->state.trace_entry_ret),
		 READ_ONCE(controller->state.trace_ret),
		 READ_ONCE(controller->state.failure_stage),
		 READ_ONCE(controller->state.derive_stage),
		 READ_ONCE(controller->state.cpu_requests));
	return count;
}

static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_WO(trigger);

static struct attribute *mt6797_a72_admission_live_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_trigger.attr,
	NULL,
};

static const struct attribute_group mt6797_a72_admission_live_group = {
	.name = "gemini_admission",
	.attrs = mt6797_a72_admission_live_attrs,
};

static int mt6797_a72_admission_probe(struct platform_device *pdev)
{
	struct mt6797_a72_admission_controller *controller;
	struct device *dev = &pdev->dev;
	int ret;

	controller = devm_kzalloc(dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	controller->dev = dev;
	mt6797_a72_admission_state_init(&controller->state);
	mt6797_a72_admission_trigger_state_init(&controller->trigger);
	platform_set_drvdata(pdev, controller);

	if (IS_ENABLED(CONFIG_MTK_MT6797_A72_ADMISSION_LIVE_TRIGGER)) {
		ret = devm_device_add_group(dev,
					    &mt6797_a72_admission_live_group);
		if (ret)
			return dev_err_probe(dev, ret,
					     "live trigger unavailable\n");
		dev_info(dev, MT6797_A72_ADMISSION_LIVE_TAG
			 " state=armed trigger_consumed=0 trigger_executions=0 "
			 "core_consumed=0 requests=0/0/0 retries=0\n");
		return 0;
	}

	ret = mt6797_a72_admission_execute(controller);
	if (!atomic_read(&controller->state.consumed))
		return dev_err_probe(dev, ret, "admission prerequisite unavailable\n");
	dev_info(dev, MT6797_A72_ADMISSION_TAG
		 " state=terminal ret=%d consumed=1 requests=%u/0/0 retries=0\n",
		 ret, controller->state.cpu_requests);
	return 0;
}

static const struct of_device_id mt6797_a72_admission_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-admission-controller" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_a72_admission_of_match);

static struct platform_driver mt6797_a72_admission_driver = {
	.probe = mt6797_a72_admission_probe,
	.driver = {
		.name = "mt6797-a72-admission-controller",
		.of_match_table = mt6797_a72_admission_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init mt6797_a72_admission_init(void)
{
	return platform_driver_register(&mt6797_a72_admission_driver);
}
late_initcall(mt6797_a72_admission_init);

MODULE_DESCRIPTION("MT6797 candidate-only one-shot CPU8 admission controller");
MODULE_LICENSE("GPL");
