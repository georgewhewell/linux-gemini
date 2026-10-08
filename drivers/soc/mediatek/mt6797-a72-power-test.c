// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 Cortex-A72 pre-CPU_ON transaction. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "mt6797-a72-power.h"

struct mt6797_a72_fake {
	struct mt6797_a72_state state;
	enum mt6797_a72_action applied[MT6797_A72_PLAN_MAX_STEPS];
	enum mt6797_a72_action rolled_back[MT6797_A72_PLAN_MAX_STEPS];
	unsigned int apply_calls;
	unsigned int verify_calls;
	unsigned int rollback_calls;
	unsigned int retain_calls;
	bool fail_before_effect;
	struct mt6797_a72_transaction *transaction;
	bool isolation_latched_at_dispatch;
	int fail_apply_at;
	int fail_verify_at;
	int fail_rollback_at;
};

static struct mt6797_a72_fake mt6797_a72_new_fake(void)
{
	return (struct mt6797_a72_fake) {
		.state = {
			.mp2_pwr_con = BIT(1) | BIT(4) | BIT(16),
			.ext_buck_iso = BIT(0) | BIT(1),
			.vproc_enabled = 0,
			.pwrap_reset_asserted = 0,
			.vproc_uv = 900000,
			.vsram_uv = 1050000,
			.vsram_selector = 0,
		},
		.fail_apply_at = -1,
		.fail_verify_at = -1,
		.fail_rollback_at = -1,
	};
}

static struct mt6797_a72_fake mt6797_a72_stopped_fake(bool retained)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();

	if (retained) {
		fake.state.vsram_uv = 1100000;
		fake.state.vsram_selector = 11;
		fake.state.vsram_control = 0x4008fb;
		fake.state.vsram_calibration = 0x8967;
	}
	return fake;
}

static struct mt6797_a72_off_state
mt6797_a72_test_off_record(const struct mt6797_a72_state *state)
{
	return (struct mt6797_a72_off_state) {
		.completed = true,
		.cluster_off = true,
		.final.power_status_second = 0x003dc0ff,
		.final.mp2_pwr_con = state->mp2_pwr_con,
		.final.ext_buck_iso = state->ext_buck_iso,
	};
}

static int mt6797_a72_fake_snapshot(void *context,
				     struct mt6797_a72_state *state)
{
	struct mt6797_a72_fake *fake = context;

	*state = fake->state;
	return 0;
}

static void mt6797_a72_fake_apply_step(struct mt6797_a72_fake *fake,
				       const struct mt6797_a72_step *step)
{
	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		fake->state.transaction_guarded = true;
		break;
	case MT6797_A72_SPM_UPDATE:
		if (step->reg == MT6797_A72_SPM_MP2_PWR_CON)
			fake->state.mp2_pwr_con =
				(fake->state.mp2_pwr_con & ~step->mask) |
				(step->value & step->mask);
		else
			fake->state.ext_buck_iso =
				(fake->state.ext_buck_iso & ~step->mask) |
				(step->value & step->mask);
		break;
	case MT6797_A72_PWRAP_RESET_ASSERT:
		fake->state.pwrap_reset_asserted = 1;
		break;
	case MT6797_A72_VPROC_ENABLE:
		fake->state.vproc_enabled = 1;
		break;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		fake->state.pwrap_reset_asserted = 0;
		break;
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		fake->state.transaction_guarded = false;
		break;
	case MT6797_A72_VSRAM_SET_UV:
		fake->state.vsram_uv = step->value;
		fake->state.vsram_selector = 11;
		fake->state.vsram_control = 0x4008fb;
		fake->state.vsram_calibration = 0x8967;
		break;
	case MT6797_A72_IDVFS_READ_FENCE:
	case MT6797_A72_DELAY_US:
	case MT6797_A72_NR_ACTIONS:
		break;
	}
}

static int mt6797_a72_fake_apply(void *context,
				  const struct mt6797_a72_step *step)
{
	struct mt6797_a72_fake *fake = context;
	unsigned int call = fake->apply_calls++;

	fake->applied[call] = step->action;
	if (fake->transaction && step->action == MT6797_A72_SPM_UPDATE &&
	    step->reg == MT6797_A72_SPM_EXT_BUCK_ISO)
		fake->isolation_latched_at_dispatch = fake->transaction->isolation_attempted;
	if (call == fake->fail_apply_at && fake->fail_before_effect)
		return -EIO;
	mt6797_a72_fake_apply_step(fake, step);

	return call == fake->fail_apply_at ? -EIO : 0;
}

