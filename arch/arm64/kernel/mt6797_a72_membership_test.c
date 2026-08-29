// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include <asm/cpu_ops.h>
#include <asm/memory.h>
#include <asm/mt6797_a72_membership.h>
#include <asm/smp.h>

extern const struct cpu_operations mt6797_psci_ops;

struct owner_observation {
	struct mt6797_a72_owner_snapshot owner;
	struct arm64_late_cpu_startup_snapshot p30;
};

static bool
mt6797_a72_transaction_empty(const struct mt6797_a72_transaction *transaction)
{
	const struct mt6797_a72_transaction empty = { };

	return !memcmp(transaction, &empty, sizeof(empty));
}

static struct mt6797_a72_entry_snapshot
mt6797_a72_entry_for_up(unsigned int cpu)
{
	struct mt6797_a72_entry_snapshot entry = {
		.cpuhp_state_cpu8 = CPUHP_OFFLINE,
		.cpuhp_state_cpu9 = CPUHP_OFFLINE,
		.observer_window = MT6797_A72_OBSERVER_WINDOW_OPEN,
		.flags = MT6797_A72_ENTRY_CPU8_PRESENT |
			 MT6797_A72_ENTRY_CPU9_PRESENT |
			 MT6797_A72_ENTRY_CPU8_POSSIBLE |
			 MT6797_A72_ENTRY_CPU9_POSSIBLE,
		.cpu8_mpidr = 0x200,
		.cpu9_mpidr = 0x201,
	};

	if (cpu == 9) {
		entry.members = BIT(0);
		entry.provider_state = MT6797_A72_PROVIDER_HELD;
		entry.online_mask = BIT(0);
		entry.cpuhp_state_cpu8 = CPUHP_ONLINE;
	}

	return entry;
}

static struct mt6797_a72_entry_snapshot
mt6797_a72_entry_for_up_closed_window(unsigned int cpu)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(cpu);

	entry.observer_window = MT6797_A72_OBSERVER_WINDOW_CLOSED;
	return entry;
}

static struct mt6797_a72_a36_prestate
mt6797_a72_prestate_for_up(unsigned int cpu)
{
	struct mt6797_a72_a36_prestate prestate = {
		.abi = MT6797_A72_A36_PRESTATE_ABI,
		.observer_window = MT6797_A72_OBSERVER_WINDOW_OPEN,
		.call_shape = MT6797_A72_A36_CALL_SHAPE_TWO_ARG,
		.da921x_page = MT6797_A72_A36_DA921X_PAGE,
		.buckb_vsel = MT6797_A72_A36_BUCKB_VSEL,
		.spm_218 = MT6797_A72_A36_SPM_218,
		.spm_290 = MT6797_A72_A36_SPM_290,
		.secure_sentinels_stable = 1,
		.protected_clock_valid = 1,
		.pstore_console_available = 1,
		.watchdog_owned = 1,
		.secondary_entry_pa = __pa_symbol(secondary_entry),
		.generation = 1,
		.cookie = 0xa7200001,
	};

	if (cpu == 8) {
		prestate.operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
		prestate.target_mpidr = 0x200;
	} else {
		prestate.operation = ARM64_LATE_CPU_STARTUP_OP_CPU9_UP;
		prestate.cpu8_online = 1;
		prestate.cpu8_cluster_dcm_published = 1;
		prestate.target_mpidr = 0x201;
	}
	return prestate;
}

static struct arm64_late_cpu_ready_token
mt6797_a72_ready_token_for_up(void)
{
	struct arm64_late_cpu_ready_token ready = {
		.abi = ARM64_LATE_CPU_PLAN_ABI,
		.plan_identity = { 1, 2, 3, 4 },
		.source_parent_identity = { 5, 6, 7, 8 },
		.config_input_identity = { 9, 10, 11, 12 },
		.evidence_identity = { 13, 14, 15, 16 },
		.target_cpu = { 8, 9 },
		.expected_target_mpidr = { 0x200, 0x201 },
		.observed_target_mpidr = { 0x200, 0x201 },
	};

	strscpy(ready.profile_id, "mt6797-a53-a72-a41-v7",
		sizeof(ready.profile_id));
	cpumask_set_cpu(8, &ready.target_cpus);
	cpumask_set_cpu(9, &ready.target_cpus);
	return ready;
}

