// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <asm/late_cpu_startup.h>

static const struct arm64_late_cpu_up_token cpu8_token = {
	.abi = ARM64_LATE_CPU_STARTUP_ABI,
	.owner = ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP,
	.operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP,
	.target_cpu = 8,
	.target_mpidr = 0x200,
	.generation = 42,
	.cookie = 0x8a720001,
	.plan_identity = { 1, 2, 3, 4 },
};

/* The lower opaque generation deliberately rejects a global ordering rule. */
static const struct arm64_late_cpu_up_token cpu9_token = {
	.abi = ARM64_LATE_CPU_STARTUP_ABI,
	.owner = ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP,
	.operation = ARM64_LATE_CPU_STARTUP_OP_CPU9_UP,
	.target_cpu = 9,
	.target_mpidr = 0x201,
	.generation = 7,
	.cookie = 0x9a720001,
	.plan_identity = { 5, 6, 7, 8 },
};

static const struct arm64_late_cpu_startup_target cpu8_target = {
	.cpu = 8,
	.mpidr = 0x200,
	.generation = 42,
	.cookie = 0x8a720001,
};

static const struct arm64_late_cpu_startup_target cpu9_target = {
	.cpu = 9,
	.mpidr = 0x201,
	.generation = 7,
	.cookie = 0x9a720001,
};

static int late_cpu_startup_test_init(struct kunit *test)
{
	(void)test;
	arm64_late_cpu_startup_test_reset();
	return 0;
}

static void late_cpu_startup_invalid_token_test(struct kunit *test)
{
	struct arm64_late_cpu_up_token token = cpu8_token;

	token.generation = 0;
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&token), -EINVAL);
	token = cpu8_token;
	token.target_mpidr = 0x201;
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&token), -EINVAL);
	KUNIT_EXPECT_FALSE(test, arm64_late_cpu_startup_quarantined());
}

static void late_cpu_startup_abort_is_one_shot_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	struct arm64_late_cpu_up_token replay = cpu8_token;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_abort_unissued(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_aborted(&cpu8_token), 0);
	replay.generation = 100;
	replay.cookie++;
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&replay),
			-EALREADY);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&cpu9_token), 0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PREPARED);
	KUNIT_EXPECT_EQ(test, snapshot.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_EQ(test, snapshot.retired_token[0].generation,
			cpu8_token.generation);
	KUNIT_EXPECT_FALSE(test, snapshot.quarantined);
}

static void late_cpu_startup_unproven_cpu_on_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	enum arm64_late_cpu_startup_state winner;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_fault_cpu_on_unproven(
				&cpu8_token, &winner),
			0);
	KUNIT_EXPECT_EQ(test, winner,
			(enum arm64_late_cpu_startup_state)
				ARM64_LATE_CPU_STARTUP_FAULTED);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token),
			-ESHUTDOWN);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0x100, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PARKED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_CPU_ON_UNPROVEN);
}

static void late_cpu_startup_cpu_on_fault_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	enum arm64_late_cpu_startup_state winner;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_cpu_on_fault(&cpu8_token, &winner),
		0);
	KUNIT_EXPECT_EQ(test, winner,
			(enum arm64_late_cpu_startup_state)
				ARM64_LATE_CPU_STARTUP_FAULTED);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0x200, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.terminal.terminal_from,
			(u32)ARM64_LATE_CPU_STARTUP_FAULTED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_CPU_ON_FAULT);
}

static void late_cpu_startup_cancel_wins_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	enum arm64_late_cpu_startup_claim claim;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	claim = arm64_late_cpu_startup_cancel(&cpu8_token);
	KUNIT_ASSERT_EQ(test, claim,
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	claim = arm64_late_cpu_startup_claim_publish(&cpu8_target);
	KUNIT_EXPECT_EQ(test, claim,
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_CANCELLED);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0x300, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PARKED);
	KUNIT_EXPECT_EQ(test, snapshot.terminal.terminal_from,
			(u32)ARM64_LATE_CPU_STARTUP_CANCELLED);
	KUNIT_EXPECT_EQ(test, snapshot.terminal.branch,
			(u32)ARM64_LATE_CPU_STARTUP_BRANCH_U);
	KUNIT_EXPECT_TRUE(test, snapshot.park_committed);
	KUNIT_EXPECT_TRUE(test, snapshot.quarantined);
}