static int mt6797_a72_fake_verify(void *context,
				   const struct mt6797_a72_step *step)
{
	struct mt6797_a72_fake *fake = context;
	unsigned int call = fake->verify_calls++;

	if (call == fake->fail_verify_at)
		return -EIO;

	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		return fake->state.transaction_guarded ? 0 : -EIO;
	case MT6797_A72_SPM_UPDATE:
		if (step->reg == MT6797_A72_SPM_MP2_PWR_CON)
			return (fake->state.mp2_pwr_con & step->mask) ==
			       step->value ? 0 : -EIO;
		return (fake->state.ext_buck_iso & step->mask) ==
		       step->value ? 0 : -EIO;
	case MT6797_A72_PWRAP_RESET_ASSERT:
		return fake->state.pwrap_reset_asserted == 1 ? 0 : -EIO;
	case MT6797_A72_VPROC_ENABLE:
		return fake->state.vproc_enabled == 1 ? 0 : -EIO;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		return fake->state.pwrap_reset_asserted == 0 ? 0 : -EIO;
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		return !fake->state.transaction_guarded ? 0 : -EIO;
	case MT6797_A72_VSRAM_SET_UV:
		return fake->state.vsram_uv == step->value &&
		       fake->state.vsram_selector == 11 ? 0 : -EIO;
	case MT6797_A72_IDVFS_READ_FENCE:
	case MT6797_A72_DELAY_US:
		return 0;
	case MT6797_A72_NR_ACTIONS:
		return -EINVAL;
	}

	return -EINVAL;
}

static int
mt6797_a72_fake_rollback(void *context, const struct mt6797_a72_step *step,
			 const struct mt6797_a72_state *initial)
{
	struct mt6797_a72_fake *fake = context;
	unsigned int call = fake->rollback_calls++;

	fake->rolled_back[call] = step->action;
	if (call == fake->fail_rollback_at)
		return -EIO;

	switch (step->action) {
	case MT6797_A72_TRANSACTION_GUARD_ENTER:
		fake->state.transaction_guarded = initial->transaction_guarded;
		break;
	case MT6797_A72_TRANSACTION_GUARD_EXIT:
		/* Undoing guard exit protects the remaining rollback. */
		fake->state.transaction_guarded = true;
		break;
	case MT6797_A72_SPM_UPDATE:
		if (step->reg == MT6797_A72_SPM_MP2_PWR_CON)
			fake->state.mp2_pwr_con = initial->mp2_pwr_con;
		else
			fake->state.ext_buck_iso = initial->ext_buck_iso;
		break;
	case MT6797_A72_PWRAP_RESET_ASSERT:
		fake->state.pwrap_reset_asserted =
			initial->pwrap_reset_asserted;
		break;
	case MT6797_A72_PWRAP_RESET_DEASSERT:
		/* Restore isolation while the PWRAP interface is held reset. */
		fake->state.pwrap_reset_asserted = 1;
		break;
	case MT6797_A72_VPROC_ENABLE:
		fake->state.vproc_enabled = initial->vproc_enabled;
		fake->state.vproc_uv = initial->vproc_uv;
		break;
	case MT6797_A72_VSRAM_SET_UV:
		fake->state.vsram_uv = initial->vsram_uv;
		fake->state.vsram_selector = initial->vsram_selector;
		break;
	case MT6797_A72_IDVFS_READ_FENCE:
	case MT6797_A72_DELAY_US:
	case MT6797_A72_NR_ACTIONS:
		break;
	}

	return 0;
}

static void mt6797_a72_fake_retain_fault(void *context)
{
	struct mt6797_a72_fake *fake = context;

	fake->retain_calls++;
	fake->state.transaction_guarded = false;
}

static const struct mt6797_a72_backend_ops mt6797_a72_fake_ops = {
	.snapshot = mt6797_a72_fake_snapshot,
	.apply = mt6797_a72_fake_apply,
	.verify = mt6797_a72_fake_verify,
	.rollback = mt6797_a72_fake_rollback,
	.retain_fault = mt6797_a72_fake_retain_fault,
};