static void
owner_observe(struct owner_observation *observation)
{
	mt6797_a72_membership_snapshot(&observation->owner);
	arm64_late_cpu_startup_snapshot(&observation->p30);
}

static void
expect_unchanged(struct kunit *test,
		 const struct owner_observation *before,
		 const struct owner_observation *after)
{
	int difference;

	difference = memcmp(&before->owner, &after->owner,
			    sizeof(before->owner));
	KUNIT_EXPECT_EQ(test, difference, 0);
	difference = memcmp(&before->p30, &after->p30,
			    sizeof(before->p30));
	KUNIT_EXPECT_EQ(test, difference, 0);
}

static int mt6797_a72_owner_test_init(struct kunit *test)
{
	(void)test;
	mt6797_a72_membership_test_reset();
	return 0;
}

static void mt6797_a72_owner_initial_closed(struct kunit *test)
{
	const struct mt6797_a72_transaction empty_transaction = { };
	const struct mt6797_a72_provider_identity empty_provider = { };
	const struct mt6797_a72_fault_record empty_fault = { };
	struct mt6797_a72_owner_snapshot snapshot;
	unsigned int slot;
	int difference;

	mt6797_a72_membership_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.abi,
			(u32)MT6797_A72_TRANSACTION_ABI);
	KUNIT_EXPECT_EQ(test, snapshot.health,
			(u32)MT6797_A72_OWNER_CLOSED);
	KUNIT_EXPECT_EQ(test, snapshot.phase,
			(u32)MT6797_A72_PHASE_UNINITIALIZED);
	KUNIT_EXPECT_FALSE(test, snapshot.bootstrap_valid);
	KUNIT_EXPECT_FALSE(test, snapshot.members_valid);
	KUNIT_EXPECT_EQ(test, snapshot.members, (u32)0);
	KUNIT_EXPECT_EQ(test, snapshot.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, snapshot.attempts_available, (u32)0);
	KUNIT_EXPECT_EQ(test, snapshot.attempts_consumed, (u32)0);
	KUNIT_EXPECT_EQ(test, snapshot.retired_mask, (u32)0);
	KUNIT_EXPECT_FALSE(test, snapshot.controller_present);
	KUNIT_EXPECT_EQ(test, snapshot.controller_cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, snapshot.diagnostic_blockers,
			(u64)MT6797_A72_BLOCK_MASK);
	difference = memcmp(&snapshot.active, &empty_transaction,
			    sizeof(empty_transaction));
	KUNIT_EXPECT_EQ(test, difference, 0);
	for (slot = 0; slot < MT6797_A72_OPERATION_SLOTS; slot++) {
		difference = memcmp(&snapshot.retired[slot], &empty_transaction,
				    sizeof(empty_transaction));
		KUNIT_EXPECT_EQ(test, difference, 0);
	}
	difference = memcmp(&snapshot.provider_identity, &empty_provider,
			    sizeof(empty_provider));
	KUNIT_EXPECT_EQ(test, difference, 0);
	difference = memcmp(&snapshot.first_fault, &empty_fault,
			    sizeof(empty_fault));
	KUNIT_EXPECT_EQ(test, difference, 0);
}