static void late_cpu_startup_cpu8_then_cpu9_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	struct arm64_late_cpu_up_token replay = cpu8_token;
	enum arm64_late_cpu_startup_claim claim;

	arm64_late_cpu_startup_test_set_online(8, true);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	claim = arm64_late_cpu_startup_claim_publish(&cpu8_target);
	KUNIT_ASSERT_EQ(test, claim,
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_cancel(&cpu8_token),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_PUBLISHER);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu8_target,
				ARM64_LATE_CPU_EFFECT_MM |
					ARM64_LATE_CPU_EFFECT_RCU_STARTING),
			0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_consume_published(&cpu8_token), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_consume_published(&cpu8_token),
			-EALREADY);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_retire_published_after_p14_p15(
				&cpu8_token),
			0);

	replay.generation = 1000;
	replay.cookie++;
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&replay),
			-EALREADY);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu9_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu9_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu9_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu9_target, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_test_set_online(9, true);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_consume_published(&cpu9_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_retire_published_after_p14_p15(
				&cpu9_token),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state, (u32)ARM64_LATE_CPU_STARTUP_FREE);
	KUNIT_EXPECT_EQ(test, snapshot.retired_mask, (u32)(BIT(0) | BIT(1)));
	KUNIT_EXPECT_EQ(test, snapshot.retired_token[0].generation, (u64)42);
	KUNIT_EXPECT_EQ(test, snapshot.retired_token[1].generation, (u64)7);
	KUNIT_EXPECT_FALSE(test, snapshot.quarantined);
}

static void late_cpu_startup_armed_quarantine_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	struct arm64_late_cpu_startup_target wrong = cpu8_target;

	wrong.cpu = 9;
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_claim_publish(&wrong),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_STALE);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0x400, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&cpu9_token),
			-ESHUTDOWN);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PARKED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_IDENTITY);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_token.generation,
			cpu8_token.generation);
}

static void late_cpu_startup_publishing_quarantine_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};

	arm64_late_cpu_startup_test_set_online(8, true);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(
		test, arm64_late_cpu_startup_publish(&cpu8_target, BIT_ULL(63)),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_PUBLISHER);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu8_target, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_QUARANTINED);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_consume_published(&cpu8_token),
			-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_retire_published_after_p14_p15(
				&cpu8_token),
			-ESHUTDOWN);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PUBLISHED);
	KUNIT_EXPECT_TRUE(test, snapshot.completion_consumed);
	KUNIT_EXPECT_TRUE(test, snapshot.online_validated);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
}

static void late_cpu_startup_publishing_wrong_target_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	struct arm64_late_cpu_startup_target wrong = cpu8_target;

	wrong.generation++;
	arm64_late_cpu_startup_test_set_online(8, true);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_claim_publish(&wrong),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_STALE);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu8_target, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_consume_published(&cpu8_token),
			-ESHUTDOWN);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PUBLISHED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_IDENTITY);
	KUNIT_EXPECT_TRUE(test, snapshot.completion_consumed);
	KUNIT_EXPECT_TRUE(test, snapshot.online_validated);
}

static void late_cpu_startup_online_mismatch_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};

	arm64_late_cpu_startup_test_set_online(8, false);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu8_target, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_consume_published(&cpu8_token),
			-EUCLEAN);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_retire_published_after_p14_p15(
				&cpu8_token),
			-ESHUTDOWN);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_TRUE(test, snapshot.completion_consumed);
	KUNIT_EXPECT_FALSE(test, snapshot.online_validated);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_ONLINE_MISMATCH);
}

static void late_cpu_startup_premature_retire_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};

	arm64_late_cpu_startup_test_set_online(8, true);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_publish(
				&cpu8_target, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_retire_published_after_p14_p15(
				&cpu8_token),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_consume_published(&cpu8_token),
			-ESHUTDOWN);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PUBLISHED);
	KUNIT_EXPECT_EQ(test, snapshot.retired_mask, (u32)0);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_REUSE);
}