static void mt6797_a72_plan_matches_vendor_boundary_test(struct kunit *test)
{
	struct mt6797_a72_plan plan;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_ASSERT_EQ(test, plan.count, 12U);
	KUNIT_EXPECT_EQ(test, plan.steps[0].action,
			(enum mt6797_a72_action)MT6797_A72_TRANSACTION_GUARD_ENTER);
	KUNIT_EXPECT_EQ(test, plan.steps[1].reg,
			(u32)MT6797_A72_SPM_MP2_PWR_CON);
	KUNIT_EXPECT_EQ(test, plan.steps[1].mask,
			(u32)MT6797_A72_MP2_PWR_RST_B);
	KUNIT_EXPECT_EQ(test, plan.steps[5].value, 1000U);
	KUNIT_EXPECT_EQ(test, plan.steps[6].reg,
			(u32)MT6797_A72_SPM_EXT_BUCK_ISO);
	KUNIT_EXPECT_EQ(test, plan.steps[6].mask,
			(u32)MT6797_A72_BIG_BUCK_ISO);
	KUNIT_EXPECT_EQ(test, plan.steps[6].value, 0U);
	KUNIT_EXPECT_EQ(test, plan.steps[10].value,
			(u32)MT6797_A72_SRAM_BOOT_UV);

	for (i = 0; i < plan.count; i++) {
		if (plan.steps[i].reg != MT6797_A72_SPM_MP2_PWR_CON)
			continue;
		KUNIT_EXPECT_EQ(test,
				plan.steps[i].mask & MT6797_A72_MP2_FW_OWNED_MASK,
				0U);
	}
}

static void mt6797_a72_admission_requires_live_backends_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	int ret;

	ret = mt6797_a72_validate_admission(&fake.state, 0);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	ret = mt6797_a72_validate_admission(&fake.state,
					    MT6797_A72_REQUIRED_CAPS);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void mt6797_a72_admission_fences_online_cpu_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();

	fake.state.a72_online = true;
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_validate_admission(&fake.state,
					      MT6797_A72_REQUIRED_CAPS),
		-EBUSY);
}

static void mt6797_a72_admission_requires_full_cold_sram_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();

	KUNIT_EXPECT_EQ(test, mt6797_a72_validate_admission(&fake.state,
		MT6797_A72_REQUIRED_CAPS), 0);
	fake.state.vsram_control = BIT(8);
	KUNIT_EXPECT_EQ(test, mt6797_a72_validate_admission(&fake.state,
		MT6797_A72_REQUIRED_CAPS), -EBUSY);
	fake.state.vsram_control = 0;
	fake.state.vsram_calibration = 1;
	KUNIT_EXPECT_EQ(test, mt6797_a72_validate_admission(&fake.state,
		MT6797_A72_REQUIRED_CAPS), -EBUSY);
	fake.state.vsram_calibration = 0;
	fake.state.vsram_selector = 9;
	KUNIT_EXPECT_EQ(test, mt6797_a72_validate_admission(&fake.state,
		MT6797_A72_REQUIRED_CAPS), -EBUSY);
	fake.state.vsram_selector = 0;
	fake.state.vsram_uv = 900000;
	KUNIT_EXPECT_EQ(test, mt6797_a72_validate_admission(&fake.state,
		MT6797_A72_REQUIRED_CAPS), -EUCLEAN);
}

