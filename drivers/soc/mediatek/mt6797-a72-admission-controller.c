// SPDX-License-Identifier: GPL-2.0-only
/* Candidate-only one-shot MT6797 CPU8 admission controller. */

#include <asm/late_cpu_profile.h>
#include <asm/mt6797_a72_membership.h>

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gemini_transition_ledger.h>
#include <linux/init.h>
#include <linux/mt6797-a72-provider.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt6797-a72-binder.h>
#include <linux/soc/mediatek/mt6797-a72-platform-state.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "mt6797-a72-admission-controller-internal.h"
#include "mt6797-a72-physical-source-observer-internal.h"

#define MT6797_A72_ADMISSION_TAG "GEMINI_A72_ADMISSION_V1"
#define MT6797_A72_ADMISSION_LIVE_TAG "GEMINI_A72_ADMISSION_LIVE_V1"
#define MT6797_A72_ADMISSION_CPU 8
#define MT6797_A72_RECOVERY_P27_RESET		0x00010132U
#define MT6797_A72_RECOVERY_P28_ISOLATION	0x00000002U
#define MT6797_A72_RECOVERY_DA9213_VBUCKB	0x46U
/*
 * MP2 CPU0/1 PWR_CON reset state, from the MT6797 register definitions.
 * The vendor CPU-off sequence asserts ISO and CLK_DIS, enables SRAM_CKISO,
 * and clears RST_B, PWR_ON, PWR_ON_2ND, and SRAM_ISOINT_B.
 */
#define MT6797_A72_RECOVERY_MP2_CPU_RESET_MASK	GENMASK(6, 0)
#define MT6797_A72_RECOVERY_MP2_CPU_RESET_VALUE	(BIT(1) | BIT(4) | BIT(5))
#define MT6797_A72_RECOVERY_ISOLATION_MASK	GENMASK(1, 0)
#define MT6797_A72_RECOVERY_BUCKB_ENABLE	BIT(0)
#define MT6797_A72_RECOVERY_BASELINE_PLATFORM_VALID	BIT(0)
#define MT6797_A72_RECOVERY_BASELINE_CPU_POWER_0_OFF	BIT(1)
#define MT6797_A72_RECOVERY_BASELINE_CPU_POWER_1_OFF	BIT(2)
#define MT6797_A72_RECOVERY_BASELINE_P27_RESET		BIT(3)
#define MT6797_A72_RECOVERY_BASELINE_P28_RESET		BIT(4)
#define MT6797_A72_RECOVERY_BASELINE_PWRAP_UNLATCHED	BIT(5)
#define MT6797_A72_RECOVERY_BASELINE_BUCKB_DISABLED	BIT(6)
#define MT6797_A72_RECOVERY_BASELINE_VBUCKB_EXPECTED	BIT(7)
#define MT6797_A72_RECOVERY_BASELINE_ALL			GENMASK(7, 0)

struct mt6797_a72_admission_controller {
	struct device *dev;
	struct device *binder;
	struct mt6797_a72_physical_source_context source;
	struct mt6797_a72_admission_controller_state state;
	struct mt6797_a72_admission_trigger_state trigger;
	/* CPU9 is explicit and cannot reuse CPU8 trace or trigger state. */
	struct mt6797_a72_admission_controller_state cpu9_state;
	struct mt6797_a72_admission_trigger_state cpu9_trigger;
	bool cpu8_success_acked;
	int ledger_prepare_ret;
	struct mutex live_lock;
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

static int mt6797_a72_admission_retained_ledger_refresh(
	struct mt6797_a72_admission_controller *controller,
	struct gemini_transition_ledger_snapshot *snapshot)
{
	int ret;

