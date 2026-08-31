// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coordinator for one MT6797 CPU8 transition. */

#include <linux/errno.h>
#include <linux/string.h>

#include "mt6797-a72-transition-internal.h"

static bool
mt6797_a72_transition_ops_valid(const struct mt6797_a72_transition_ops *ops)
{
	return ops && ops->checkpoint && ops->watchdog_arm &&
		ops->p27_acquire && ops->p27_release &&
		ops->provider_acquire && ops->provider_release &&
		ops->isolation_clear && ops->sram_enable && ops->cpu_on &&
		ops->secondary_complete && ops->ipi_proof && ops->dcm_update &&
		ops->membership_commit && ops->terminal;
}

static int
mt6797_a72_transition_checkpoint(const struct mt6797_a72_transition_ops *ops,
				 void *context,
				 struct mt6797_a72_transition_result *result,
				 enum mt6797_a72_transition_phase phase,
				 enum mt6797_a72_transition_stage stage)
{
	int ret;

	result->last_stage = stage;
	result->checkpoints++;
	ret = ops->checkpoint(context, phase, stage, result);
	if (ret)
		result->checkpoint_errno = ret;
	return ret;
}

static void
mt6797_a72_transition_set_retained(struct mt6797_a72_transition_result *result)
{
	result->retained_mask = 0;
	if (result->p27_owned)
		result->retained_mask |= MT6797_A72_TRANSITION_OWNED_P27;
	if (result->provider_owned)
		result->retained_mask |= MT6797_A72_TRANSITION_OWNED_PROVIDER;
	if (result->cpu8_online)
		result->retained_mask |= MT6797_A72_TRANSITION_OWNED_CPU8;
}

static int
mt6797_a72_transition_terminal(struct mt6797_a72_transition_controller *controller,
			       const struct mt6797_a72_transition_ops *ops,
			       void *context,
			       struct mt6797_a72_transition_result *result,
			       enum mt6797_a72_transition_terminal terminal,
			       int return_errno, u32 uncertain_mask)
{
	int ret;

	result->terminal = terminal;
	mt6797_a72_transition_set_retained(result);
	result->retained_mask |= uncertain_mask;
	result->terminal_commits++;
	ret = ops->terminal(context, result);
	if (ret) {
		result->checkpoint_errno = ret;
		if (terminal == MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF) {
			result->terminal =
				MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO;
			result->stage_errno = ret;
			return_errno = ret;
		}
	}
	atomic_set_release(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_TERMINAL);
	return return_errno;
}

static int
mt6797_a72_transition_rollback(struct mt6797_a72_transition_controller *controller,
			       const struct mt6797_a72_transition_ops *ops,
			       void *context,
			       struct mt6797_a72_transition_result *result,
			       int stage_errno)
{
	int ret;

	result->stage_errno = stage_errno;
	if (result->provider_owned) {
		ret = ops->provider_release(context);
		if (ret) {
			result->rollback_errno = ret;
			return mt6797_a72_transition_terminal(controller, ops,
				context, result,
				MT6797_A72_TRANSITION_ROLLBACK_FAULT_PREISO,
				ret, 0);
		}
		result->provider_owned = false;
		result->rollback_mask |= MT6797_A72_TRANSITION_OWNED_PROVIDER;
	}
	if (result->p27_owned) {
		ret = ops->p27_release(context);
		if (ret) {
			result->rollback_errno = ret;
			return mt6797_a72_transition_terminal(controller, ops,
				context, result,
				MT6797_A72_TRANSITION_ROLLBACK_FAULT_PREISO,
				ret, 0);
		}
		result->p27_owned = false;
		result->rollback_mask |= MT6797_A72_TRANSITION_OWNED_P27;
	}
	return mt6797_a72_transition_terminal(controller, ops, context,
		result,
		MT6797_A72_TRANSITION_ROLLED_BACK_PREISO, stage_errno, 0);
}

static int
mt6797_a72_owner_fault(struct mt6797_a72_transition_controller *controller,
		       const struct mt6797_a72_transition_ops *ops,
		       void *context,
		       struct mt6797_a72_transition_result *result,
		       u32 unknown_mask)
{
	result->stage_errno = -EPROTO;
	return mt6797_a72_transition_terminal(controller, ops, context,
		result,
		MT6797_A72_TRANSITION_ROLLBACK_FAULT_PREISO, -EPROTO,
		unknown_mask);
}

static int
mt6797_a72_transition_postiso_fault(struct mt6797_a72_transition_controller *controller,
				    const struct mt6797_a72_transition_ops *ops,
				    void *context,
				    struct mt6797_a72_transition_result *result,
				    int stage_errno)
{
	result->stage_errno = stage_errno;
	return mt6797_a72_transition_terminal(controller, ops, context,
		result,
		MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO, stage_errno, 0);
}