static void mt6797_a72_cpu8_boot_admission_test(struct kunit *test)
{
	int ret;

	ret = mt6797_a72_validate_cpu8_boot(8, true, true, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_validate_cpu8_boot(9, true, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	ret = mt6797_a72_validate_cpu8_boot(8, false, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_a72_validate_cpu8_boot(8, true, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_a72_validate_cpu8_boot(8, true, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
}

static void mt6797_a72_cpu9_boot_admission_test(struct kunit *test)
{
	int ret;

	ret = mt6797_a72_validate_cpu9_boot(9, true, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_validate_cpu9_boot(8, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	ret = mt6797_a72_validate_cpu9_boot(10, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	ret = mt6797_a72_validate_cpu9_boot(9, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_a72_validate_cpu9_boot(9, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	/* A faulted owner is terminal even with CPU8 online. */
	ret = mt6797_a72_validate_cpu9_boot(9, false, true);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
}

static void mt6797_a72_cpu8_identity_test(struct kunit *test)
{
	int ret;

	ret = mt6797_a72_validate_cpu8_identity(MT6797_A72_R0P1_MIDR);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_validate_cpu8_identity(0x410fd080);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
	ret = mt6797_a72_validate_cpu8_identity(0x411fd081);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
}

static void mt6797_a72_removal_requires_quiescent_owner_test(struct kunit *test)
{
	int ret;

	ret = mt6797_a72_validate_removal(false, false, false, false, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_validate_removal(true, false, false, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	ret = mt6797_a72_validate_removal(false, true, false, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	ret = mt6797_a72_validate_removal(false, false, true, false, false);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	ret = mt6797_a72_validate_removal(false, false, false, true, false);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	ret = mt6797_a72_validate_removal(false, false, false, false, true);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
}

static void mt6797_a72_transaction_orders_and_aborts_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_state powered;
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_ASSERT_EQ(test, mt6797_a72_transaction_begin(&transaction, &plan,
		&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS), 0);
	KUNIT_EXPECT_TRUE(test, transaction.active);
	KUNIT_EXPECT_TRUE(test, transaction.isolation_attempted);
	KUNIT_EXPECT_EQ(test, fake.apply_calls, plan.count);
	KUNIT_EXPECT_EQ(test, fake.applied[2],
		(enum mt6797_a72_action)MT6797_A72_IDVFS_READ_FENCE);
	KUNIT_EXPECT_EQ(test, fake.applied[7],
		(enum mt6797_a72_action)MT6797_A72_PWRAP_RESET_DEASSERT);
	powered = fake.state;
	KUNIT_ASSERT_EQ(test, mt6797_a72_transaction_abort(&transaction), -EUCLEAN);
	KUNIT_EXPECT_FALSE(test, transaction.active);
	KUNIT_EXPECT_TRUE(test, transaction.faulted);
	KUNIT_EXPECT_EQ(test, fake.state.mp2_pwr_con, powered.mp2_pwr_con);
	KUNIT_EXPECT_EQ(test, fake.state.ext_buck_iso, powered.ext_buck_iso);
	KUNIT_EXPECT_EQ(test, fake.state.vproc_enabled, powered.vproc_enabled);
	KUNIT_EXPECT_EQ(test, fake.state.vsram_uv, powered.vsram_uv);
	KUNIT_EXPECT_EQ(test, fake.rollback_calls, 0U);
	KUNIT_EXPECT_EQ(test, fake.retain_calls, 1U);
}

/* Apply failures before/after effects, plus readback failures at every step. */
static void mt6797_a72_step_failures(struct kunit *test, unsigned int start)
{
	struct mt6797_a72_plan plan;
	unsigned int i, mode, j;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	for (mode = 0; mode < 3; mode++) {
		for (i = 0; i < plan.count; i++) {
			struct mt6797_a72_fake fake = mt6797_a72_stopped_fake(start == 2);
			struct mt6797_a72_state stopped = fake.state;
			struct mt6797_a72_off_state off = mt6797_a72_test_off_record(&stopped);
			struct mt6797_a72_fake expected = fake;
			struct mt6797_a72_transaction transaction = { };
			unsigned int calls;
			int ret;

			fake.transaction = &transaction;
			fake.fail_before_effect = mode == 2;
			if (mode == 1)
				fake.fail_verify_at = i;
			else
				fake.fail_apply_at = i;
			ret = start ? mt6797_a72_transaction_restart(&transaction, &plan,
				&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS,
				&off, &stopped) :
				mt6797_a72_transaction_begin(&transaction, &plan,
				&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS);
			KUNIT_EXPECT_EQ(test, fake.apply_calls, i + 1);
			KUNIT_EXPECT_FALSE(test, fake.state.transaction_guarded);
			KUNIT_EXPECT_FALSE(test, transaction.active);
			if (i < 6) {
				KUNIT_EXPECT_EQ(test, ret, -EIO);
				KUNIT_EXPECT_EQ(test, fake.rollback_calls, i + 1);
				KUNIT_EXPECT_EQ(test, fake.retain_calls, 0U);
				KUNIT_EXPECT_FALSE(test, transaction.faulted);
				KUNIT_EXPECT_FALSE(test, transaction.isolation_attempted);
			} else {
				/* Expected hardware is exactly the attempted forward prefix. */
				for (j = 0; j < i + (mode != 2); j++)
					mt6797_a72_fake_apply_step(&expected, &plan.steps[j]);
				KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
				KUNIT_EXPECT_EQ(test, fake.rollback_calls, 0U);
				KUNIT_EXPECT_EQ(test, fake.retain_calls, 1U);
				KUNIT_EXPECT_TRUE(test, transaction.faulted);
				KUNIT_EXPECT_TRUE(test, transaction.isolation_attempted);
				KUNIT_EXPECT_TRUE(test, fake.isolation_latched_at_dispatch);
				calls = fake.apply_calls;
				KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_begin(
					&transaction, &plan, &mt6797_a72_fake_ops,
					&fake, MT6797_A72_REQUIRED_CAPS), -EUCLEAN);
				KUNIT_EXPECT_EQ(test, fake.apply_calls, calls);
				KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_commit(
					&transaction), -EINVAL);
			}
			KUNIT_EXPECT_EQ(test, fake.state.mp2_pwr_con, expected.state.mp2_pwr_con);
			KUNIT_EXPECT_EQ(test, fake.state.ext_buck_iso, expected.state.ext_buck_iso);
			KUNIT_EXPECT_EQ(test, fake.state.pwrap_reset_asserted, expected.state.pwrap_reset_asserted);
			KUNIT_EXPECT_EQ(test, fake.state.vproc_enabled, expected.state.vproc_enabled);
			KUNIT_EXPECT_EQ(test, fake.state.vsram_uv, expected.state.vsram_uv);
			KUNIT_EXPECT_EQ(test, fake.state.vsram_selector, expected.state.vsram_selector);
			KUNIT_EXPECT_EQ(test, fake.state.vsram_control, expected.state.vsram_control);
			KUNIT_EXPECT_EQ(test, fake.state.vsram_calibration, expected.state.vsram_calibration);
		}
	}
}

static void mt6797_a72_all_step_failures_test(struct kunit *test)
{
	/* Cold boot, restart with reset SRAM, restart with retained SRAM. */
	mt6797_a72_step_failures(test, 0);
	mt6797_a72_step_failures(test, 1);
	mt6797_a72_step_failures(test, 2);
}

static void mt6797_a72_requires_isolated_start_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	fake.state.ext_buck_iso &= ~MT6797_A72_BIG_BUCK_ISO;
	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_begin(&transaction, &plan,
		&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS), -EBUSY);
	KUNIT_EXPECT_EQ(test, fake.apply_calls, 0U);
}

static void mt6797_a72_uncertain_rollback_fences_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	fake.fail_apply_at = 4;
	fake.fail_rollback_at = 0;
	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_transaction_begin(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS),
		-EUCLEAN);
	KUNIT_EXPECT_TRUE(test, transaction.faulted);
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_transaction_begin(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS),
		-EUCLEAN);
}

static void mt6797_a72_rejects_modified_plan_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	plan.steps[1].mask |= BIT(2);
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_transaction_begin(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS),
		-EINVAL);
	KUNIT_EXPECT_EQ(test, fake.apply_calls, 0U);
}

static void mt6797_a72_commit_forgets_rollback_state_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	ret = mt6797_a72_transaction_begin(&transaction, &plan,
					   &mt6797_a72_fake_ops, &fake,
					   MT6797_A72_REQUIRED_CAPS);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_commit(&transaction), 0);
	KUNIT_EXPECT_FALSE(test, transaction.active);
	KUNIT_EXPECT_EQ(test, transaction.applied, 0U);
	KUNIT_EXPECT_EQ(test, fake.rollback_calls, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_abort(&transaction), -EINVAL);
}

static void mt6797_a72_restart_admission_test(struct kunit *test)
{
	struct mt6797_a72_plan plan;
	unsigned int n;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	for (n = 0; n < 32; n++) {
		struct mt6797_a72_fake fake = mt6797_a72_stopped_fake(n != 31);
		struct mt6797_a72_state stopped = fake.state;
		struct mt6797_a72_off_state off = mt6797_a72_test_off_record(&stopped);
		struct mt6797_a72_transaction transaction = {};
		unsigned long caps = MT6797_A72_REQUIRED_CAPS;
		int ret;

		switch (n) {
		case 0:
			off.completed = false;
			break;
		case 1:
			off.cluster_off = false;
			break;
		case 2:
			off.faulted = true;
			break;
		case 3:
			off.error = -EIO;
			break;
		case 4:
			off.final.online_mask = 1;
			break;
		case 5:
			off.final.power_status = BIT(7);
			break;
		case 6:
			off.final.power_status = BIT(17);
			break;
		case 7:
			off.final.idvfs_control = 1;
			break;
		case 8:
			off.final.vproc_enabled = 1;
			break;
		case 9:
			off.final.ext_buck_iso = 0;
			break;
		case 10:
			off.final.mp2_pwr_con |= BIT(0);
			break;
		case 11:
			off.final.mp2_pwr_con &= ~BIT(4);
			break;
		case 12:
			fake.state.a72_online = true;
			break;
		case 13:
			fake.state.transaction_guarded = true;
			break;
		case 14:
			fake.state.vproc_enabled = 1;
			break;
		case 15:
			fake.state.pwrap_reset_asserted = 1;
			break;
		case 16:
			fake.state.mp2_pwr_con ^= BIT(16);
			break;
		case 17:
			fake.state.ext_buck_iso = 0;
			break;
		case 18:
			fake.state.vproc_uv += 10000;
			break;
		case 19:
			fake.state.vsram_uv -= 25000;
			break;
		case 20:
			fake.state.vsram_selector ^= 1;
			break;
		case 21:
			fake.state.vsram_control ^= BIT(22);
			break;
		case 22:
			fake.state.vsram_calibration ^= 1;
			break;
		case 23:
			stopped.a72_online = true;
			break;
		case 24:
			stopped.vproc_enabled = 1;
			break;
		case 25:
			stopped.vsram_uv = 1200000;
			break;
		case 26:
			caps = 0;
			break;
		case 27:
			fake.state.vsram_calibration = stopped.vsram_calibration = 0;
			break;
		case 28:
			fake.state.vsram_calibration = stopped.vsram_calibration = BIT(16);
			break;
		case 29:
			fake.state.vsram_control = stopped.vsram_control = 0;
			break;
		default:
			break;
		}
		/* The little-cluster isolation bit is independently owned. */
		fake.state.ext_buck_iso ^= BIT(0);
		fake.transaction = &transaction;
		ret = mt6797_a72_transaction_restart(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, caps, &off, &stopped);
		if (n < 30) {
			KUNIT_EXPECT_TRUE(test, ret < 0);
			KUNIT_EXPECT_EQ(test, fake.apply_calls, 0U);
		} else {
			KUNIT_EXPECT_EQ(test, ret, 0);
			KUNIT_EXPECT_EQ(test, fake.apply_calls, 12U);
			KUNIT_EXPECT_EQ(test, fake.state.vsram_uv, 1100000);
			KUNIT_EXPECT_EQ(test, mt6797_a72_transaction_commit(&transaction), 0);
		}
	}
}

static void mt6797_a72_rail_matrix_test(struct kunit *test)
{
	unsigned int cpu, sram, target, i;

	for (cpu = 800000; cpu <= 1200000; cpu += 10000)
		for (sram = 1000000; sram <= 1200000; sram += 25000) {
			if (sram < cpu || sram - cpu > 300000)
				continue;
			for (target = 800000; target <= 1200000; target += 10000) {
				struct mt6797_a72_rail_plan plan;
				unsigned int last_cpu = cpu, last_sram = sram;
				unsigned int end_sram = ((target + 124999) / 25000) * 25000;

				if (end_sram < 1000000) end_sram = 1000000;
				if (end_sram > 1200000) end_sram = 1200000;
				KUNIT_ASSERT_EQ(test, mt6797_a72_plan_rails(cpu, sram, target, &plan), 0);
				KUNIT_EXPECT_EQ(test, plan.initial.vproc_uv, cpu);
				KUNIT_EXPECT_EQ(test, plan.initial.vsram_uv, sram);
				KUNIT_EXPECT_LE(test, plan.count, MT6797_A72_RAIL_MAX_STEPS);
				for (i = 0; i < plan.count; i++) {
					unsigned int c = plan.steps[i].vproc_uv;
					unsigned int r = plan.steps[i].vsram_uv;

					KUNIT_EXPECT_TRUE(test, (c != last_cpu) != (r != last_sram));
					KUNIT_EXPECT_TRUE(test, c >= 800000 && c <= 1200000 && !(c % 10000));
					KUNIT_EXPECT_TRUE(test, r >= 1000000 && r <= 1200000 && !(r % 25000));
					KUNIT_EXPECT_TRUE(test, r >= c && r - c <= 300000);
					KUNIT_EXPECT_TRUE(test, target >= cpu ? c >= last_cpu && c <= target :
						c <= last_cpu && c >= target);
					KUNIT_EXPECT_TRUE(test, end_sram >= sram ? r >= last_sram && r <= end_sram :
						r <= last_sram && r >= end_sram);
					if (c < last_cpu || r > last_sram)
						KUNIT_EXPECT_LE(test, r - c, 275000U);
					last_cpu = c;
					last_sram = r;
				}
				KUNIT_EXPECT_EQ(test, last_cpu, target);
				KUNIT_EXPECT_EQ(test, last_sram, end_sram);
			}
		}
}

static void mt6797_a72_rail_admission_test(struct kunit *test)
{
	static const unsigned int invalid[][3] = {
		{ 799999, 1100000, 1000000 }, { 1200001, 1200000, 1000000 },
		{ 1000001, 1100000, 1000000 }, { 1000000, 999999, 1000000 },
		{ 1000000, 1200001, 1000000 }, { 1000000, 1100001, 1000000 },
		{ 1100000, 1000000, 1000000 }, { 800000, 1200000, 1000000 },
		{ 1000000, 1100000, 799999 }, { 1000000, 1100000, 1200001 },
		{ 1000000, 1100000, 1000001 }, { U32_MAX, U32_MAX, U32_MAX },
	};
	struct mt6797_a72_rail_plan plan, before;
	unsigned int i;

	memset(&before, 0xa5, sizeof(before));
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		plan = before;
		KUNIT_EXPECT_EQ(test, mt6797_a72_plan_rails(invalid[i][0], invalid[i][1],
			invalid[i][2], &plan), -EINVAL);
		KUNIT_EXPECT_EQ(test, memcmp(&plan, &before, sizeof(plan)), 0);
	}
	KUNIT_EXPECT_EQ(test, mt6797_a72_plan_rails(1000000, 1100000, 1000000, NULL), -EINVAL);
}

