// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-independent MT6797 Cortex-A72 pre-CPU_ON transaction model. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "mt6797-a72-power.h"

/*
 * Vendor psci.c cpu_psci_cpu_kill(): on the last MP2 core, stop sync DCM,
 * let AFFINITY_INFO finish firmware power-off, then disable DA9214 BUCK B.
 * BigiDVFSSRAMLDODisable() in that source is a no-op, not another MMIO step.
 * ATF power_off_big()/power_off_cl3() poll CPU_PWR_STATUS bits 7/6/17.
 * CPU_PWR_STATUS_2ND does not mirror these MP2 acknowledgments: on Gemini
 * its core bits remain set after shutdown and its cluster bit is clear even
 * while MP2 runs. Keep it for diagnostics only. Primary acknowledgments and
 * external isolation must confirm shutdown before removing the rail. We do not
 * duplicate the firmware's MTCMOS, snoop, SCU, PLL or isolation writes.
 */
static bool mt6797_a72_off_sample_valid(const struct mt6797_a72_off_sample *s,
				      unsigned int cpu, bool rail_off)
{
	u32 mask = MT6797_A72_CORE0_POWER_ACK | MT6797_A72_CORE1_POWER_ACK |
		   MT6797_A72_CLUSTER_POWER_ACK;
	u32 expected = cpu == 9 ? MT6797_A72_CORE0_POWER_ACK |
				 MT6797_A72_CLUSTER_POWER_ACK : 0;

	if (s->online_mask != (cpu == 9 ? BIT(0) : 0) ||
	    (s->power_status & mask) != expected ||
	    (s->idvfs_control & BIT(0)) ||
	    s->vproc_enabled != !rail_off)
		return false;
	if (cpu == 9)
		return !rail_off && !(s->ext_buck_iso & MT6797_A72_BIG_BUCK_ISO) &&
		       (s->mp2_pwr_con & MT6797_A72_MP2_PWR_RST_B) &&
		       !(s->mp2_pwr_con & BIT(4));
	return (s->ext_buck_iso & MT6797_A72_BIG_BUCK_ISO) &&
	       !(s->mp2_pwr_con & MT6797_A72_MP2_PWR_RST_B) &&
	       (s->mp2_pwr_con & BIT(4));
}

int mt6797_a72_check_cluster_off(const struct mt6797_a72_off_state *off,
				 const struct mt6797_a72_off_sample *sample)
{
	if (!off || !sample)
		return -EINVAL;
	if (!off->completed || !off->cluster_off || off->faulted || off->error ||
	    !mt6797_a72_off_sample_valid(&off->final, 8, true))
		return -EUCLEAN;
	return mt6797_a72_off_sample_valid(sample, 8, true) ? 0 : -EBUSY;
}

int mt6797_a72_finish_cpu_off(struct mt6797_a72_off_state *state,
			     const struct mt6797_a72_off_io *io,
			     void *context, unsigned int cpu)
{
	struct mt6797_a72_off_sample sample;
	unsigned int i;
	int ret;

	if (!state || !io || !io->sample || !io->sync_dcm_off ||
	    !io->firmware_kill || !io->vproc_disable || !io->fence ||
	    (cpu != 8 && cpu != 9))
		return -EINVAL;
	if (state->faulted || state->completed)
		return -EUCLEAN;

	/* Refuse an effectful firmware query while either target may be online. */
	ret = io->sample(context, &sample);
	if (ret)
		goto fault;
	if (sample.online_mask != (cpu == 9 ? BIT(0) : 0) ||
	    sample.vproc_enabled != 1 || (sample.idvfs_control & BIT(0))) {
		ret = -EBUSY;
		goto fault;
	}
	if (cpu == 8) {
		ret = io->sync_dcm_off(context);
		if (ret)
			goto fault;
	}
	ret = io->firmware_kill(context, cpu);
	if (ret)
		goto fault;
	for (i = 0; i < 2; i++) {
		ret = io->sample(context, &sample);
		if (ret)
			goto fault;
		if (!mt6797_a72_off_sample_valid(&sample, cpu, false)) {
			ret = -EIO;
			goto fault;
		}
	}
	if (cpu == 8) {
		ret = io->vproc_disable(context);
		if (ret)
			goto fault;
		for (i = 0; i < 2; i++) {
			ret = io->sample(context, &sample);
			if (ret)
				goto fault;
			if (!mt6797_a72_off_sample_valid(&sample, cpu, true)) {
				ret = -EIO;
				goto fault;
			}
		}
	}
	state->final = sample;
	state->cluster_off = cpu == 8;
	state->completed = true;
	state->error = 0;
	return 0;
fault:
	/* The target already left Linux. Never attempt to restart or undo this. */
	state->faulted = true;
	state->error = ret;
	io->fence(context);
	return -EUCLEAN;
}

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
	{ .action = MT6797_A72_TRANSACTION_GUARD_ENTER },
	{
		.action = MT6797_A72_SPM_UPDATE,
		.reg = MT6797_A72_SPM_MP2_PWR_CON,
		.mask = MT6797_A72_MP2_PWR_RST_B,
		.value = MT6797_A72_MP2_PWR_RST_B,
	},
	{ .action = MT6797_A72_IDVFS_READ_FENCE },
	{ .action = MT6797_A72_PWRAP_RESET_ASSERT },
	{ .action = MT6797_A72_VPROC_ENABLE },
	{ MT6797_A72_DELAY_US, .value = 1000 },
	{
		.action = MT6797_A72_SPM_UPDATE,
		.reg = MT6797_A72_SPM_EXT_BUCK_ISO,
		.mask = MT6797_A72_BIG_BUCK_ISO,
	},
	{ .action = MT6797_A72_PWRAP_RESET_DEASSERT },
	{ .action = MT6797_A72_TRANSACTION_GUARD_EXIT },
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