static void mt6797_a72_owner_cpu8_denied(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct mt6797_a72_transaction transaction;
	int ret;

	owner_observe(&before);
	memset(&transaction, 0xa5, sizeof(transaction));
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_cpu9_denied(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct mt6797_a72_transaction transaction;
	int ret;

	owner_observe(&before);
	memset(&transaction, 0x5a, sizeof(transaction));
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, NULL, NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_public_hook_denied(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mt6797_psci_ops.cpu_up_preflight);
	owner_observe(&before);
	ret = mt6797_psci_ops.cpu_up_preflight(0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_preflight(7, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_preflight(8, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_preflight(9, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_preflight(8, CPUHP_AP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_internal_hook_denied(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mt6797_psci_ops.cpu_up_validate);
	owner_observe(&before);
	ret = mt6797_psci_ops.cpu_up_validate(0, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_validate(7, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_validate(8, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_validate(9, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_validate(8, 1, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	ret = mt6797_psci_ops.cpu_up_validate(9, 0, CPUHP_AP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_non_target_invalid(struct kunit *test)
{
	struct mt6797_a72_transaction transaction;
	int ret;

	ret = mt6797_a72_membership_begin_up(7, CPUHP_ONLINE, 0, NULL, NULL,
					     NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
}

static void mt6797_a72_owner_intermediate_target_invalid(struct kunit *test)
{
	struct mt6797_a72_transaction transaction;
	int ret;

	ret = mt6797_a72_membership_begin_up(8, CPUHP_AP_ONLINE, 0, NULL, NULL,
					     NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
}

static void mt6797_a72_owner_repeat_is_diagnostic(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct mt6797_a72_transaction transaction;
	int ret;

	owner_observe(&before);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_entry_snapshot_gate(struct kunit *test)
{
	const struct mt6797_a72_entry_snapshot cpu8 =
		mt6797_a72_entry_for_up(8);
	const struct mt6797_a72_entry_snapshot cpu9 =
		mt6797_a72_entry_for_up(9);
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_entry_snapshot mutated;

	owner_observe(&before);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&cpu8), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(9, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU9_UP,
				&cpu9), 0);

	mutated = cpu8;
	mutated.members = BIT(0);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&mutated), -EPERM);
	mutated = cpu8;
	mutated.provider_state = MT6797_A72_PROVIDER_HELD;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&mutated), -EPERM);
	mutated = cpu9;
	mutated.online_mask = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(9, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU9_UP,
				&mutated), -EPERM);
	mutated = cpu8;
	mutated.flags &= ~MT6797_A72_ENTRY_CPU9_POSSIBLE;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&mutated), -EPERM);
	mutated = cpu8;
	mutated.cpuhp_state_cpu8 = CPUHP_ONLINE;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&mutated), -EPERM);
	mutated = cpu8;
	mutated.cpu9_mpidr = 0x301;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&mutated), -EPERM);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU9_UP,
				&cpu8), -EPERM);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_AP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&cpu8), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(7, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				&cpu8), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
				MT6797_A72_ATTEMPT_CPU8_UP,
				NULL), -EINVAL);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_p31_consumes_once(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct mt6797_a72_entry_snapshot malformed = entry;
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate prestate9 = mt6797_a72_prestate_for_up(9);
	struct arm64_late_cpu_ready_token bad_ready;
	struct mt6797_a72_transaction transaction;
	bool owns_token;
	int difference;
	int ret;

	mt6797_a72_membership_test_seed_available();
	owner_observe(&before);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, mt6797_a72_transaction_empty(&transaction));
	KUNIT_EXPECT_EQ(test, transaction.identity.abi,
			(u32)MT6797_A72_TRANSACTION_ABI);
	KUNIT_EXPECT_EQ(test, transaction.identity.operation,
			(u16)ARM64_LATE_CPU_STARTUP_OP_CPU8_UP);
	KUNIT_EXPECT_EQ(test, transaction.identity.target_cpu, (u32)8);
	KUNIT_EXPECT_EQ(test, transaction.identity.target_mpidr, (u64)0x200);
	KUNIT_EXPECT_EQ(test, transaction.identity.cpu_on_entry_pa,
			(u64)__pa_symbol(secondary_entry));
	KUNIT_EXPECT_EQ(test, transaction.identity.generation, (u64)1);
	KUNIT_EXPECT_EQ(test, transaction.identity.cookie, (u64)0xa7200001);
	KUNIT_EXPECT_EQ(test, transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, transaction.budgets.cpu_on,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, transaction.budgets.affinity,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_TRUE(test, transaction.p30_token_valid);
	KUNIT_EXPECT_TRUE(test, transaction.a36_valid);
	KUNIT_EXPECT_EQ(test, transaction.a36_prestate.spm_218,
			(u32)MT6797_A72_A36_SPM_218);
	KUNIT_EXPECT_TRUE(test, transaction.valid);
	KUNIT_EXPECT_EQ(test, transaction.p30_token.plan_identity[0],
			(u64)1);
	KUNIT_EXPECT_EQ(test, transaction.p30_token.plan_identity[3],
			(u64)4);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.health,
			(u32)MT6797_A72_OWNER_AVAILABLE);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_FROZEN);
	KUNIT_EXPECT_EQ(test, after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);
	owns_token = mt6797_a72_membership_owns_up_token(&transaction.p30_token);
	KUNIT_EXPECT_TRUE(test, owns_token);
	difference = memcmp(&before.p30, &after.p30, sizeof(before.p30));
	KUNIT_EXPECT_EQ(test, difference, 0);

	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));

	mt6797_a72_membership_test_seed_available();
	malformed = mt6797_a72_entry_for_up(9);
	malformed.members = 0;
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &malformed, &ready, &prestate9, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU9_UP));
	KUNIT_EXPECT_EQ(test, after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU9_UP);

	entry = mt6797_a72_entry_for_up_closed_window(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate9, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU9_UP));
	KUNIT_EXPECT_EQ(test, after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU9_UP);

	mt6797_a72_membership_test_seed_available();
	bad_ready = ready;
	bad_ready.plan_identity[0] = 0;
	entry = mt6797_a72_entry_for_up(8);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &bad_ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_IDLE);
	KUNIT_EXPECT_EQ(test, after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);
}