	ret = gemini_transition_ledger_snapshot(snapshot);
	controller->state.ledger_snapshot_ret = ret;
	if (!ret)
		controller->state.ledger_snapshot = *snapshot;
	else
		controller->state.ledger_snapshot =
			(struct gemini_transition_ledger_snapshot){};
	return ret;
}

static const char *
mt6797_a72_admission_ledger_state(int ret,
	const struct gemini_transition_ledger_snapshot *snapshot)
{
	if (ret == -ENODATA)
		return "empty";
	if (ret)
		return "unreadable";
	return snapshot->phase == GEMINI_TRANSITION_LEDGER_TERMINAL &&
	       snapshot->terminal ? "retained-terminal" : "retained-incomplete";
}

/* Read-only reset proof for retiring a completed prior attempt. */
static int mt6797_a72_admission_reset_baseline(
	struct mt6797_a72_admission_controller *controller)
{
	struct mt6797_a72_platform_state platform;
	struct mt6797_a72_provider_snapshot provider;
	int ret;

	ret = mt6797_a72_admission_prepare(controller);
	if (ret)
		return ret;
	ret = mt6797_a72_platform_state_snapshot(controller->source.platform,
					  &platform);
	if (ret)
		return ret;
	ret = mt6797_a72_provider_snapshot(&provider);
	if (ret)
		return ret;
	controller->state.baseline_p27 = platform.spm_mp2_cpusys_pwr_con;
	controller->state.baseline_p28 = platform.spm_cpu_ext_buck_iso;
	controller->state.baseline_mp2_cpu8 = platform.spm_mp2_cpu0_pwr_con;
	controller->state.baseline_mp2_cpu9 = platform.spm_mp2_cpu1_pwr_con;
	controller->state.baseline_pwr_status = platform.spm_pwr_status;
	controller->state.baseline_pwr_status_2nd = platform.spm_pwr_status_2nd;
	controller->state.baseline_buckb_cont = provider.buckb_cont;
	controller->state.baseline_buckb_vsel = provider.vbuckb_b;
	controller->state.baseline_pwrap_asserted = platform.pwrap_reset_asserted;
	controller->state.baseline_flags = 0;
	if (platform.valid)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_PLATFORM_VALID;
	if ((platform.spm_mp2_cpu0_pwr_con &
	     MT6797_A72_RECOVERY_MP2_CPU_RESET_MASK) ==
	    MT6797_A72_RECOVERY_MP2_CPU_RESET_VALUE)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_CPU_POWER_0_OFF;
	if ((platform.spm_mp2_cpu1_pwr_con &
	     MT6797_A72_RECOVERY_MP2_CPU_RESET_MASK) ==
	    MT6797_A72_RECOVERY_MP2_CPU_RESET_VALUE)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_CPU_POWER_1_OFF;
	if (platform.spm_mp2_cpusys_pwr_con == MT6797_A72_RECOVERY_P27_RESET)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_P27_RESET;
	if ((platform.spm_cpu_ext_buck_iso & MT6797_A72_RECOVERY_ISOLATION_MASK) ==
	    MT6797_A72_RECOVERY_P28_ISOLATION)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_P28_RESET;
	/* Vendor PSCI latches PWRAP only around P27/P28, then unlatches it. */
	if (!platform.pwrap_reset_asserted)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_PWRAP_UNLATCHED;
	if (!(provider.buckb_cont & MT6797_A72_RECOVERY_BUCKB_ENABLE))
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_BUCKB_DISABLED;
	if (provider.vbuckb_b == MT6797_A72_RECOVERY_DA9213_VBUCKB)
		controller->state.baseline_flags |=
			MT6797_A72_RECOVERY_BASELINE_VBUCKB_EXPECTED;
	dev_info(controller->dev, MT6797_A72_ADMISSION_LIVE_TAG
		 " baseline flags=0x%x/%x pwr_status=%08x/%08x mp2_cpu=%08x/%08x p27=%08x p28=%08x pwrap_asserted=%u buckb=%02x vsel=%02x\n",
		 controller->state.baseline_flags, MT6797_A72_RECOVERY_BASELINE_ALL,
		 controller->state.baseline_pwr_status,
		 controller->state.baseline_pwr_status_2nd,
		 controller->state.baseline_mp2_cpu8,
		 controller->state.baseline_mp2_cpu9,
		 controller->state.baseline_p27, controller->state.baseline_p28,
		 platform.pwrap_reset_asserted,
		 controller->state.baseline_buckb_cont,
		 controller->state.baseline_buckb_vsel);
	return controller->state.baseline_flags == MT6797_A72_RECOVERY_BASELINE_ALL ?
		0 : -EUCLEAN;
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
	struct gemini_transition_ledger_snapshot ledger_snapshot;
	bool complete;
	bool consumed;
	const char *ledger_state;
	int ledger_ret;
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
	ledger_ret = mt6797_a72_admission_retained_ledger_refresh(controller,
							     &ledger_snapshot);
	ledger_state = mt6797_a72_admission_ledger_state(ledger_ret,
								 &ledger_snapshot);
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
	len += sysfs_emit_at(buf, len, "ledger_prepare_ret=%d ",
			     READ_ONCE(controller->ledger_prepare_ret));
	len += sysfs_emit_at(buf, len,
			     "ledger_snapshot_ret=%d ledger_state=%s ledger_attempt=%llu ledger_generation=%u ledger_phase=%u ledger_stage=%u ledger_terminal=%u ",
			     ledger_ret, ledger_state,
			     (unsigned long long)ledger_snapshot.attempt_id,
			     ledger_snapshot.generation, ledger_snapshot.phase,
			     ledger_snapshot.stage, ledger_snapshot.terminal);
	len += sysfs_emit_at(buf, len,
			     "ledger_ack_ret=%d ledger_acknowledged=%u archived_attempt=%llu archived_generation=%u archived_phase=%u archived_stage=%u archived_terminal=%u ",
			     READ_ONCE(controller->state.ledger_ack_ret),
			     READ_ONCE(controller->state.ledger_acknowledged),
			     (unsigned long long)READ_ONCE(controller->state.archived_ledger.attempt_id),
			     READ_ONCE(controller->state.archived_ledger.generation),
			     READ_ONCE(controller->state.archived_ledger.phase),
			     READ_ONCE(controller->state.archived_ledger.stage),
			     READ_ONCE(controller->state.archived_ledger.terminal));
	len += sysfs_emit_at(buf, len,
			     "baseline_checked=%u baseline_ret=%d baseline_flags=0x%x/0x%x baseline_pwr_status=0x%x/0x%x baseline_mp2_cpu8=0x%x baseline_mp2_cpu9=0x%x baseline_p27=0x%x baseline_p28=0x%x baseline_pwrap_asserted=%u baseline_buckb_cont=0x%x baseline_buckb_vsel=0x%x ",
			     READ_ONCE(controller->state.baseline_checked),
			     READ_ONCE(controller->state.baseline_ret),
			     READ_ONCE(controller->state.baseline_flags),
			     MT6797_A72_RECOVERY_BASELINE_ALL,
			     READ_ONCE(controller->state.baseline_pwr_status),
			     READ_ONCE(controller->state.baseline_pwr_status_2nd),
			     READ_ONCE(controller->state.baseline_mp2_cpu8),
			     READ_ONCE(controller->state.baseline_mp2_cpu9),
			     READ_ONCE(controller->state.baseline_p27),
			     READ_ONCE(controller->state.baseline_p28),
			     READ_ONCE(controller->state.baseline_pwrap_asserted),
			     READ_ONCE(controller->state.baseline_buckb_cont),
			     READ_ONCE(controller->state.baseline_buckb_vsel));
	len += sysfs_emit_at(buf, len,
			     "trace_prepare_ret=%d trace_entry_words=%08x/%08x/%08x/%08x/%08x/%08x/%08x ",
			     READ_ONCE(controller->state.trace_prepare_ret),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[0]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[1]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[2]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[3]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[4]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[5]),
			     READ_ONCE(controller->state.trace_snapshot.entry_words[6]));
	len += sysfs_emit_at(buf, len,
			     "trace_terminal_words=%08x/%08x/%08x/%08x/%08x/%08x/%08x ",
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[0]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[1]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[2]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[3]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[4]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[5]),
			     READ_ONCE(controller->state.trace_snapshot.terminal_words[6]));
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
	len += sysfs_emit_at(buf, len,
			     "watchdog_armed=%u watchdog_released=%u watchdog_release_ret=%d watchdog_release_identity=%llu p27_owned=%u ",
			     diagnostic.watchdog_armed, diagnostic.watchdog_released,
			     diagnostic.watchdog_release_ret,
			     diagnostic.watchdog_release_identity, diagnostic.p27_owned);
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
	len += sysfs_emit_at(buf, len,
			     "p27r_bpll=0x%x p27r_owned=%u p27r_sealed=%u ",
			     diagnostic.p27_release_bpll,
			     diagnostic.p27_release_owned,
			     diagnostic.p27_release_sealed);
	len += sysfs_emit_at(buf, len,
			     "p28_begin_attempted=%u p28_begin_ret=%d p28_begun=%u ",
			     diagnostic.p28_begin_attempted,
			     diagnostic.p28_begin_ret, diagnostic.p28_begun);
	len += sysfs_emit_at(buf, len,
			     "sram_returned=%u sram_ret=%d sram_match=0x%x ",
			     diagnostic.sram_returned, diagnostic.sram_ret,
			     diagnostic.sram_match_mask);
	len += sysfs_emit_at(buf, len,
			     "sram_required=0x%x p28_complete_attempted=%u ",
			     diagnostic.sram_required_mask,
			     diagnostic.p28_complete_attempted);
	len += sysfs_emit_at(buf, len,
			     "p28_complete_ret=%d sram_abi=%u ",
			     diagnostic.p28_complete_ret, diagnostic.sram_abi);
	len += sysfs_emit_at(buf, len,
			     "sram_attempted=0x%x sram_completed=0x%x sram_mv=%u ",
			     diagnostic.sram_attempted, diagnostic.sram_completed,
			     diagnostic.sram_requested_mv_x100);
	len += sysfs_emit_at(buf, len,
			     "sram_selector_first=0x%x sram_calibration_first=0x%x ",
			     diagnostic.sram_selector_first,
			     diagnostic.sram_calibration_first);
	len += sysfs_emit_at(buf, len,
			     "sram_selector_second=0x%x sram_calibration_second=0x%x ",
			     diagnostic.sram_selector_second,
			     diagnostic.sram_calibration_second);
	len += sysfs_emit_at(buf, len,
			     "sram_attempt_id=%llu sram_cookie=%llu sram_error=%d ",
			     (unsigned long long)diagnostic.sram_attempt_id,
			     (unsigned long long)diagnostic.sram_cookie,
			     diagnostic.sram_error);
	return len + sysfs_emit_at(buf, len,
				   "sram_effect_attempted=%u sram_verified=%u sram_sealed=%u\n",
				   diagnostic.sram_effect_attempted,
				   diagnostic.sram_verified,
				   diagnostic.sram_sealed);
}