static int mt6797_a72_validate_base(const struct mt6797_a72_state *state)
{
	if (!state)
		return -EINVAL;
	if (state->a72_online || state->transaction_guarded)
		return -EBUSY;

	if (state->vproc_enabled < 0 || state->vproc_enabled > 1 ||
	    state->pwrap_reset_asserted < 0 ||
	    state->pwrap_reset_asserted > 1)
		return -EUCLEAN;
	if (!(state->ext_buck_iso & MT6797_A72_BIG_BUCK_ISO))
		return -EBUSY;
	if (state->pwrap_reset_asserted)
		return -EBUSY;
	if (state->vproc_uv < MT6797_A72_VPROC_MIN_UV ||
	    state->vproc_uv > MT6797_A72_VPROC_MAX_UV ||
	    state->vsram_uv < MT6797_A72_SRAM_MIN_UV ||
	    state->vsram_uv > MT6797_A72_SRAM_MAX_UV)
		return -ERANGE;
	return 0;
}

int mt6797_a72_validate_admission(const struct mt6797_a72_state *state,
				  unsigned long capabilities)
{
	int ret = mt6797_a72_validate_base(state);

	if (ret)
		return ret;
	/* First-start only: full cold sample, not equivalent nominal voltage. */
	if (state->vsram_selector || state->vsram_control || state->vsram_calibration)
		return -EBUSY;
	if (state->vsram_uv != 1050000)
		return -EUCLEAN;

	if ((capabilities & MT6797_A72_REQUIRED_CAPS) !=
	    MT6797_A72_REQUIRED_CAPS)
		return -EOPNOTSUPP;

	return 0;
}

static int mt6797_a72_validate_restart(const struct mt6797_a72_state *state,
				     const struct mt6797_a72_off_state *off,
				     const struct mt6797_a72_state *stopped,
				     unsigned long capabilities)
{
	bool cold, retained;
	int ret;

	if (!off || !stopped)
		return -EINVAL;
	if (!off->completed || !off->cluster_off || off->faulted || off->error ||
	    !mt6797_a72_off_sample_valid(&off->final, 8, true))
		return -EUCLEAN;
	ret = mt6797_a72_validate_base(state);
	if (ret)
		return ret;
	ret = mt6797_a72_validate_base(stopped);
	if (ret)
		return ret;
	if (state->vproc_enabled || stopped->vproc_enabled ||
	    state->mp2_pwr_con != off->final.mp2_pwr_con ||
	    stopped->mp2_pwr_con != off->final.mp2_pwr_con ||
	    state->vproc_uv != stopped->vproc_uv ||
	    state->vsram_uv != stopped->vsram_uv ||
	    state->vsram_selector != stopped->vsram_selector ||
	    state->vsram_control != stopped->vsram_control ||
	    state->vsram_calibration != stopped->vsram_calibration)
		return -EBUSY;
	cold = !state->vsram_selector && !state->vsram_control &&
	       !state->vsram_calibration && state->vsram_uv == 1050000;
	retained = state->vsram_selector == 11 && state->vsram_uv == 1100000 &&
		   (state->vsram_control & 0xfff) == 0x8fb &&
		   state->vsram_calibration && !(state->vsram_calibration & 0xffff0000);
	if (!cold && !retained)
		return -EUCLEAN;
	if ((capabilities & MT6797_A72_REQUIRED_CAPS) != MT6797_A72_REQUIRED_CAPS)
		return -EOPNOTSUPP;
	return 0;
}