static void late_cpu_startup_terminal_branches_test(struct kunit *test)
{
	static const struct {
		enum arm64_late_cpu_startup_branch claimed;
		enum arm64_late_cpu_startup_branch final;
		u64 reason;
	} cases[] = {
		{ ARM64_LATE_CPU_STARTUP_BRANCH_K,
		  ARM64_LATE_CPU_STARTUP_BRANCH_K, 0x501 },
		{ ARM64_LATE_CPU_STARTUP_BRANCH_C,
		  ARM64_LATE_CPU_STARTUP_BRANCH_C, 0x502 },
		{ ARM64_LATE_CPU_STARTUP_BRANCH_E,
		  ARM64_LATE_CPU_STARTUP_BRANCH_E, 0x503 },
		{ ARM64_LATE_CPU_STARTUP_BRANCH_U,
		  ARM64_LATE_CPU_STARTUP_BRANCH_U, 0x504 },
	};
	struct arm64_late_cpu_startup_snapshot snapshot;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		arm64_late_cpu_startup_test_reset();
		KUNIT_ASSERT_EQ(test,
				arm64_late_cpu_startup_prepare(&cpu8_token), 0);
		KUNIT_ASSERT_EQ(
			test,
			arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token),
			0);
		KUNIT_ASSERT_EQ(test,
				arm64_late_cpu_startup_claim_failure(
					&cpu8_target, cases[i].claimed),
				(enum arm64_late_cpu_startup_claim)
					ARM64_LATE_CPU_STARTUP_CLAIM_WON);
		KUNIT_ASSERT_EQ(test,
				arm64_late_cpu_startup_park(
					&cpu8_target, cases[i].final,
					cases[i].reason,
					ARM64_LATE_CPU_EFFECT_MM),
				0);
		memset(&snapshot, 0, sizeof(snapshot));
		arm64_late_cpu_startup_snapshot(&snapshot);
		KUNIT_EXPECT_EQ(test, snapshot.state,
				(u32)ARM64_LATE_CPU_STARTUP_PARKED);
		KUNIT_EXPECT_EQ(test, snapshot.terminal.claimed_branch,
				(u32)cases[i].claimed);
		KUNIT_EXPECT_EQ(test, snapshot.terminal.branch,
				(u32)cases[i].final);
		KUNIT_EXPECT_EQ(test, snapshot.terminal.reason,
				cases[i].reason);
	}
}

static void late_cpu_startup_k_to_c_immutability_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot before = {};
	struct arm64_late_cpu_startup_snapshot after = {};
	struct arm64_late_cpu_startup_target wrong = cpu8_target;

	wrong.cookie++;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_K),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&wrong, ARM64_LATE_CPU_STARTUP_BRANCH_K),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_STALE);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_park(
				&wrong, ARM64_LATE_CPU_STARTUP_BRANCH_C, 0x5ff,
				ARM64_LATE_CPU_EFFECT_MM),
			-ESTALE);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_C,
				0x600,
				ARM64_LATE_CPU_EFFECT_PRESENT_CLEAR |
					ARM64_LATE_CPU_EFFECT_RCU_DEAD |
					ARM64_LATE_CPU_EFFECT_DIE_GUARD),
			0);
	arm64_late_cpu_startup_snapshot(&before);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_C,
				0x601, ARM64_LATE_CPU_EFFECT_MM),
			-EPROTO);
	arm64_late_cpu_startup_snapshot(&after);
	KUNIT_EXPECT_EQ(test, before.terminal.claimed_branch,
			(u32)ARM64_LATE_CPU_STARTUP_BRANCH_K);
	KUNIT_EXPECT_EQ(test, before.terminal.branch,
			(u32)ARM64_LATE_CPU_STARTUP_BRANCH_C);
	KUNIT_EXPECT_EQ(test, after.terminal.reason, before.terminal.reason);
	KUNIT_EXPECT_EQ(test, after.terminal.effects, before.terminal.effects);
}