static int mt6797_a72_admission_cpu9_execute(void *context)
{
	struct mt6797_a72_admission_controller *controller = context;
	const struct arm64_late_cpu_ready_token *ready;
	bool source_registered = false;
	int ret;

	if (!controller->cpu8_success_acked ||
	    !mt6797_a72_binder_cpu8_success_ready())
		return -EPERM;
	ready = mt6797_a72_admission_ready_token(controller);
	if (!ready)
		return -EAGAIN;
	if (atomic_cmpxchg(&controller->cpu9_state.consumed, 0, 1))
		return -EALREADY;
	ret = mt6797_a72_admission_source_register(controller);
	if (ret)
		goto out;
	source_registered = true;
	ret = mt6797_a72_membership_derive_cpu9_diagnostic(ready,
		&controller->cpu9_state.transaction,
		&controller->cpu9_state.derive_stage);
	if (ret)
		goto out;
	ret = mt6797_a72_admission_publish_up(controller,
		&controller->cpu9_state.transaction);
	if (ret)
		goto out;
	controller->cpu9_state.cpu_requests++;
	ret = mt6797_a72_admission_add_cpu(controller, 9);
out:
	if (source_registered)
		mt6797_a72_admission_source_unregister(controller);
	controller->cpu9_state.operation_ret = ret;
	return ret;
}