struct mt6797_a72_rail_fake {
	struct mt6797_a72_rail_step state;
	unsigned int reads, writes, waits, fail_read, fail_write, corrupt_read;
	unsigned int corrupt_bit, minimum_delay;
	bool effect, pending;
};

static int mt6797_a72_rail_fake_read(void *context, struct mt6797_a72_rail_step *state)
{
	struct mt6797_a72_rail_fake *f = context;

	if (f->pending)
		return -EBUSY;
	if (++f->reads == f->fail_read)
		return -EIO;
	*state = f->state;
	if (f->reads == f->corrupt_read) {
		if (f->corrupt_bit < 32)
			state->vproc_uv ^= BIT(f->corrupt_bit);
		else
			state->vsram_uv ^= BIT(f->corrupt_bit - 32);
	}
	return 0;
}

static int mt6797_a72_rail_fake_write(void *context, unsigned int uv, bool sram)
{
	struct mt6797_a72_rail_fake *f = context;
	bool fail = ++f->writes == f->fail_write;
	unsigned int old = sram ? f->state.vsram_uv : f->state.vproc_uv;
	unsigned int delta = old > uv ? old - uv : uv - old;
	unsigned int slew = !sram ? 10000 : uv > old ? 12500 : 3125;

	f->minimum_delay = (delta + slew - 1) / slew + (sram ? 5 : 0);
	if (f->minimum_delay < 25)
		f->minimum_delay = 25;
	if (!fail || f->effect) {
		if (sram) f->state.vsram_uv = uv;
		else f->state.vproc_uv = uv;
	}
	f->pending = true;
	return fail ? -EIO : 0;
}