static void late_cpu_startup_panic_order_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};
	struct arm64_late_cpu_startup_target wrong = cpu8_target;

	wrong.cookie++;
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_P),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_P,
				0x700, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_publish_panicked(&cpu8_token),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_panic_park(&wrong, 0x701, 0),
			-ESTALE);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_panic_park(
				&cpu8_target, 0x702, ARM64_LATE_CPU_EFFECT_MM),
			0);
	KUNIT_EXPECT_EQ(
		test, arm64_late_cpu_startup_panic_park(&cpu8_target, 0x703, 0),
		-EPROTO);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_publish_panicked(&cpu8_token), 0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PANICKED);
	KUNIT_EXPECT_EQ(test, snapshot.terminal.claimed_branch,
			(u32)ARM64_LATE_CPU_STARTUP_BRANCH_P);
	KUNIT_EXPECT_EQ(test, snapshot.terminal.reason, (u64)0x702);
	KUNIT_EXPECT_TRUE(test, snapshot.park_committed);
	KUNIT_EXPECT_TRUE(test, snapshot.stuck_interlock);
}

static void late_cpu_startup_target_tuple_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot;
	struct arm64_late_cpu_startup_target wrong;
	unsigned int i;

	for (i = 0; i < 5; i++) {
		arm64_late_cpu_startup_test_reset();
		wrong = cpu8_target;
		switch (i) {
		case 0:
			wrong.cpu++;
			break;
		case 1:
			wrong.reserved = 1;
			break;
		case 2:
			wrong.mpidr++;
			break;
		case 3:
			wrong.generation++;
			break;
		default:
			wrong.cookie++;
			break;
		}

		KUNIT_ASSERT_EQ(test,
				arm64_late_cpu_startup_prepare(&cpu8_token), 0);
		KUNIT_ASSERT_EQ(
			test,
			arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token),
			0);
		KUNIT_EXPECT_EQ(test,
				arm64_late_cpu_startup_claim_publish(&wrong),
				(enum arm64_late_cpu_startup_claim)
					ARM64_LATE_CPU_STARTUP_CLAIM_STALE);
		memset(&snapshot, 0, sizeof(snapshot));
		arm64_late_cpu_startup_snapshot(&snapshot);
		KUNIT_EXPECT_EQ(test, snapshot.state,
				(u32)ARM64_LATE_CPU_STARTUP_FAULTED);
		KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
				(u32)ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		KUNIT_ASSERT_EQ(test,
				arm64_late_cpu_startup_park(
					&cpu8_target,
					ARM64_LATE_CPU_STARTUP_BRANCH_U,
					0x800 + i, ARM64_LATE_CPU_EFFECT_MM),
				0);
	}
}

static void late_cpu_startup_invalid_failure_branch_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&cpu8_target,
				ARM64_LATE_CPU_STARTUP_BRANCH_NONE),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_STALE);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0x900, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PARKED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);

	arm64_late_cpu_startup_test_reset();
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(
		test, arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_cancel(&cpu8_token),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_WON);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&cpu8_target,
				ARM64_LATE_CPU_STARTUP_BRANCH_NONE),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_CANCELLED);
}

static void late_cpu_startup_prearmed_target_claim_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot snapshot = {};

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_publish(&cpu8_target),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_arm_before_cpu_on(&cpu8_token),
			-ESHUTDOWN);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0xa00, ARM64_LATE_CPU_EFFECT_MM),
			0);
	arm64_late_cpu_startup_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.state,
			(u32)ARM64_LATE_CPU_STARTUP_PARKED);
	KUNIT_EXPECT_EQ(test, snapshot.quarantine_cause,
			(u32)ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);

	arm64_late_cpu_startup_test_reset();
	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_abort_unissued(&cpu8_token), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_failure(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_K),
			(enum arm64_late_cpu_startup_claim)
				ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_aborted(&cpu8_token),
			-ESHUTDOWN);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_park(
				&cpu8_target, ARM64_LATE_CPU_STARTUP_BRANCH_U,
				0xa01, ARM64_LATE_CPU_EFFECT_MM),
			0);
}