static const struct mt6797_a72_admission_trigger_ops
mt6797_a72_admission_cpu9_trigger_ops = {
	.execute = mt6797_a72_admission_cpu9_execute,
};

static ssize_t ack_cpu8_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	struct gemini_transition_ledger_snapshot ledger;
	int ret;

	(void)attr;
	if (count != sizeof(MT6797_A72_ADMISSION_ACK_CPU8_TOKEN) - 1 ||
	    memcmp(buf, MT6797_A72_ADMISSION_ACK_CPU8_TOKEN, count))
		return -EINVAL;
	mutex_lock(&controller->live_lock);
	if (atomic_read(&controller->cpu9_trigger.consumed)) {
		ret = -EALREADY;
		goto out;
	}
	ret = mt6797_a72_admission_retained_ledger_refresh(controller, &ledger);
	if (ret)
		goto out;
	if (ledger.phase != GEMINI_TRANSITION_LEDGER_TERMINAL ||
	    ledger.terminal != MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF ||
	    !mt6797_a72_binder_cpu8_success_ready() ||
	    !cpu_online(8) || cpu_online(9)) {
		ret = -EPERM;
		goto out;
	}
	controller->state.archived_ledger = ledger;
	ret = gemini_transition_ledger_ack_terminal(&ledger);
	controller->state.ledger_ack_ret = ret;
	if (!ret) {
		controller->state.ledger_acknowledged = true;
		controller->cpu8_success_acked = true;
		mt6797_a72_admission_retained_ledger_refresh(controller,
			&controller->state.ledger_snapshot);
	}
