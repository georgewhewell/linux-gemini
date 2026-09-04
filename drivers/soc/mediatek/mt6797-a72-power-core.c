// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-independent MT6797 Cortex-A72 pre-CPU_ON transaction model. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "mt6797-a72-power.h"

/*
 * Gemian's MT6797 vendor tree at 51e705e4e7b1, arch/arm64/kernel/psci.c,
 * cpu_power_on_buck(), is the pinned evidence for this order.  CPU_ON and the
 * later MP2 DCM/iDVFS operations are intentionally outside this plan.
 *
 * The vendor write to CPU_EXT_BUCK_ISO clears bits 1:0.  Bit 0 belongs to the
 * little-cluster buck, so the production plan narrows that update to the
 * independently named B_EXT_BUCK_ISO bit 1.  Nothing in the vendor Linux path
 * writes MP2 PWR_ON, isolation, clock-disable, SRAM power-down, or retention;
 * those fields remain firmware-owned and are excluded here.
 */
static const struct mt6797_a72_step mt6797_a72_power_steps[] = {
	{ MT6797_A72_OCP_GUARD_ENTER },
	{
		.action = MT6797_A72_SPM_UPDATE,
		.reg = MT6797_A72_SPM_MP2_PWR_CON,
		.mask = MT6797_A72_MP2_PWR_RST_B,
		.value = MT6797_A72_MP2_PWR_RST_B,
	},
	{ MT6797_A72_IDVFS_READ_FENCE },
	{ MT6797_A72_PWRAP_RESET_ASSERT },
	{ MT6797_A72_VPROC_ENABLE },
	{ MT6797_A72_DELAY_US, .value = 1000 },
	{
		.action = MT6797_A72_SPM_UPDATE,
		.reg = MT6797_A72_SPM_EXT_BUCK_ISO,
		.mask = MT6797_A72_BIG_BUCK_ISO,
	},
	{ MT6797_A72_PWRAP_RESET_DEASSERT },
	{ MT6797_A72_OCP_GUARD_EXIT },
	{ MT6797_A72_DELAY_US, .value = 240 },
	{ MT6797_A72_VSRAM_SET_UV, .value = MT6797_A72_SRAM_BOOT_UV },
	{ MT6797_A72_DELAY_US, .value = 240 },
};

static bool mt6797_a72_steps_equal(const struct mt6797_a72_step *first,
				    const struct mt6797_a72_step *second)
{
	return first->action == second->action && first->reg == second->reg &&
	       first->mask == second->mask && first->value == second->value;
}

static int mt6797_a72_validate_plan(const struct mt6797_a72_plan *plan)
{
	unsigned int i;

	if (!plan || plan->count != ARRAY_SIZE(mt6797_a72_power_steps))
		return -EINVAL;

	for (i = 0; i < plan->count; i++)
		if (!mt6797_a72_steps_equal(&plan->steps[i],
					       &mt6797_a72_power_steps[i]))
			return -EINVAL;

	return 0;
}

int mt6797_a72_build_plan(struct mt6797_a72_plan *plan)
{
	if (!plan)
		return -EINVAL;

	static_assert(ARRAY_SIZE(mt6797_a72_power_steps) <=
		      MT6797_A72_PLAN_MAX_STEPS);
	memset(plan, 0, sizeof(*plan));
	memcpy(plan->steps, mt6797_a72_power_steps,
	       sizeof(mt6797_a72_power_steps));
	plan->count = ARRAY_SIZE(mt6797_a72_power_steps);

	return 0;
}

int mt6797_a72_validate_admission(const struct mt6797_a72_state *state,
				  unsigned long capabilities)
{
	if (!state)
		return -EINVAL;
	if (state->a72_online || state->ocp_guarded)
		return -EBUSY;

	/* A voltage selector is not evidence that the secure LDO is enabled. */
	if (!(capabilities & MT6797_A72_CAP_SRAM_STATUS) ||
	    state->vsram_enabled == -EOPNOTSUPP)
		return -EOPNOTSUPP;
	if (state->vsram_enabled < 0)
		return state->vsram_enabled;
	if (state->vsram_enabled > 1 || state->vproc_enabled < 0 ||
	    state->vproc_enabled > 1 || state->pwrap_reset_asserted < 0 ||
	    state->pwrap_reset_asserted > 1)
		return -EUCLEAN;

	/*
	 * Disable is required even when the observed LDO starts enabled: the
	 * transaction must also be able to restore a changed enable state after
	 * a partial or firmware-handoff failure.
	 */
	if ((capabilities & MT6797_A72_REQUIRED_CAPS) !=
	    MT6797_A72_REQUIRED_CAPS)
		return -EOPNOTSUPP;

	return 0;
}

static bool mt6797_a72_states_equal(const struct mt6797_a72_state *first,
				     const struct mt6797_a72_state *second)
{
	return first->mp2_pwr_con == second->mp2_pwr_con &&
	       first->ext_buck_iso == second->ext_buck_iso &&
	       first->vproc_enabled == second->vproc_enabled &&
	       first->vsram_enabled == second->vsram_enabled &&
	       first->pwrap_reset_asserted == second->pwrap_reset_asserted &&
	       first->vproc_uv == second->vproc_uv &&
	       first->vsram_uv == second->vsram_uv &&
	       first->ocp_guarded == second->ocp_guarded &&
	       first->a72_online == second->a72_online;
}

static int
mt6797_a72_transaction_rollback(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_state observed;
	int ret = 0;
	int step_ret;

	while (transaction->applied) {
		transaction->applied--;
		step_ret = transaction->ops->rollback(
			transaction->context,
			&transaction->plan->steps[transaction->applied],
			&transaction->initial);
		if (step_ret)
			ret = -EUCLEAN;
	}

	step_ret = transaction->ops->snapshot(transaction->context, &observed);
	if (step_ret || !mt6797_a72_states_equal(&transaction->initial,
						 &observed))
		ret = -EUCLEAN;

	transaction->active = false;
	if (ret)
		transaction->faulted = true;

	return ret;
}

int mt6797_a72_transaction_begin(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context,
				  unsigned long capabilities)
{
	const struct mt6797_a72_step *step;
	int rollback_ret;
	int ret;

	if (!transaction || !ops || !ops->snapshot || !ops->apply ||
	    !ops->verify || !ops->rollback)
		return -EINVAL;
	if (transaction->active || transaction->faulted)
		return -EUCLEAN;

	ret = mt6797_a72_validate_plan(plan);
	if (ret)
		return ret;

	memset(transaction, 0, sizeof(*transaction));
	transaction->ops = ops;
	transaction->context = context;
	transaction->plan = plan;

	ret = ops->snapshot(context, &transaction->initial);
	if (ret)
		return ret;
	ret = mt6797_a72_validate_admission(&transaction->initial,
					    capabilities);
	if (ret)
		return ret;

	for (transaction->applied = 0;
	     transaction->applied < plan->count;) {
		step = &plan->steps[transaction->applied];
		/* A failed operation may already have changed hardware. */
		transaction->applied++;
		ret = ops->apply(context, step);
		if (!ret)
			ret = ops->verify(context, step);
		if (ret) {
			rollback_ret = mt6797_a72_transaction_rollback(transaction);
			return rollback_ret ?: ret;
		}
	}

	transaction->active = true;
	return 0;
}

int mt6797_a72_transaction_abort(struct mt6797_a72_transaction *transaction)
{
	if (!transaction || !transaction->active || !transaction->ops ||
	    !transaction->plan)
		return -EINVAL;

	return mt6797_a72_transaction_rollback(transaction);
}