static int mt6797_a72_rail_fake_cpu(void *context, unsigned int uv)
{
	return mt6797_a72_rail_fake_write(context, uv, false);
}

static int mt6797_a72_rail_fake_sram(void *context, unsigned int uv)
{
	return mt6797_a72_rail_fake_write(context, uv, true);
}

static void mt6797_a72_rail_fake_settle(void *context, unsigned int usec)
{
	struct mt6797_a72_rail_fake *f = context;

	if (usec < f->minimum_delay || usec > 69 || !f->pending)
		return;
	f->waits++;
	f->pending = false;
}

static const struct mt6797_a72_rail_io mt6797_a72_rail_fake_io = {
	.read = mt6797_a72_rail_fake_read,
	.set_vproc = mt6797_a72_rail_fake_cpu,
	.set_vsram = mt6797_a72_rail_fake_sram,
	.settle = mt6797_a72_rail_fake_settle,
};

static void mt6797_a72_rail_executor_test(struct kunit *test)
{
	unsigned int from, to, n, bit;

	for (from = 800000; from <= 1200000; from += 10000)
		for (to = 800000; to <= 1200000; to += 10000) {
			struct mt6797_a72_rail_plan plan, origin;
			struct mt6797_a72_rail_fake f;
			bool faulted;

			KUNIT_ASSERT_EQ(test, mt6797_a72_plan_rails(1000000, 1100000, from, &origin), 0);
			origin.initial = origin.count ? origin.steps[origin.count - 1] : origin.initial;
			KUNIT_ASSERT_EQ(test, mt6797_a72_plan_rails(from, origin.initial.vsram_uv, to, &plan), 0);
			f = (struct mt6797_a72_rail_fake) { .state = plan.initial };
			faulted = false;
			KUNIT_EXPECT_EQ(test, mt6797_a72_execute_rails(&plan, to,
				&mt6797_a72_rail_fake_io, &f, &faulted), 0);
			KUNIT_EXPECT_FALSE(test, faulted);
			KUNIT_EXPECT_EQ(test, f.writes, plan.count);
			KUNIT_EXPECT_EQ(test, f.waits, plan.count);
			for (n = 1; n <= plan.count + 1; n++) {
				f = (struct mt6797_a72_rail_fake) { .state = plan.initial, .fail_read = n };
				faulted = false;
				KUNIT_EXPECT_EQ(test, mt6797_a72_execute_rails(&plan, to,
					&mt6797_a72_rail_fake_io, &f, &faulted), n == 1 ? -EIO : -EUCLEAN);
				KUNIT_EXPECT_EQ(test, f.writes, n - 1);
				KUNIT_EXPECT_EQ(test, faulted, n != 1);
			}
			for (n = 1; n <= plan.count; n++) for (bit = 0; bit < 2; bit++) {
				f = (struct mt6797_a72_rail_fake) { .state = plan.initial,
					.fail_write = n, .effect = bit };
				faulted = false;
				KUNIT_EXPECT_EQ(test, mt6797_a72_execute_rails(&plan, to,
					&mt6797_a72_rail_fake_io, &f, &faulted), -EUCLEAN);
				KUNIT_EXPECT_TRUE(test, faulted);
				KUNIT_EXPECT_EQ(test, f.writes, n);
				KUNIT_EXPECT_EQ(test, mt6797_a72_execute_rails(&plan, to,
					&mt6797_a72_rail_fake_io, &f, &faulted), -EUCLEAN);
				KUNIT_EXPECT_EQ(test, f.writes, n);
			}
			for (n = 1; n <= plan.count + 1; n++) for (bit = 0; bit < 64; bit++) {
				f = (struct mt6797_a72_rail_fake) { .state = plan.initial,
					.corrupt_read = n, .corrupt_bit = bit };
				faulted = false;
				KUNIT_EXPECT_EQ(test, mt6797_a72_execute_rails(&plan, to,
					&mt6797_a72_rail_fake_io, &f, &faulted), n == 1 ? -EAGAIN : -EUCLEAN);
				KUNIT_EXPECT_EQ(test, f.writes, n - 1);
				KUNIT_EXPECT_EQ(test, faulted, n != 1);
			}
		}
	/* Reject changes to the full plan, including unused entries, before I/O. */
	for (n = 0; n < sizeof(struct mt6797_a72_rail_plan); n++) {
		struct mt6797_a72_rail_plan plan;
		struct mt6797_a72_rail_fake f = { };
		bool faulted = false;

		KUNIT_ASSERT_EQ(test, mt6797_a72_plan_rails(800000, 1000000, 1200000, &plan), 0);
		((u8 *)&plan)[n] ^= 1;
		KUNIT_EXPECT_TRUE(test, mt6797_a72_execute_rails(&plan, 1200000,
			&mt6797_a72_rail_fake_io, &f, &faulted) < 0);
		KUNIT_EXPECT_EQ(test, f.reads, 0U);
		KUNIT_EXPECT_EQ(test, f.writes, 0U);
	}
}