out:
	mutex_unlock(&controller->live_lock);
	return ret ? ret : count;
}

static ssize_t cpu9_trigger_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	struct gemini_transition_ledger_snapshot ledger;
	int ret;

	(void)attr;
	mutex_lock(&controller->live_lock);
	if (!controller->cpu8_success_acked ||
	    !mt6797_a72_binder_cpu8_success_ready()) {
		ret = -EPERM;
		goto out;
	}
	ret = mt6797_a72_admission_retained_ledger_refresh(controller, &ledger);
	if (ret != -ENODATA) {
		ret = ret ?: -EUCLEAN;
		goto out;
	}
	ret = mt6797_a72_admission_trigger_run(&controller->cpu9_trigger,
		&mt6797_a72_admission_cpu9_trigger_ops, controller, buf, count);
out:
	mutex_unlock(&controller->live_lock);
	return ret ? ret : count;
}

static ssize_t trigger_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	int ret;

	(void)attr;
	mutex_lock(&controller->live_lock);
	ret = mt6797_a72_admission_retained_ledger_refresh(
		controller, &controller->state.ledger_snapshot);
	if (!ret)
		ret = -EUCLEAN;
	else if (ret == -ENODATA)
		ret = mt6797_a72_admission_trigger_run(&controller->trigger,
			       &mt6797_a72_admission_trigger_production_ops,
			       controller, buf, count);
	mutex_unlock(&controller->live_lock);
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

static ssize_t recover_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct mt6797_a72_admission_controller *controller = dev_get_drvdata(dev);
	struct gemini_transition_ledger_snapshot ledger;
	int ret;

	(void)attr;
	if (count != sizeof(MT6797_A72_ADMISSION_RECOVER_TOKEN) - 1 ||
	    memcmp(buf, MT6797_A72_ADMISSION_RECOVER_TOKEN, count))
		return -EINVAL;
	mutex_lock(&controller->live_lock);
	if (atomic_read(&controller->trigger.consumed)) {
		ret = -EALREADY;
		goto out_unlock;
	}
	ret = mt6797_a72_admission_retained_ledger_refresh(controller, &ledger);
	if (ret)
		goto out_unlock;
	if (ledger.phase != GEMINI_TRANSITION_LEDGER_TERMINAL ||
	    !ledger.terminal) {
		ret = -EINVAL;
		goto out_unlock;
	}
	controller->state.baseline_checked = true;
	controller->state.baseline_ret =
		mt6797_a72_admission_reset_baseline(controller);
	if (controller->state.baseline_ret) {
		ret = controller->state.baseline_ret;
		goto out_unlock;
	}
	controller->state.archived_ledger = ledger;
	controller->state.ledger_ack_ret =
		gemini_transition_ledger_ack_terminal(&ledger);
	ret = controller->state.ledger_ack_ret;
	if (!ret) {
		controller->state.ledger_acknowledged = true;
		mt6797_a72_admission_retained_ledger_refresh(controller,
							     &controller->state.ledger_snapshot);
		dev_info(dev, MT6797_A72_ADMISSION_LIVE_TAG
			 " ledger=acknowledged attempt=%llu generation=%u phase=%u stage=%u terminal=%u baseline=p27:%08x,p28:%08x,buckb:%02x,vsel:%02x\n",
			 (unsigned long long)ledger.attempt_id, ledger.generation,
			 ledger.phase, ledger.stage, ledger.terminal,
			 controller->state.baseline_p27,
			 controller->state.baseline_p28,
			 controller->state.baseline_buckb_cont,
			 controller->state.baseline_buckb_vsel);
	}