static void mt6797_a72_owner_a36_rejects_without_rearm(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate retry_prestate;
	struct mt6797_a72_transaction transaction;
	struct mt6797_a72_owner_snapshot snapshot;
	int ret;

	mt6797_a72_membership_test_seed_available();
	prestate.spm_218 ^= 1;
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
	mt6797_a72_membership_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_EQ(test, snapshot.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_EQ(test, snapshot.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, snapshot.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);

	retry_prestate = mt6797_a72_prestate_for_up(8);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready,
					     &retry_prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
}

static void mt6797_a72_owner_a36_prestate_mutations_rejected(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate mutated;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&before);

	mutated = prestate;
	mutated.secondary_entry_pa = 0;
	ret = mt6797_a72_membership_validate_up_prestate(&transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.generation++;
	ret = mt6797_a72_membership_validate_up_prestate(&transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.call_shape = MT6797_A72_A36_CALL_SHAPE_NONE;
	ret = mt6797_a72_membership_validate_up_prestate(&transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.cpu8_cluster_dcm_published = 1;
	ret = mt6797_a72_membership_validate_up_prestate(&transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_cpu9_mints_distinct_token(struct kunit *test)
{
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	struct mt6797_a72_transaction transaction;
	int ret;

	mt6797_a72_membership_test_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transaction.identity.operation,
			(u16)ARM64_LATE_CPU_STARTUP_OP_CPU9_UP);
	KUNIT_EXPECT_EQ(test, transaction.identity.target_cpu, (u32)9);
	KUNIT_EXPECT_EQ(test, transaction.identity.target_mpidr, (u64)0x201);
	KUNIT_EXPECT_EQ(test, transaction.identity.generation, (u64)1);
	KUNIT_EXPECT_EQ(test, transaction.identity.cookie, (u64)0xa7200001);
	KUNIT_EXPECT_EQ(test, transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_EQ(test, transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_EQ(test, transaction.budgets.cpu_on,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_TRUE(test, transaction.p30_token_valid);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.generation,
			(u64)1);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.cookie,
			(u64)0xa72000f0);
}

static void mt6797_a72_owner_p17_cpu8_publishes_once(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.p17_p18_published);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.generation,
			(u64)0);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_TRUE(test, after.owner.active.p17_p18_published);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);

	owner_observe(&before);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_p18_cpu9_preserves_provider(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	struct mt6797_a72_transaction transaction;
	struct mt6797_a72_owner_snapshot snapshot;
	int ret;

	mt6797_a72_membership_test_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.p17_p18_published);
	mt6797_a72_membership_snapshot(&snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, snapshot.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, snapshot.provider_identity.generation,
			(u64)1);
	KUNIT_EXPECT_EQ(test, snapshot.provider_identity.cookie,
			(u64)0xa72000f0);
	KUNIT_EXPECT_EQ(test, snapshot.active.provider_identity.cookie,
			(u64)0xa72000f0);
}

static void mt6797_a72_owner_p18_without_provider_rejected(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	struct mt6797_a72_transaction transaction;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&transaction));
}