static int
mt6797_a72_checkpoint_stage(struct mt6797_a72_transition_controller *controller,
			    const struct mt6797_a72_transition_ops *ops,
			    void *context,
			    struct mt6797_a72_transition_result *result,
			    enum mt6797_a72_transition_phase phase,
			    enum mt6797_a72_transition_stage stage)
{
	int ret;

	ret = mt6797_a72_transition_checkpoint(ops, context, result,
					       phase, stage);
	if (!ret)
		return 0;
	if (stage == MT6797_A72_TRANSITION_STAGE_WATCHDOG &&
	    !result->watchdog_armed) {
		result->stage_errno = ret;
		return mt6797_a72_transition_terminal(controller, ops,
			context, result,
			MT6797_A72_TRANSITION_REJECTED_PRESTATE, ret, 0);
	}
	if (!result->isolation_attempted)
		return mt6797_a72_transition_rollback(controller, ops,
					      context, result, ret);
	return mt6797_a72_transition_postiso_fault(controller, ops,
					     context, result, ret);
}

int mt6797_a72_transition_begin(struct mt6797_a72_transition_controller *controller,
				const struct mt6797_a72_transition_ops *ops,
				void *context,
				const struct mt6797_a72_transition_request *request,
				struct mt6797_a72_transition_result *result)
{
	bool owned = false;
	u64 watchdog_identity = 0;
	int ret;

	if (!result)
		return -EINVAL;
	if (controller &&
	    (atomic_read_acquire(&controller->lifecycle) !=
	     MT6797_A72_TRANSITION_LIFECYCLE_IDLE ||
	     atomic_read_acquire(&controller->consumed)))
		return -EALREADY;
	memset(result, 0, sizeof(*result));
	result->last_stage = MT6797_A72_TRANSITION_STAGE_ENTRY;
	result->terminal = MT6797_A72_TRANSITION_REJECTED_PRESTATE;
	if (!controller || !request || !mt6797_a72_transition_ops_valid(ops))
		return -EINVAL;
	result->cpu8_online = request->cpu8_online;
	result->cpu9_online = request->cpu9_online;
	if (request->cpu != MT6797_A72_TRANSITION_CPU8)
		return -EINVAL;
	if (!request->token_exact || !request->prefix_complete ||
	    request->cpu8_online || request->cpu9_online)
		return -EPERM;
	if (atomic_cmpxchg(&controller->consumed, 0, 1))
		return -EALREADY;
	if (atomic_cmpxchg(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_IDLE,
			   MT6797_A72_TRANSITION_LIFECYCLE_STARTING) !=
	    MT6797_A72_TRANSITION_LIFECYCLE_IDLE)
		return -EALREADY;
	result->attempted = true;
	result->terminal = MT6797_A72_TRANSITION_TERMINAL_NONE;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_WATCHDOG);
	if (ret)
		return ret;
	ret = ops->watchdog_arm(context, MT6797_A72_TRANSITION_RECOVERY_MS,
				&watchdog_identity);
	if (ret) {
		result->stage_errno = ret;
		return mt6797_a72_transition_terminal(controller, ops,
			context, result,
			MT6797_A72_TRANSITION_REJECTED_PRESTATE, ret, 0);
	}
	if (!watchdog_identity) {
		result->stage_errno = -EPROTO;
		return mt6797_a72_transition_terminal(controller, ops,
			context, result,
			MT6797_A72_TRANSITION_REJECTED_PRESTATE, -EPROTO, 0);
	}
	result->watchdog_armed = true;
	result->watchdog_identity = watchdog_identity;
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_WATCHDOG);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_P27);
	if (ret)
		return ret;
	ret = ops->p27_acquire(context, &owned);
	result->p27_owned = owned;
	if (ret)
		return mt6797_a72_transition_rollback(controller, ops, context,
						      result, ret);
	if (!owned)
		return mt6797_a72_owner_fault(controller, ops, context, result,
					      MT6797_A72_TRANSITION_OWNED_P27);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_P27);
	if (ret)
		return ret;

	owned = false;
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_PROVIDER);
	if (ret)
		return ret;
	ret = ops->provider_acquire(context, &owned);
	result->provider_owned = owned;
	if (ret)
		return mt6797_a72_transition_rollback(controller, ops, context,
						      result, ret);
	if (!owned)
		return mt6797_a72_owner_fault(controller, ops, context, result,
					      MT6797_A72_TRANSITION_OWNED_PROVIDER);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_PROVIDER);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_ISOLATION);
	if (ret)
		return ret;
	result->isolation_attempted = true;
	ret = ops->isolation_clear(context);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	result->isolation_crossed = true;
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_ISOLATION);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_SRAM);
	if (ret)
		return ret;
	ret = ops->sram_enable(context);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_SRAM);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_CPU_ON);
	if (ret)
		return ret;
	result->cpu_requests++;
	ret = ops->cpu_on(context, MT6797_A72_TRANSITION_CPU8);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	result->cpu_on_accepted = true;
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_CPU_ON);
	if (ret)
		return ret;
	atomic_set_release(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED);
	return 0;
}