static void late_cpu_startup_bootstrap_claim_excludes_prepare_test(struct kunit *test)
{
	struct arm64_late_cpu_startup_snapshot before = {};
	struct arm64_late_cpu_startup_snapshot after = {};
	struct arm64_late_cpu_bootstrap_claim claim;
	struct arm64_late_cpu_bootstrap_claim second;

	arm64_late_cpu_startup_snapshot(&before);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test, claim.abi,
			(u32)ARM64_LATE_CPU_BOOTSTRAP_CLAIM_ABI);
	KUNIT_EXPECT_NE(test, claim.cookie, (u64)0);
	memset(&second, 0xa5, sizeof(second));
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&second), -EBUSY);
	KUNIT_EXPECT_EQ(test, second.abi, (u32)0);
	KUNIT_EXPECT_EQ(test, second.cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token),
			-EBUSY);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);
	arm64_late_cpu_startup_snapshot(&after);
	KUNIT_EXPECT_MEMEQ(test, &before, &after, sizeof(before));
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
}

static void late_cpu_startup_bootstrap_claim_identity_test(struct kunit *test)
{
	struct arm64_late_cpu_bootstrap_claim claim;
	struct arm64_late_cpu_bootstrap_claim wrong;

	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	wrong = claim;
	wrong.cookie++;
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&wrong), -ESTALE);
	KUNIT_EXPECT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token),
			-EBUSY);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), -EINVAL);
}

static void late_cpu_startup_bootstrap_claim_rejects_nonpristine_test(struct kunit *test)
{
	struct arm64_late_cpu_bootstrap_claim claim;

	KUNIT_ASSERT_EQ(test, arm64_late_cpu_startup_prepare(&cpu8_token), 0);
	memset(&claim, 0xa5, sizeof(claim));
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), -EBUSY);
	KUNIT_EXPECT_EQ(test, claim.abi, (u32)0);
	KUNIT_EXPECT_EQ(test, claim.cookie, (u64)0);
}

static struct kunit_case late_cpu_startup_test_cases[] = {
	KUNIT_CASE(late_cpu_startup_bootstrap_claim_excludes_prepare_test),
	KUNIT_CASE(late_cpu_startup_bootstrap_claim_identity_test),
	KUNIT_CASE(late_cpu_startup_bootstrap_claim_rejects_nonpristine_test),
	KUNIT_CASE(late_cpu_startup_invalid_token_test),
	KUNIT_CASE(late_cpu_startup_abort_is_one_shot_test),
	KUNIT_CASE(late_cpu_startup_unproven_cpu_on_test),
	KUNIT_CASE(late_cpu_startup_cpu_on_fault_test),
	KUNIT_CASE(late_cpu_startup_cancel_wins_test),
	KUNIT_CASE(late_cpu_startup_cpu8_then_cpu9_test),
	KUNIT_CASE(late_cpu_startup_armed_quarantine_test),
	KUNIT_CASE(late_cpu_startup_publishing_quarantine_test),
	KUNIT_CASE(late_cpu_startup_publishing_wrong_target_test),
	KUNIT_CASE(late_cpu_startup_online_mismatch_test),
	KUNIT_CASE(late_cpu_startup_premature_retire_test),
	KUNIT_CASE(late_cpu_startup_terminal_branches_test),
	KUNIT_CASE(late_cpu_startup_k_to_c_immutability_test),
	KUNIT_CASE(late_cpu_startup_panic_order_test),
	KUNIT_CASE(late_cpu_startup_target_tuple_test),
	KUNIT_CASE(late_cpu_startup_invalid_failure_branch_test),
	KUNIT_CASE(late_cpu_startup_prearmed_target_claim_test),
	{}
};

static struct kunit_suite late_cpu_startup_test_suite = {
	.name = "arm64-late-cpu-startup",
	.init = late_cpu_startup_test_init,
	.test_cases = late_cpu_startup_test_cases,
};

kunit_test_suite(late_cpu_startup_test_suite);

MODULE_LICENSE("GPL");