static struct mt6797_a72_p27_preparation
mt6797_a72_p27_preparation_for(const struct mt6797_a72_transaction *transaction)
{
	return (struct mt6797_a72_p27_preparation) {
		.abi = MT6797_A72_P27_PREPARATION_ABI,
		.operation = transaction->identity.operation,
		.stage = MT6797_A72_P27_STAGE_COMPLETE,
		.effect_mask = MT6797_A72_P27_EFFECT_MASK,
		.generation = transaction->identity.generation,
		.cookie = transaction->identity.cookie,
	};
}

static int
mt6797_a72_test_seed_cpu8_p27(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_p27_preparation preparation;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_publish_up(transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_begin_p27_preparation(transaction);
	if (ret)
		return ret;
	preparation = mt6797_a72_p27_preparation_for(transaction);
	return mt6797_a72_membership_complete_p27_preparation(transaction,
							     &preparation);
}

static struct mt6797_a72_provider_acquire_proof
mt6797_a72_provider_acquire_proof_for(
	const struct mt6797_a72_transaction *transaction)
{
	return (struct mt6797_a72_provider_acquire_proof) {
		.abi = MT6797_A72_PROVIDER_ACQUIRE_ABI,
		.operation = transaction->identity.operation,
		.settle_us = MT6797_A72_PROVIDER_ACQUIRE_SETTLE_US,
		.da921x_page = MT6797_A72_A36_DA921X_PAGE,
		.buckb_enabled = 1,
		.buckb_vsel = MT6797_A72_A36_BUCKB_VSEL,
		.origin = MT6797_A72_PROVIDER_ORIGIN_M01,
		.held_identity = {
			.generation = 2,
			.cookie = 0xa7200101,
		},
		.transaction_generation = transaction->identity.generation,
		.transaction_cookie = transaction->identity.cookie,
		.origin_generation = 2,
	};
}

static int
mt6797_a72_test_seed_cpu8_r02(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_provider_acquire_proof proof;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_begin_provider_acquire(transaction);
	if (ret)
		return ret;
	proof = mt6797_a72_provider_acquire_proof_for(transaction);
	return mt6797_a72_membership_confirm_provider_acquire(transaction,
							      &proof);
}

static struct mt6797_a72_provider_rejection
mt6797_a72_provider_rejection_for(
	const struct mt6797_a72_transaction *transaction)
{
	return (struct mt6797_a72_provider_rejection) {
		.abi = MT6797_A72_PROVIDER_REJECT_ABI,
		.operation = transaction->identity.operation,
		.result = MT6797_A72_PROVIDER_REJECTED_BEFORE_VOTE,
		.returned = 1,
		.transaction_generation = transaction->identity.generation,
		.transaction_cookie = transaction->identity.cookie,
	};
}

static struct mt6797_a72_p28_preparation
mt6797_a72_p28_preparation_for(
	const struct mt6797_a72_transaction *transaction)
{
	return (struct mt6797_a72_p28_preparation) {
		.abi = MT6797_A72_P28_PREPARATION_ABI,
		.operation = transaction->identity.operation,
		.stage = MT6797_A72_P28_STAGE_COMPLETE,
		.effect_mask = MT6797_A72_P28_EFFECT_MASK,
		.isolation_from = MT6797_A72_P28_ISOLATION_FROM,
		.isolation_to = MT6797_A72_P28_ISOLATION_TO,
		.pwrap_deasserted = 1,
		.software_guard_released = 1,
		.wait_before_sram_us = MT6797_A72_P28_WAIT_US,
		.sram_ldo_mv = MT6797_A72_P28_SRAM_LDO_MV,
		.wait_after_sram_us = MT6797_A72_P28_WAIT_US,
		.selector = MT6797_A72_P28_SELECTOR,
		.calibration_stable = 1,
		.calibration_valid = 1,
		.provider_identity = transaction->provider_identity,
		.transaction_generation = transaction->identity.generation,
		.transaction_cookie = transaction->identity.cookie,
	};
}

static struct mt6797_a72_p29_rollback_proof
mt6797_a72_p29_rollback_for(
	const struct mt6797_a72_transaction *transaction)
{
	return (struct mt6797_a72_p29_rollback_proof) {
		.abi = MT6797_A72_P29_ROLLBACK_ABI,
		.operation = transaction->identity.operation,
		.restored_effect_mask = MT6797_A72_P27_EFFECT_MASK,
		.transaction_generation = transaction->identity.generation,
		.transaction_cookie = transaction->identity.cookie,
	};
}

static void mt6797_a72_owner_p27_cpu8_preparation_once(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_p27_preparation preparation;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p27_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	KUNIT_EXPECT_EQ(test, transaction.p27_preparation.stage,
			(u32)MT6797_A72_P27_STAGE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.generation,
			(u64)0);
	KUNIT_EXPECT_EQ(test, transaction.entry.members, (u32)0);

	preparation = mt6797_a72_p27_preparation_for(&transaction);
	ret = mt6797_a72_membership_complete_p27_preparation(&transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.p27_valid);
	KUNIT_EXPECT_EQ(test, transaction.p27_preparation.stage,
			(u32)MT6797_A72_P27_STAGE_COMPLETE);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);

	owner_observe(&before);
	ret = mt6797_a72_membership_begin_p27_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_p27_rejects_bad_or_cpu9_proof(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_p27_preparation preparation;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	mt6797_a72_membership_test_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p27_preparation(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	preparation = mt6797_a72_p27_preparation_for(&transaction);
	preparation.effect_mask ^= MT6797_A72_P27_BPLL_ORDER_READ;
	owner_observe(&before);
	ret = mt6797_a72_membership_complete_p27_preparation(&transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);

	mt6797_a72_membership_test_seed_available_cpu9();
	entry = mt6797_a72_entry_for_up(9);
	prestate = mt6797_a72_prestate_for_up(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&before);
	ret = mt6797_a72_membership_begin_p27_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_r01_r02_provider_once(struct kunit *test)
{
	struct mt6797_a72_provider_acquire_proof proof;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, after.owner.provider_identity.generation,
			(u64)0);

	proof = mt6797_a72_provider_acquire_proof_for(&transaction);
	ret = mt6797_a72_membership_confirm_provider_acquire(&transaction,
							     &proof);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.provider_acquire_valid);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.generation,
			(u64)2);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.cookie,
			(u64)0xa7200101);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, after.owner.active.provider_identity.cookie,
			(u64)0xa7200101);

	owner_observe(&before);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_r02_rejects_bad_proof_or_cpu9(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	struct mt6797_a72_provider_acquire_proof proof;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	proof = mt6797_a72_provider_acquire_proof_for(&transaction);
	proof.da921x_page ^= 1;
	owner_observe(&before);
	ret = mt6797_a72_membership_confirm_provider_acquire(&transaction,
							     &proof);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);

	mt6797_a72_membership_test_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&before);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_p28_cpu8_preparation_once(struct kunit *test)
{
	struct mt6797_a72_p28_preparation preparation;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_r02(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p28_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transaction.budgets.postprovider_preparation,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	KUNIT_EXPECT_EQ(test, transaction.p28_preparation.stage,
			(u32)MT6797_A72_P28_STAGE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, transaction.provider_identity.generation,
			(u64)2);

	preparation = mt6797_a72_p28_preparation_for(&transaction);
	ret = mt6797_a72_membership_complete_p28_preparation(&transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.p28_valid);
	KUNIT_EXPECT_EQ(test, transaction.p28_preparation.stage,
			(u32)MT6797_A72_P28_STAGE_COMPLETE);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);

	owner_observe(&before);
	ret = mt6797_a72_membership_begin_p28_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_p28_rejects_bad_proof_or_cpu9(struct kunit *test)
{
	struct mt6797_a72_p28_preparation preparation;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_r02(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p28_preparation(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	preparation = mt6797_a72_p28_preparation_for(&transaction);
	preparation.wait_after_sram_us++;
	owner_observe(&before);
	ret = mt6797_a72_membership_complete_p28_preparation(&transaction,
							      &preparation);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);

	mt6797_a72_membership_test_seed_available_cpu9();
	{
		struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
		struct arm64_late_cpu_ready_token ready =
			mt6797_a72_ready_token_for_up();
		struct mt6797_a72_a36_prestate prestate =
			mt6797_a72_prestate_for_up(9);

		ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
						     MT6797_A72_ATTEMPT_CPU9_UP,
						     &entry, &ready, &prestate,
						     &transaction);
		KUNIT_ASSERT_EQ(test, ret, 0);
		ret = mt6797_a72_membership_publish_up(&transaction);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	owner_observe(&before);
	ret = mt6797_a72_membership_begin_p28_preparation(&transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_r03_p29_rejects_and_retires(struct kunit *test)
{
	struct mt6797_a72_provider_rejection rejection;
	struct mt6797_a72_p29_rollback_proof rollback;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rejection = mt6797_a72_provider_rejection_for(&transaction);
	ret = mt6797_a72_membership_reject_provider_acquire(&transaction,
							    &rejection);
	KUNIT_EXPECT_EQ(test, ret, 0);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_TRUE(test, after.owner.active.provider_rejection_valid);
	KUNIT_EXPECT_FALSE(test, after.owner.active.p29_valid);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);

	rollback = mt6797_a72_p29_rollback_for(&transaction);
	ret = mt6797_a72_membership_complete_p29_rollback(&transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, 0);
	owner_observe(&after);
	KUNIT_EXPECT_EQ(test, after.owner.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_EQ(test, after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, after.owner.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_FALSE(test, after.owner.active.valid);
	KUNIT_EXPECT_TRUE(test, after.owner.retired[0].p29_valid);

	owner_observe(&before);
	ret = mt6797_a72_membership_complete_p29_rollback(&transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_r03_p29_mutations_rejected(struct kunit *test)
{
	struct mt6797_a72_entry_snapshot entry;
	struct arm64_late_cpu_ready_token ready;
	struct mt6797_a72_a36_prestate prestate;
	struct mt6797_a72_provider_rejection rejection;
	struct mt6797_a72_p29_rollback_proof rollback;
	struct mt6797_a72_transaction transaction;
	struct owner_observation before;
	struct owner_observation after;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rejection = mt6797_a72_provider_rejection_for(&transaction);
	rejection.vote_requested = 1;
	owner_observe(&before);
	ret = mt6797_a72_membership_reject_provider_acquire(&transaction,
							    &rejection);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);

	rejection = mt6797_a72_provider_rejection_for(&transaction);
	ret = mt6797_a72_membership_reject_provider_acquire(&transaction,
							    &rejection);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rollback = mt6797_a72_p29_rollback_for(&transaction);
	rollback.residual_effect_mask = MT6797_A72_P27_PWRAP_ASSERTED;
	owner_observe(&before);
	ret = mt6797_a72_membership_complete_p29_rollback(&transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);

	mt6797_a72_membership_test_seed_available_cpu9();
	entry = mt6797_a72_entry_for_up(9);
	ready = mt6797_a72_ready_token_for_up();
	prestate = mt6797_a72_prestate_for_up(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate,
					     &transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_complete_p29_rollback(&transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
}

static void mt6797_a72_owner_forged_token_rejected(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct arm64_late_cpu_up_token token = {
		.abi = ARM64_LATE_CPU_STARTUP_ABI,
		.owner = ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP,
		.operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP,
		.target_cpu = 8,
		.target_mpidr = 0x200,
		.generation = 42,
		.cookie = 0x8a720042,
		.plan_identity = { 1, 2, 3, 4 },
	};
	bool owned;

	owner_observe(&before);
	owned = mt6797_a72_membership_owns_up_token(&token);
	KUNIT_EXPECT_FALSE(test, owned);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static void mt6797_a72_owner_no_live_token(struct kunit *test)
{
	struct owner_observation before;
	struct owner_observation after;
	struct arm64_late_cpu_up_token token;
	int ret;

	owner_observe(&before);
	memset(&token, 0xa5, sizeof(token));
	ret = mt6797_a72_membership_copy_up_token(&token);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_EXPECT_EQ(test, token.abi, (u32)0);
	KUNIT_EXPECT_EQ(test, token.generation, (u64)0);
	KUNIT_EXPECT_EQ(test, token.cookie, (u64)0);
	owner_observe(&after);
	expect_unchanged(test, &before, &after);
}

static struct kunit_case mt6797_a72_owner_cases[] = {
	KUNIT_CASE(mt6797_a72_owner_initial_closed),
	KUNIT_CASE(mt6797_a72_owner_cpu8_denied),
	KUNIT_CASE(mt6797_a72_owner_cpu9_denied),
	KUNIT_CASE(mt6797_a72_owner_public_hook_denied),
	KUNIT_CASE(mt6797_a72_owner_internal_hook_denied),
	KUNIT_CASE(mt6797_a72_owner_non_target_invalid),
	KUNIT_CASE(mt6797_a72_owner_intermediate_target_invalid),
	KUNIT_CASE(mt6797_a72_owner_repeat_is_diagnostic),
	KUNIT_CASE(mt6797_a72_owner_entry_snapshot_gate),
	KUNIT_CASE(mt6797_a72_owner_p31_consumes_once),
	KUNIT_CASE(mt6797_a72_owner_a36_rejects_without_rearm),
	KUNIT_CASE(mt6797_a72_owner_a36_prestate_mutations_rejected),
	KUNIT_CASE(mt6797_a72_owner_cpu9_mints_distinct_token),
	KUNIT_CASE(mt6797_a72_owner_p17_cpu8_publishes_once),
	KUNIT_CASE(mt6797_a72_owner_p18_cpu9_preserves_provider),
	KUNIT_CASE(mt6797_a72_owner_p18_without_provider_rejected),
	KUNIT_CASE(mt6797_a72_owner_p27_cpu8_preparation_once),
	KUNIT_CASE(mt6797_a72_owner_p27_rejects_bad_or_cpu9_proof),
	KUNIT_CASE(mt6797_a72_owner_r01_r02_provider_once),
	KUNIT_CASE(mt6797_a72_owner_r02_rejects_bad_proof_or_cpu9),
	KUNIT_CASE(mt6797_a72_owner_p28_cpu8_preparation_once),
	KUNIT_CASE(mt6797_a72_owner_p28_rejects_bad_proof_or_cpu9),
	KUNIT_CASE(mt6797_a72_owner_r03_p29_rejects_and_retires),
	KUNIT_CASE(mt6797_a72_owner_r03_p29_mutations_rejected),
	KUNIT_CASE(mt6797_a72_owner_forged_token_rejected),
	KUNIT_CASE(mt6797_a72_owner_no_live_token),
	{ }
};

static struct kunit_suite mt6797_a72_owner_suite = {
	.name = "mt6797-a72-p24-owner",
	.init = mt6797_a72_owner_test_init,
	.test_cases = mt6797_a72_owner_cases,
};

kunit_test_suite(mt6797_a72_owner_suite);

MODULE_LICENSE("GPL");