out_unlock:
	mutex_unlock(&controller->live_lock);
	return ret ? ret : count;
}

static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_WO(trigger);
static DEVICE_ATTR_WO(recover);
static DEVICE_ATTR_WO(ack_cpu8);
static DEVICE_ATTR_WO(cpu9_trigger);

static struct attribute *mt6797_a72_admission_live_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_trigger.attr,
	&dev_attr_recover.attr,
	&dev_attr_ack_cpu8.attr,
	&dev_attr_cpu9_trigger.attr,
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
	mutex_init(&controller->live_lock);
	mt6797_a72_admission_state_init(&controller->state);
	mt6797_a72_admission_trigger_state_init(&controller->trigger);
	mt6797_a72_admission_state_init(&controller->cpu9_state);
	mt6797_a72_admission_trigger_state_init(&controller->cpu9_trigger);
	/* Private retained pages are prepared before exposing the one-shot ABI. */
	controller->state.trace_prepare_ret =
		gemini_admission_trace_prepare(&controller->state.trace_snapshot);
	controller->ledger_prepare_ret = gemini_transition_ledger_prepare();
	controller->state.ledger_snapshot_ret =
		gemini_transition_ledger_snapshot(&controller->state.ledger_snapshot);
	platform_set_drvdata(pdev, controller);

	if (IS_ENABLED(CONFIG_MTK_MT6797_A72_ADMISSION_LIVE_TRIGGER)) {
		if (controller->state.trace_prepare_ret ||
		    controller->ledger_prepare_ret)
			return dev_err_probe(dev,
				controller->state.trace_prepare_ret ?: controller->ledger_prepare_ret,
				"private retained pages unavailable\n");
		ret = devm_device_add_group(dev,
					    &mt6797_a72_admission_live_group);
		if (ret)
			return dev_err_probe(dev, ret,
					     "live trigger unavailable\n");
		dev_info(dev, MT6797_A72_ADMISSION_LIVE_TAG
			 " state=armed trigger_consumed=0 trigger_executions=0 "
			 "core_consumed=0 requests=0/0/0 retries=0 trace_node=444f0000/1000 trace_prepare_ret=%d ledger_node=444f1000/1000 ledger_prepare_ret=%d "
			 "entry=%08x/%08x/%08x/%08x/%08x/%08x/%08x "
			 "terminal=%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
			 controller->state.trace_prepare_ret, controller->ledger_prepare_ret,
			 controller->state.trace_snapshot.entry_words[0],
			 controller->state.trace_snapshot.entry_words[1],
			 controller->state.trace_snapshot.entry_words[2],
			 controller->state.trace_snapshot.entry_words[3],
			 controller->state.trace_snapshot.entry_words[4],
			 controller->state.trace_snapshot.entry_words[5],
			 controller->state.trace_snapshot.entry_words[6],
			 controller->state.trace_snapshot.terminal_words[0],
			 controller->state.trace_snapshot.terminal_words[1],
			 controller->state.trace_snapshot.terminal_words[2],
			 controller->state.trace_snapshot.terminal_words[3],
			 controller->state.trace_snapshot.terminal_words[4],
			 controller->state.trace_snapshot.terminal_words[5],
			 controller->state.trace_snapshot.terminal_words[6]);
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