int mt6797_a72_validate_cpu8_boot(unsigned int cpu, bool cpu4_online,
				  bool transaction_armed, bool faulted)
{
	if (cpu != 8)
		return -EPERM;
	if (faulted)
		return -EUCLEAN;
	if (!cpu4_online || !transaction_armed)
		return -EAGAIN;

	return 0;
}

/*
 * Vendor psci.c:617-621 guards the buck sequence with "if (!g_cl2_online)":
 * the second big core only needs PSCI CPU_ON once the first one is running.
 * CPU8 online is the only accepted proof that BUCK B, B_EXT_BUCK_ISO and the
 * SRAM LDO were committed by this owner; a faulted owner admits nothing.
 */
int mt6797_a72_validate_cpu9_boot(unsigned int cpu, bool cpu8_online,
				  bool faulted)
{
	if (cpu != 9)
		return -EPERM;
	if (faulted)
		return -EUCLEAN;
	if (!cpu8_online)
		return -EAGAIN;

	return 0;
}

int mt6797_a72_validate_cpu8_identity(u32 midr)
{
	return midr == MT6797_A72_R0P1_MIDR ? 0 : -ENODEV;
}

int mt6797_a72_validate_removal(bool online, bool active, bool armed, bool guarded, bool faulted)
{
	if (faulted)
		return -EUCLEAN;
	if (online || active || armed || guarded)
		return -EBUSY;

	return 0;
}

static bool mt6797_a72_states_equal(const struct mt6797_a72_state *first,
				     const struct mt6797_a72_state *second)
{
	return first->mp2_pwr_con == second->mp2_pwr_con &&
	       first->ext_buck_iso == second->ext_buck_iso &&
	       first->vproc_enabled == second->vproc_enabled &&
	       first->pwrap_reset_asserted == second->pwrap_reset_asserted &&
	       first->vproc_uv == second->vproc_uv &&
	       first->vsram_uv == second->vsram_uv &&
	       first->vsram_selector == second->vsram_selector &&
	       first->vsram_control == second->vsram_control &&
	       first->vsram_calibration == second->vsram_calibration &&
	       first->transaction_guarded == second->transaction_guarded &&
	       first->a72_online == second->a72_online;
}

/* There is no proven electrical inverse after attempting isolation clear. */
static int
mt6797_a72_transaction_retain_fault(struct mt6797_a72_transaction *transaction)
{
	transaction->faulted = true;
	transaction->active = false;
	transaction->ops->retain_fault(transaction->context);
	return -EUCLEAN;
}

static int
mt6797_a72_transaction_rollback(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_state observed;
	int ret = 0;
	int step_ret;

	if (transaction->isolation_attempted)
		return mt6797_a72_transaction_retain_fault(transaction);

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

static int mt6797_a72_transaction_start(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context, unsigned long capabilities,
				  const struct mt6797_a72_off_state *off,
				  const struct mt6797_a72_state *stopped)
{
	const struct mt6797_a72_step *step;
	int rollback_ret;
	int ret;

	if (!transaction || !ops || !ops->snapshot || !ops->apply ||
	    !ops->verify || !ops->rollback || !ops->retain_fault)
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
	if (off || stopped)
		ret = mt6797_a72_validate_restart(&transaction->initial,
						 off, stopped, capabilities);
	else
		ret = mt6797_a72_validate_admission(&transaction->initial,
						   capabilities);
	if (ret)
		return ret;

	for (transaction->applied = 0;
	     transaction->applied < plan->count;) {
		step = &plan->steps[transaction->applied];
		if (step->action == MT6797_A72_SPM_UPDATE &&
		    step->reg == MT6797_A72_SPM_EXT_BUCK_ISO)
			transaction->isolation_attempted = true;
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

int mt6797_a72_transaction_begin(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context,
				  unsigned long capabilities)
{
	return mt6797_a72_transaction_start(transaction, plan, ops, context,
					    capabilities, NULL, NULL);
}

int mt6797_a72_transaction_restart(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context, unsigned long capabilities,
				  const struct mt6797_a72_off_state *off,
				  const struct mt6797_a72_state *stopped)
{
	if (!off || !stopped)
		return -EINVAL;
	return mt6797_a72_transaction_start(transaction, plan, ops, context,
					    capabilities, off, stopped);
}

int mt6797_a72_transaction_abort(struct mt6797_a72_transaction *transaction)
{
	if (!transaction || !transaction->active || !transaction->ops ||
	    !transaction->plan)
		return -EINVAL;

	return mt6797_a72_transaction_rollback(transaction);
}

int mt6797_a72_transaction_commit(struct mt6797_a72_transaction *transaction)
{
	if (!transaction || !transaction->active || !transaction->ops ||
	    !transaction->plan)
		return -EINVAL;

	transaction->active = false;
	transaction->applied = 0;
	return 0;
}