int mt6797_a72_transition_secondary_complete(struct mt6797_a72_transition_controller *controller,
					     const struct mt6797_a72_transition_ops *ops,
					     void *context, unsigned int cpu,
					     bool cpu8_online, bool cpu9_online,
					     struct mt6797_a72_transition_result *result)
{
	int ret;

	if (!controller || !result || !mt6797_a72_transition_ops_valid(ops))
		return -EINVAL;
	if (atomic_cmpxchg(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED,
			   MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_INFLIGHT) !=
	    MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED)
		return -EALREADY;
	result->cpu8_online = cpu8_online;
	result->cpu9_online = cpu9_online;
	if (cpu != MT6797_A72_TRANSITION_CPU8 || !cpu8_online || cpu9_online)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     -EPROTO);

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
	if (ret)
		return ret;
	ret = ops->secondary_complete(context, cpu);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
	if (ret)
		return ret;
	atomic_set_release(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE);
	return 0;
}

int mt6797_a72_transition_complete(struct mt6797_a72_transition_controller *controller,
				   const struct mt6797_a72_transition_ops *ops,
				   void *context, unsigned int cpu,
				   bool cpu8_online, bool cpu9_online,
				   struct mt6797_a72_transition_result *result)
{
	int ret;

	if (!controller || !result || !mt6797_a72_transition_ops_valid(ops))
		return -EINVAL;
	if (atomic_cmpxchg(&controller->lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE,
			   MT6797_A72_TRANSITION_LIFECYCLE_FINAL_INFLIGHT) !=
	    MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE)
		return -EALREADY;
	result->cpu8_online = cpu8_online;
	result->cpu9_online = cpu9_online;
	if (cpu != MT6797_A72_TRANSITION_CPU8 || !cpu8_online || cpu9_online)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     -EPROTO);

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_IPI);
	if (ret)
		return ret;
	ret = ops->ipi_proof(context, cpu);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_IPI);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_DCM);
	if (ret)
		return ret;
	ret = ops->dcm_update(context);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops, context, result,
							     ret);
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_DCM);
	if (ret)
		return ret;

	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_BEFORE,
					  MT6797_A72_TRANSITION_STAGE_MEMBERSHIP);
	if (ret)
		return ret;
	ret = ops->membership_commit(context, MT6797_A72_TRANSITION_CPU8);
	if (ret)
		return mt6797_a72_transition_postiso_fault(controller, ops,
			context, result, ret);
	result->membership_published = true;
	ret = mt6797_a72_checkpoint_stage(controller,
					  ops, context, result,
					  MT6797_A72_TRANSITION_AFTER,
					  MT6797_A72_TRANSITION_STAGE_MEMBERSHIP);
	if (ret)
		return ret;
	return mt6797_a72_transition_terminal(controller, ops, context,
		result,
		MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF, 0, 0);
}

int mt6797_a72_transition_fail(struct mt6797_a72_transition_controller *controller,
			       const struct mt6797_a72_transition_ops *ops,
			       void *context, unsigned int cpu,
			       bool cpu8_online, bool cpu9_online, int error,
			       struct mt6797_a72_transition_result *result)
{
	int lifecycle, ret;

	if (!controller || !result || !error ||
	    !mt6797_a72_transition_ops_valid(ops))
		return -EINVAL;
	lifecycle = atomic_read_acquire(&controller->lifecycle);
	if (lifecycle != MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED &&
	    lifecycle != MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE)
		return -EALREADY;
	if (atomic_cmpxchg(&controller->lifecycle, lifecycle,
			   MT6797_A72_TRANSITION_LIFECYCLE_FINAL_INFLIGHT) !=
	    lifecycle)
		return -EALREADY;
	result->cpu8_online = cpu8_online;
	result->cpu9_online = cpu9_online;
	if (cpu != MT6797_A72_TRANSITION_CPU8 || cpu9_online ||
	    (lifecycle == MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE &&
	     !cpu8_online))
		error = -EPROTO;
	if (lifecycle == MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED) {
		ret = mt6797_a72_checkpoint_stage(controller,
						  ops, context, result,
						  MT6797_A72_TRANSITION_BEFORE,
						  MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
		if (ret)
			return ret;
	}
	return mt6797_a72_transition_postiso_fault(controller, ops, context, result, error);
}

int mt6797_a72_transition_run(struct mt6797_a72_transition_controller *controller,
			      const struct mt6797_a72_transition_ops *ops,
			      void *context,
			      const struct mt6797_a72_transition_request *request,
			      struct mt6797_a72_transition_result *result)
{
	int ret;

	ret = mt6797_a72_transition_begin(controller, ops, context,
					  request, result);
	if (ret)
		return ret;
	ret = mt6797_a72_transition_secondary_complete(controller, ops, context,
						       MT6797_A72_TRANSITION_CPU8,
						       true, false, result);
	if (ret)
		return ret;
	return mt6797_a72_transition_complete(controller, ops, context,
					      MT6797_A72_TRANSITION_CPU8,
					      true, false, result);
}
