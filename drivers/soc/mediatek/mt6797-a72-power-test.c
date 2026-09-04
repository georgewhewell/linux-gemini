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
			.vsram_uv = 900000,
			.vsram_selector = 3,
		},
		.fail_apply_at = -1,
		.fail_verify_at = -1,
		.fail_rollback_at = -1,
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

static const struct mt6797_a72_backend_ops mt6797_a72_fake_ops = {
	.snapshot = mt6797_a72_fake_snapshot,
	.apply = mt6797_a72_fake_apply,
	.verify = mt6797_a72_fake_verify,
	.rollback = mt6797_a72_fake_rollback,
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

static void mt6797_a72_admission_rejects_unrestorable_selector_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	int ret;

	fake.state.vsram_selector = 0;
	ret = mt6797_a72_validate_admission(&fake.state,
					    MT6797_A72_REQUIRED_CAPS);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
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
	struct mt6797_a72_state initial = fake.state;
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	ret = mt6797_a72_transaction_begin(&transaction, &plan,
					   &mt6797_a72_fake_ops, &fake,
					   MT6797_A72_REQUIRED_CAPS);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.active);
	KUNIT_EXPECT_EQ(test, fake.apply_calls, plan.count);
	KUNIT_EXPECT_EQ(test, fake.applied[2],
			(enum mt6797_a72_action)MT6797_A72_IDVFS_READ_FENCE);
	KUNIT_EXPECT_EQ(test, fake.applied[7],
			(enum mt6797_a72_action)MT6797_A72_PWRAP_RESET_DEASSERT);

	KUNIT_ASSERT_EQ(test, mt6797_a72_transaction_abort(&transaction), 0);
	KUNIT_EXPECT_FALSE(test, transaction.active);
	KUNIT_EXPECT_EQ(test, fake.state.mp2_pwr_con, initial.mp2_pwr_con);
	KUNIT_EXPECT_EQ(test, fake.state.ext_buck_iso, initial.ext_buck_iso);
	KUNIT_EXPECT_EQ(test, fake.state.vproc_enabled, initial.vproc_enabled);
	KUNIT_EXPECT_EQ(test, fake.state.vsram_uv, initial.vsram_uv);
	KUNIT_EXPECT_EQ(test, fake.rollback_calls, plan.count);
}

static void mt6797_a72_failure_rolls_back_attempted_step_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_state initial = fake.state;
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	fake.fail_apply_at = 6;
	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_transaction_begin(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS),
		-EIO);
	KUNIT_EXPECT_EQ(test, fake.rollback_calls, 7U);
	KUNIT_EXPECT_EQ(test, fake.rolled_back[0], plan.steps[6].action);
	KUNIT_EXPECT_EQ(test, fake.state.mp2_pwr_con, initial.mp2_pwr_con);
	KUNIT_EXPECT_EQ(test, fake.state.ext_buck_iso, initial.ext_buck_iso);
	KUNIT_EXPECT_FALSE(test, transaction.faulted);
}

static void mt6797_a72_verify_failure_rolls_back_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_state initial = fake.state;
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	fake.fail_verify_at = 10;
	KUNIT_ASSERT_EQ(test, mt6797_a72_build_plan(&plan), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_a72_transaction_begin(&transaction, &plan,
			&mt6797_a72_fake_ops, &fake, MT6797_A72_REQUIRED_CAPS),
		-EIO);
	KUNIT_EXPECT_EQ(test, fake.rollback_calls, 11U);
	KUNIT_EXPECT_EQ(test, fake.state.vsram_uv, initial.vsram_uv);
	KUNIT_EXPECT_FALSE(test, transaction.faulted);
}

static void mt6797_a72_uncertain_rollback_fences_test(struct kunit *test)
{
	struct mt6797_a72_fake fake = mt6797_a72_new_fake();
	struct mt6797_a72_transaction transaction = { };
	struct mt6797_a72_plan plan;

	fake.fail_apply_at = 6;
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

static struct kunit_case mt6797_a72_power_test_cases[] = {
	KUNIT_CASE(mt6797_a72_plan_matches_vendor_boundary_test),
	KUNIT_CASE(mt6797_a72_admission_requires_live_backends_test),
	KUNIT_CASE(mt6797_a72_admission_fences_online_cpu_test),
	KUNIT_CASE(mt6797_a72_admission_rejects_unrestorable_selector_test),
	KUNIT_CASE(mt6797_a72_cpu8_boot_admission_test),
	KUNIT_CASE(mt6797_a72_cpu8_identity_test),
	KUNIT_CASE(mt6797_a72_removal_requires_quiescent_owner_test),
	KUNIT_CASE(mt6797_a72_transaction_orders_and_aborts_test),
	KUNIT_CASE(mt6797_a72_failure_rolls_back_attempted_step_test),
	KUNIT_CASE(mt6797_a72_verify_failure_rolls_back_test),
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