static struct kunit_case mt6797_a72_power_test_cases[] = {
	KUNIT_CASE(mt6797_a72_rail_executor_test),
	KUNIT_CASE(mt6797_a72_rail_matrix_test),
	KUNIT_CASE(mt6797_a72_rail_admission_test),
	KUNIT_CASE(mt6797_a72_plan_matches_vendor_boundary_test),
	KUNIT_CASE(mt6797_a72_restart_admission_test),
	KUNIT_CASE(mt6797_a72_admission_requires_live_backends_test),
	KUNIT_CASE(mt6797_a72_admission_fences_online_cpu_test),
	KUNIT_CASE(mt6797_a72_admission_requires_full_cold_sram_test),
	KUNIT_CASE(mt6797_a72_cpu8_boot_admission_test),
	KUNIT_CASE(mt6797_a72_cpu9_boot_admission_test),
	KUNIT_CASE(mt6797_a72_cpu8_identity_test),
	KUNIT_CASE(mt6797_a72_removal_requires_quiescent_owner_test),
	KUNIT_CASE(mt6797_a72_transaction_orders_and_aborts_test),
	KUNIT_CASE(mt6797_a72_all_step_failures_test),
	KUNIT_CASE(mt6797_a72_requires_isolated_start_test),
	KUNIT_CASE(mt6797_a72_uncertain_rollback_fences_test),
	KUNIT_CASE(mt6797_a72_rejects_modified_plan_test),
	KUNIT_CASE(mt6797_a72_commit_forgets_rollback_state_test),
	{ }
};

static struct kunit_suite mt6797_a72_power_test_suite = {
	.name = "mt6797-a72-power",
	.test_cases = mt6797_a72_power_test_cases,
};

kunit_test_suite(mt6797_a72_power_test_suite);

MODULE_LICENSE("GPL");
