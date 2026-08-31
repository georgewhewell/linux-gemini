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

struct mt6797_a72_owner_test_state {
	struct owner_observation before;
	struct owner_observation after;
	struct mt6797_a72_transaction transaction;
	struct mt6797_a72_owner_snapshot snapshot;
};

static const struct mt6797_a72_transaction mt6797_a72_empty_transaction;

static bool
mt6797_a72_transaction_empty(const struct mt6797_a72_transaction *transaction)
{
	return !memchr_inv(transaction, 0, sizeof(*transaction));
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
		.secondary_entry_pa = __pa_symbol(secondary_entry),
		.generation = 1,
		.cookie = 0xa7200001,
	};

	if (cpu == 8) {
		prestate.operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
		prestate.buckb_vsel = MT6797_A72_A36_BUCKB_VSEL;
		prestate.spm_218 = MT6797_A72_A36_SPM_218;
		prestate.spm_290 = MT6797_A72_A36_SPM_290;
		prestate.protected_clock_valid = 1;
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
		.observed_target_mpidr = { 0, 0 },
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

static void mt6797_a72_owner_reset_state(void)
{
	arm64_late_cpu_startup_test_reset();
	mt6797_a72_membership_test_reset();
}

static void mt6797_a72_owner_seed_available(void)
{
	arm64_late_cpu_startup_test_reset();
	mt6797_a72_membership_test_seed_available();
}

static void mt6797_a72_owner_seed_available_cpu9(void)
{
	arm64_late_cpu_startup_test_reset();
	mt6797_a72_membership_test_seed_available_cpu9();
}

static int mt6797_a72_owner_test_init(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	test->priv = state;
	mt6797_a72_owner_reset_state();
	return 0;
}

static void mt6797_a72_owner_initial_closed(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	const struct mt6797_a72_provider_identity empty_provider = { };
	const struct mt6797_a72_fault_record empty_fault = { };
	unsigned int slot;
	int difference;

	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.abi,
			(u32)MT6797_A72_TRANSACTION_ABI);
	KUNIT_EXPECT_EQ(test, state->snapshot.health,
			(u32)MT6797_A72_OWNER_CLOSED);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_UNINITIALIZED);
	KUNIT_EXPECT_FALSE(test, state->snapshot.bootstrap_valid);
	KUNIT_EXPECT_FALSE(test, state->snapshot.members_valid);
	KUNIT_EXPECT_EQ(test, state->snapshot.members, (u32)0);
	KUNIT_EXPECT_EQ(test, state->snapshot.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, state->snapshot.attempts_available, (u32)0);
	KUNIT_EXPECT_EQ(test, state->snapshot.attempts_consumed, (u32)0);
	KUNIT_EXPECT_EQ(test, state->snapshot.retired_mask, (u32)0);
	KUNIT_EXPECT_FALSE(test, state->snapshot.controller_present);
	KUNIT_EXPECT_EQ(test, state->snapshot.controller_cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, state->snapshot.diagnostic_blockers,
			(u64)MT6797_A72_BLOCK_MASK);
	difference = memcmp(&state->snapshot.active, &mt6797_a72_empty_transaction,
			    sizeof(mt6797_a72_empty_transaction));
	KUNIT_EXPECT_EQ(test, difference, 0);
	for (slot = 0; slot < MT6797_A72_OPERATION_SLOTS; slot++) {
		difference = memcmp(&state->snapshot.retired[slot], &mt6797_a72_empty_transaction,
				    sizeof(mt6797_a72_empty_transaction));
		KUNIT_EXPECT_EQ(test, difference, 0);
	}
	difference = memcmp(&state->snapshot.provider_identity, &empty_provider,
			    sizeof(empty_provider));
	KUNIT_EXPECT_EQ(test, difference, 0);
	difference = memcmp(&state->snapshot.first_fault, &empty_fault,
			    sizeof(empty_fault));
	KUNIT_EXPECT_EQ(test, difference, 0);
}

static void mt6797_a72_owner_cpu8_denied(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	int ret;

	owner_observe(&state->before);
	memset(&state->transaction, 0xa5, sizeof(state->transaction));
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_cpu9_denied(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	int ret;

	owner_observe(&state->before);
	memset(&state->transaction, 0x5a, sizeof(state->transaction));
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, NULL, NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_public_hook_denied(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mt6797_psci_ops.cpu_up_preflight);
	owner_observe(&state->before);
	ret = mt6797_psci_ops.cpu_up_preflight(0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_preflight(7, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_preflight(8, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_preflight(9, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_preflight(8, CPUHP_AP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret,
			IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER) ?
				-EAGAIN : -EINVAL);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_internal_hook_denied(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, mt6797_psci_ops.cpu_up_validate);
	owner_observe(&state->before);
	ret = mt6797_psci_ops.cpu_up_validate(0, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_validate(7, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_psci_ops.cpu_up_validate(8, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_validate(9, 0, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_psci_ops.cpu_up_validate(8, 1, CPUHP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret,
			IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER) ?
				-EAGAIN : -EPERM);
	ret = mt6797_psci_ops.cpu_up_validate(9, 0, CPUHP_AP_ONLINE);
	KUNIT_EXPECT_EQ(test, ret,
			IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER) ?
				-EAGAIN : -EINVAL);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_non_target_invalid(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	ret = mt6797_a72_membership_begin_up(7, CPUHP_ONLINE, 0, NULL, NULL,
					     NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
}

static void mt6797_a72_owner_intermediate_target_invalid(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	ret = mt6797_a72_membership_begin_up(8, CPUHP_AP_ONLINE, 0, NULL, NULL,
					     NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
}

static void mt6797_a72_owner_repeat_is_diagnostic(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	int ret;

	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, NULL, NULL, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_entry_snapshot_gate(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	const struct mt6797_a72_entry_snapshot cpu8 =
		mt6797_a72_entry_for_up(8);
	const struct mt6797_a72_entry_snapshot cpu9 =
		mt6797_a72_entry_for_up(9);
	struct mt6797_a72_entry_snapshot mutated;

	owner_observe(&state->before);
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
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_p31_consumes_once(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct mt6797_a72_entry_snapshot malformed = entry;
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate prestate9 = mt6797_a72_prestate_for_up(9);
	struct arm64_late_cpu_ready_token bad_ready;
	bool owns_token;
	int difference;
	int ret;

	mt6797_a72_owner_seed_available();
	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, mt6797_a72_transaction_empty(&state->transaction));
	KUNIT_EXPECT_EQ(test, state->transaction.identity.abi,
			(u32)MT6797_A72_TRANSACTION_ABI);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.operation,
			(u16)ARM64_LATE_CPU_STARTUP_OP_CPU8_UP);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.target_cpu, (u32)8);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.target_mpidr, (u64)0x200);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.cpu_on_entry_pa,
			(u64)__pa_symbol(secondary_entry));
	KUNIT_EXPECT_EQ(test, state->transaction.identity.generation, (u64)1);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.cookie, (u64)0xa7200001);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.cpu_on,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.affinity,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_TRUE(test, state->transaction.p30_token_valid);
	KUNIT_EXPECT_TRUE(test, state->transaction.a36_valid);
	KUNIT_EXPECT_EQ(test, state->transaction.a36_prestate.spm_218,
			(u32)MT6797_A72_A36_SPM_218);
	KUNIT_EXPECT_TRUE(test, state->transaction.valid);
	KUNIT_EXPECT_EQ(test, state->transaction.p30_token.plan_identity[0],
			(u64)1);
	KUNIT_EXPECT_EQ(test, state->transaction.p30_token.plan_identity[3],
			(u64)4);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.health,
			(u32)MT6797_A72_OWNER_AVAILABLE);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_FROZEN);
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);
	owns_token = mt6797_a72_membership_owns_up_token(&state->transaction.p30_token);
	KUNIT_EXPECT_TRUE(test, owns_token);
	difference = memcmp(&state->before.p30, &state->after.p30, sizeof(state->before.p30));
	KUNIT_EXPECT_EQ(test, difference, 0);

	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));

	mt6797_a72_owner_seed_available();
	malformed = mt6797_a72_entry_for_up(9);
	malformed.members = 0;
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &malformed, &ready, &prestate9, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU9_UP));
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU9_UP);

	entry = mt6797_a72_entry_for_up_closed_window(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate9, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU9_UP));
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU9_UP);

	mt6797_a72_owner_seed_available();
	bad_ready = ready;
	memset(bad_ready.plan_identity, 0,
	       sizeof(bad_ready.plan_identity));
	entry = mt6797_a72_entry_for_up(8);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &bad_ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_IDLE);
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, state->after.owner.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);
}

static void mt6797_a72_owner_a36_rejects_without_rearm(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate retry_prestate;
	int ret;

	mt6797_a72_owner_seed_available();
	prestate.spm_218 ^= 1;
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_EQ(test, state->snapshot.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_EQ(test, state->snapshot.attempts_available,
			(u32)(MT6797_A72_ATTEMPT_MASK &
			       ~MT6797_A72_ATTEMPT_CPU8_UP));
	KUNIT_EXPECT_EQ(test, state->snapshot.attempts_consumed,
			(u32)MT6797_A72_ATTEMPT_CPU8_UP);

	retry_prestate = mt6797_a72_prestate_for_up(8);
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready,
					     &retry_prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
}

static void mt6797_a72_owner_a36_prestate_mutations_rejected(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_a36_prestate mutated;
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&state->before);

	mutated = prestate;
	mutated.secondary_entry_pa = 0;
	ret = mt6797_a72_membership_validate_up_prestate(&state->transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.generation++;
	ret = mt6797_a72_membership_validate_up_prestate(&state->transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.call_shape = MT6797_A72_A36_CALL_SHAPE_NONE;
	ret = mt6797_a72_membership_validate_up_prestate(&state->transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	mutated = prestate;
	mutated.cpu8_cluster_dcm_published = 1;
	ret = mt6797_a72_membership_validate_up_prestate(&state->transaction, &mutated);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_cpu9_mints_distinct_token(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	int ret;

	mt6797_a72_owner_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.operation,
			(u16)ARM64_LATE_CPU_STARTUP_OP_CPU9_UP);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.target_cpu, (u32)9);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.target_mpidr, (u64)0x201);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.generation, (u64)1);
	KUNIT_EXPECT_EQ(test, state->transaction.identity.cookie, (u64)0xa7200001);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_NONE);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.cpu_on,
			(u8)MT6797_A72_BUDGET_AVAILABLE);
	KUNIT_EXPECT_TRUE(test, state->transaction.p30_token_valid);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.generation,
			(u64)1);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.cookie,
			(u64)0xa72000f0);
}

static void mt6797_a72_owner_p17_cpu8_publishes_once(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, state->transaction.p17_p18_published);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.generation,
			(u64)0);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_TRUE(test, state->after.owner.active.p17_p18_published);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);

	owner_observe(&state->before);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_p18_cpu9_preserves_provider(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	int ret;

	mt6797_a72_owner_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, state->transaction.p17_p18_published);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, state->snapshot.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, state->snapshot.provider_identity.generation,
			(u64)1);
	KUNIT_EXPECT_EQ(test, state->snapshot.provider_identity.cookie,
			(u64)0xa72000f0);
	KUNIT_EXPECT_EQ(test, state->snapshot.active.provider_identity.cookie,
			(u64)0xa72000f0);
}

static void mt6797_a72_owner_p18_without_provider_rejected(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, mt6797_a72_transaction_empty(&state->transaction));
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

	mt6797_a72_owner_seed_available();
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
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_p27_preparation preparation;
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p27_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.preparation,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	KUNIT_EXPECT_EQ(test, state->transaction.p27_preparation.stage,
			(u32)MT6797_A72_P27_STAGE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.generation,
			(u64)0);
	KUNIT_EXPECT_EQ(test, state->transaction.entry.members, (u32)0);

	preparation = mt6797_a72_p27_preparation_for(&state->transaction);
	ret = mt6797_a72_membership_complete_p27_preparation(&state->transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, state->transaction.p27_valid);
	KUNIT_EXPECT_EQ(test, state->transaction.p27_preparation.stage,
			(u32)MT6797_A72_P27_STAGE_COMPLETE);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);

	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_p27_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_p27_rejects_bad_or_cpu9_proof(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	struct mt6797_a72_p27_preparation preparation;
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p27_preparation(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	preparation = mt6797_a72_p27_preparation_for(&state->transaction);
	preparation.effect_mask ^= MT6797_A72_P27_BPLL_ORDER_READ;
	owner_observe(&state->before);
	ret = mt6797_a72_membership_complete_p27_preparation(&state->transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);

	mt6797_a72_owner_seed_available_cpu9();
	entry = mt6797_a72_entry_for_up(9);
	prestate = mt6797_a72_prestate_for_up(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_p27_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_r01_r02_provider_once(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_provider_acquire_proof proof;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.provider_acquire,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_identity.generation,
			(u64)0);

	proof = mt6797_a72_provider_acquire_proof_for(&state->transaction);
	ret = mt6797_a72_membership_confirm_provider_acquire(&state->transaction,
							     &proof);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, state->transaction.provider_acquire_valid);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.generation,
			(u64)2);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.cookie,
			(u64)0xa7200101);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, state->after.owner.active.provider_identity.cookie,
			(u64)0xa7200101);

	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_r02_rejects_bad_proof_or_cpu9(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(9);
	struct mt6797_a72_provider_acquire_proof proof;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	proof = mt6797_a72_provider_acquire_proof_for(&state->transaction);
	proof.da921x_page ^= 1;
	owner_observe(&state->before);
	ret = mt6797_a72_membership_confirm_provider_acquire(&state->transaction,
							     &proof);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);

	mt6797_a72_owner_seed_available_cpu9();
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate, &state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_p28_cpu8_preparation_once(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_p28_preparation preparation;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_r02(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p28_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.postprovider_preparation,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	KUNIT_EXPECT_EQ(test, state->transaction.p28_preparation.stage,
			(u32)MT6797_A72_P28_STAGE_INFLIGHT);
	KUNIT_EXPECT_EQ(test, state->transaction.provider_identity.generation,
			(u64)2);

	preparation = mt6797_a72_p28_preparation_for(&state->transaction);
	ret = mt6797_a72_membership_complete_p28_preparation(&state->transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, state->transaction.p28_valid);
	KUNIT_EXPECT_EQ(test, state->transaction.p28_preparation.stage,
			(u32)MT6797_A72_P28_STAGE_COMPLETE);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_HELD);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);

	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_p28_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_p28_rejects_bad_proof_or_cpu9(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_p28_preparation preparation;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_r02(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_p28_preparation(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	preparation = mt6797_a72_p28_preparation_for(&state->transaction);
	preparation.wait_after_sram_us++;
	owner_observe(&state->before);
	ret = mt6797_a72_membership_complete_p28_preparation(&state->transaction,
							     &preparation);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);

	mt6797_a72_owner_seed_available_cpu9();
	{
		struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(9);
		struct arm64_late_cpu_ready_token ready =
			mt6797_a72_ready_token_for_up();
		struct mt6797_a72_a36_prestate prestate =
			mt6797_a72_prestate_for_up(9);

		ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
						     MT6797_A72_ATTEMPT_CPU9_UP,
						     &entry, &ready, &prestate,
						     &state->transaction);
		KUNIT_ASSERT_EQ(test, ret, 0);
		ret = mt6797_a72_membership_publish_up(&state->transaction);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	owner_observe(&state->before);
	ret = mt6797_a72_membership_begin_p28_preparation(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_r03_p29_rejects_and_retires(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_provider_rejection rejection;
	struct mt6797_a72_p29_rollback_proof rollback;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_preflight_up(8, CPUHP_ONLINE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_validate_up(8, 0, CPUHP_ONLINE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_claim_cpu8(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rejection = mt6797_a72_provider_rejection_for(&state->transaction);
	ret = mt6797_a72_membership_reject_provider_acquire(&state->transaction,
							    &rejection);
	KUNIT_EXPECT_EQ(test, ret, 0);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_ON_ISSUED);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_TRUE(test, state->after.owner.active.provider_rejection_valid);
	KUNIT_EXPECT_FALSE(test, state->after.owner.active.p29_valid);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);

	rollback = mt6797_a72_p29_rollback_for(&state->transaction);
	ret = mt6797_a72_membership_complete_p29_rollback(&state->transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, 0);
	owner_observe(&state->after);
	KUNIT_EXPECT_EQ(test, state->after.owner.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_EQ(test, state->after.owner.provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, state->after.owner.members, (u32)0);
	KUNIT_EXPECT_EQ(test, state->after.owner.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_FALSE(test, state->after.owner.active.valid);
	KUNIT_EXPECT_TRUE(test, state->after.owner.retired[0].p29_valid);

	owner_observe(&state->before);
	ret = mt6797_a72_membership_complete_p29_rollback(&state->transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_r03_p29_mutations_rejected(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_entry_snapshot entry;
	struct arm64_late_cpu_ready_token ready;
	struct mt6797_a72_a36_prestate prestate;
	struct mt6797_a72_provider_rejection rejection;
	struct mt6797_a72_p29_rollback_proof rollback;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_p27(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_preflight_up(8, CPUHP_ONLINE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_validate_up(8, 0, CPUHP_ONLINE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_claim_cpu8(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_provider_acquire(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rejection = mt6797_a72_provider_rejection_for(&state->transaction);
	rejection.vote_requested = 1;
	owner_observe(&state->before);
	ret = mt6797_a72_membership_reject_provider_acquire(&state->transaction,
							    &rejection);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);

	rejection = mt6797_a72_provider_rejection_for(&state->transaction);
	ret = mt6797_a72_membership_reject_provider_acquire(&state->transaction,
							    &rejection);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rollback = mt6797_a72_p29_rollback_for(&state->transaction);
	rollback.residual_effect_mask = MT6797_A72_P27_PWRAP_ASSERTED;
	owner_observe(&state->before);
	ret = mt6797_a72_membership_complete_p29_rollback(&state->transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);

	mt6797_a72_owner_seed_available_cpu9();
	entry = mt6797_a72_entry_for_up(9);
	ready = mt6797_a72_ready_token_for_up();
	prestate = mt6797_a72_prestate_for_up(9);
	ret = mt6797_a72_membership_begin_up(9, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU9_UP,
					     &entry, &ready, &prestate,
					     &state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_up(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_complete_p29_rollback(&state->transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);
}

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER)
static int
mt6797_a72_test_seed_cpu8_claimed(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_entry_snapshot entry = mt6797_a72_entry_for_up(8);
	struct arm64_late_cpu_ready_token ready = mt6797_a72_ready_token_for_up();
	struct mt6797_a72_a36_prestate prestate = mt6797_a72_prestate_for_up(8);
	int ret;

	mt6797_a72_owner_seed_available();
	ret = mt6797_a72_membership_begin_up(8, CPUHP_ONLINE,
					     MT6797_A72_ATTEMPT_CPU8_UP,
					     &entry, &ready, &prestate,
					     transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_publish_up(transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_preflight_up(8, CPUHP_ONLINE);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_validate_up(8, 0, CPUHP_ONLINE);
	if (ret)
		return ret;
	return mt6797_a72_membership_claim_cpu8(transaction);
}

static int
mt6797_a72_test_seed_cpu8_claimed_p27(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_p27_preparation preparation;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed(transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_begin_p27_preparation(transaction);
	if (ret)
		return ret;
	preparation = mt6797_a72_p27_preparation_for(transaction);
	return mt6797_a72_membership_complete_p27_preparation(transaction,
							     &preparation);
}

static int
mt6797_a72_test_seed_cpu8_claimed_p28(struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_provider_acquire_proof proof;
	struct mt6797_a72_p28_preparation preparation;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed_p27(transaction);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_begin_provider_acquire(transaction);
	if (ret)
		return ret;
	proof = mt6797_a72_provider_acquire_proof_for(transaction);
	ret = mt6797_a72_membership_confirm_provider_acquire(transaction, &proof);
	if (ret)
		return ret;
	ret = mt6797_a72_membership_begin_p28_preparation(transaction);
	if (ret)
		return ret;
	preparation = mt6797_a72_p28_preparation_for(transaction);
	return mt6797_a72_membership_complete_p28_preparation(transaction,
							     &preparation);
}

static void mt6797_a72_owner_binder_success_handoff(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed_p28(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_cpu8_on(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->transaction.budgets.cpu_on,
			(u8)MT6797_A72_BUDGET_CONSUMED);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_begin_cpu8_on(&state->transaction),
			-EPERM);
	ret = mt6797_a72_membership_test_publish_cpu8_success(&state->transaction,
							      true, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_VERIFYING);
	KUNIT_EXPECT_EQ(test, state->snapshot.members, (u32)BIT(0));
	KUNIT_EXPECT_TRUE(test, state->snapshot.active.cpu8_success_published);

	ret = mt6797_a72_membership_test_finalize_cpu8_success(&state->transaction,
							       true, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase, (u32)MT6797_A72_PHASE_IDLE);
	KUNIT_EXPECT_EQ(test, state->snapshot.members, (u32)BIT(0));
	KUNIT_EXPECT_EQ(test, state->snapshot.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_FALSE(test, state->snapshot.active.valid);
	KUNIT_EXPECT_TRUE(test, state->snapshot.retired[0].cpu8_success_published);
}

static void mt6797_a72_owner_binder_p32_from_verifying(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct cpu_up_rollback_trace trace = { };
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed_p28(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_begin_cpu8_on(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_test_publish_cpu8_success(&state->transaction,
							      true, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_publish_p32(8, CPUHP_ONLINE,
						-ENOSPC, &trace);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase, (u32)MT6797_A72_PHASE_FAULT);
	KUNIT_EXPECT_EQ(test, state->snapshot.members, (u32)BIT(0));
	KUNIT_EXPECT_TRUE(test, state->snapshot.active.p32_valid);
	KUNIT_EXPECT_EQ(test, state->snapshot.active.p32.cpu_up_error, -ENOSPC);
}

static void mt6797_a72_owner_binder_clean_rejection(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_membership_reject_cpu8(&state->transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_EQ(test, state->snapshot.members, (u32)0);
	KUNIT_EXPECT_EQ(test, state->snapshot.retired_mask, (u32)BIT(0));
	KUNIT_EXPECT_FALSE(test, state->snapshot.active.valid);
}

static void mt6797_a72_owner_binder_p29_without_provider(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct mt6797_a72_p29_rollback_proof rollback;
	int ret;

	ret = mt6797_a72_test_seed_cpu8_claimed_p27(&state->transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rollback = mt6797_a72_p29_rollback_for(&state->transaction);
	ret = mt6797_a72_membership_complete_p29_rollback(&state->transaction,
							  &rollback);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mt6797_a72_membership_snapshot(&state->snapshot);
	KUNIT_EXPECT_EQ(test, state->snapshot.phase,
			(u32)MT6797_A72_PHASE_REJECTED);
	KUNIT_EXPECT_TRUE(test, state->snapshot.retired[0].p29_valid);
	KUNIT_EXPECT_FALSE(test, state->snapshot.retired[0].provider_acquire_valid);
}
#endif

static void mt6797_a72_owner_forged_token_rejected(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
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

	owner_observe(&state->before);
	owned = mt6797_a72_membership_owns_up_token(&token);
	KUNIT_EXPECT_FALSE(test, owned);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
}

static void mt6797_a72_owner_no_live_token(struct kunit *test)
{
	struct mt6797_a72_owner_test_state *state = test->priv;
	struct arm64_late_cpu_up_token token;
	int ret;

	owner_observe(&state->before);
	memset(&token, 0xa5, sizeof(token));
	ret = mt6797_a72_membership_copy_up_token(&token);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_EXPECT_EQ(test, token.abi, (u32)0);
	KUNIT_EXPECT_EQ(test, token.generation, (u64)0);
	KUNIT_EXPECT_EQ(test, token.cookie, (u64)0);
	owner_observe(&state->after);
	expect_unchanged(test, &state->before, &state->after);
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
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER)
	KUNIT_CASE(mt6797_a72_owner_binder_success_handoff),
	KUNIT_CASE(mt6797_a72_owner_binder_p32_from_verifying),
	KUNIT_CASE(mt6797_a72_owner_binder_clean_rejection),
	KUNIT_CASE(mt6797_a72_owner_binder_p29_without_provider),
#endif
	KUNIT_CASE(mt6797_a72_owner_forged_token_rejected),
	KUNIT_CASE(mt6797_a72_owner_no_live_token),
	{ }
};

static struct kunit_suite mt6797_a72_owner_suite __maybe_unused = {
	.name = "mt6797-a72-p24-owner",
	.init = mt6797_a72_owner_test_init,
	.test_cases = mt6797_a72_owner_cases,
};

#ifdef CONFIG_ARM64_MT6797_A72_ATOMIC_PUBLICATION_KUNIT_TEST
struct atomic_publication_test_state {
	struct mt6797_a72_direct_source_snapshot source;
	struct mt6797_a72_owner_snapshot owner_before;
	struct mt6797_a72_owner_snapshot owner_after;
	struct arm64_late_cpu_startup_snapshot p30_before;
	struct arm64_late_cpu_startup_snapshot p30_after;
	struct mt6797_a72_direct_topology topology;
	struct mt6797_a72_a34_replay replay;
	int source_result;
	int finalizer_result;
	u32 source_calls;
	u32 finalizer_calls;
};

static struct mt6797_a72_direct_topology atomic_topology(void)
{
	return (struct mt6797_a72_direct_topology) {
		.cpu8_possible = 1,
		.cpu9_possible = 1,
		.cpu8_present = 1,
		.cpu9_present = 1,
		.cpu8_method_valid = 1,
		.cpu9_method_valid = 1,
		.cpu8_mpidr = 0x200,
		.cpu9_mpidr = 0x201,
	};
}

static struct mt6797_a72_direct_source_snapshot atomic_source(void)
{
	return (struct mt6797_a72_direct_source_snapshot) {
		.abi = MT6797_A72_DIRECT_SOURCE_ABI,
		.valid = 1,
		.provider = {
			.abi = MT6797_A72_PROVIDER_STATE_ABI,
			.valid = 1,
			.control_a = 0x7b,
			.status_b = 0xc1,
			.vbuckb_a = 0x46,
			.vbuckb_b = 0x46,
		},
		.platform = {
			.spm_pwr_status = 0x2a00005c,
			.spm_pwr_status_2nd = 0x2a00004c,
			.spm_cpu_pwr_status = 0x003dce08,
			.spm_cpu_pwr_status_2nd = 0x003dceff,
			.spm_mp2_cpusys_pwr_con = 0x00010132,
			.spm_mp2_cpu0_pwr_con = 0x00010332,
			.spm_mp2_cpu1_pwr_con = 0x00010332,
			.spm_cpu_ext_buck_iso = 0x00000002,
			.cci_mp2_port_control = 0xc0000000,
			.valid = true,
		},
		.clock = {
			.abi = MT6797_DVFSP_CLOCK_BACKEND_ABI,
			.sample_generation = 1,
		},
		.bigidvfs = {
			.abi = MT6797_BIGIDVFS_BACKEND_ABI,
			.sample_generation = 1,
		},
	};
}

static struct mt6797_a72_a34_replay atomic_replay(void)
{
	return (struct mt6797_a72_a34_replay) {
		.abi = MT6797_A72_A34_REPLAY_ABI,
		.valid = 1,
		.proof =
			MT6797_A72_A34_REPLAY_APPLICABLE_PRIMARY_BL31_CLEAR,
	};
}

static int
atomic_source_snapshot(void *context,
		       struct mt6797_a72_direct_source_snapshot *snapshot)
{
	struct atomic_publication_test_state *state = context;

	state->source_calls++;
	if (state->source_result)
		return state->source_result;
	*snapshot = state->source;
	return 0;
}

static const struct mt6797_a72_direct_source_ops atomic_source_ops = {
	.snapshot = atomic_source_snapshot,
};

static void atomic_unregister_source(void *context)
{
	mt6797_a72_direct_source_unregister(&atomic_source_ops, context);
}

static int
atomic_register_source(struct kunit *test,
		       struct atomic_publication_test_state *state)
{
	int ret;

	ret = mt6797_a72_direct_source_register(&atomic_source_ops, state);
	if (ret)
		return ret;
	return kunit_add_action_or_reset(test, atomic_unregister_source, state);
}

static int atomic_finalize_callback(void *context)
{
	struct atomic_publication_test_state *state = context;

	state->finalizer_calls++;
	return state->finalizer_result;
}

static void expect_owner_equal(struct kunit *test,
			       const struct mt6797_a72_owner_snapshot *left,
			       const struct mt6797_a72_owner_snapshot *right)
{
	KUNIT_EXPECT_EQ(test, memcmp(left, right, sizeof(*left)), 0);
}

static void expect_p30_equal(struct kunit *test,
			     const struct arm64_late_cpu_startup_snapshot *left,
			     const struct arm64_late_cpu_startup_snapshot *right)
{
	KUNIT_EXPECT_EQ(test, memcmp(left, right, sizeof(*left)), 0);
}

static void
expect_available_owner(struct kunit *test,
		       const struct mt6797_a72_owner_snapshot *owner)
{
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(&owner->active, 0,
					     sizeof(owner->active)), NULL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(owner->retired, 0,
					     sizeof(owner->retired)), NULL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(&owner->provider_identity, 0,
					     sizeof(owner->provider_identity)), NULL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(&owner->first_fault, 0,
					     sizeof(owner->first_fault)), NULL);
	KUNIT_EXPECT_EQ(test, owner->diagnostic_blockers,
			MT6797_A72_BLOCK_MASK &
			~MT6797_A72_BLOCK_A34_BOOTSTRAP);
	KUNIT_EXPECT_EQ(test, owner->controller_cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, owner->abi, (u32)MT6797_A72_TRANSACTION_ABI);
	KUNIT_EXPECT_EQ(test, owner->health,
			(u32)MT6797_A72_OWNER_AVAILABLE);
	KUNIT_EXPECT_EQ(test, owner->phase, (u32)MT6797_A72_PHASE_IDLE);
	KUNIT_EXPECT_EQ(test, owner->members, (u32)0);
	KUNIT_EXPECT_EQ(test, owner->provider_state,
			(u32)MT6797_A72_PROVIDER_NONE);
	KUNIT_EXPECT_EQ(test, owner->bootstrap_valid, (u32)1);
	KUNIT_EXPECT_EQ(test, owner->members_valid, (u32)1);
	KUNIT_EXPECT_EQ(test, owner->attempts_available,
			(u32)MT6797_A72_ATTEMPT_MASK);
	KUNIT_EXPECT_EQ(test, owner->attempts_consumed, (u32)0);
	KUNIT_EXPECT_EQ(test, owner->retired_mask, (u32)0);
	KUNIT_EXPECT_EQ(test, owner->controller_present, (u32)0);
}

static int atomic_publication_test_init(struct kunit *test)
{
	struct atomic_publication_test_state *state;

	mt6797_a72_membership_test_reset();
	arm64_late_cpu_startup_test_reset();
	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->source = atomic_source();
	state->topology = atomic_topology();
	state->replay = atomic_replay();
	test->priv = state;
	return 0;
}

static void atomic_finalizer_success_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct arm64_late_cpu_bootstrap_claim claim;
	struct arm64_late_cpu_bootstrap_claim second;

	arm64_late_cpu_startup_snapshot(&state->p30_before);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_finalize_pristine(&claim,
								 atomic_finalize_callback,
								 state), 0);
	KUNIT_EXPECT_EQ(test, state->finalizer_calls, (u32)1);
	KUNIT_EXPECT_EQ(test, claim.abi, (u32)0);
	KUNIT_EXPECT_EQ(test, claim.cookie, (u64)0);
	arm64_late_cpu_startup_snapshot(&state->p30_after);
	expect_p30_equal(test, &state->p30_before, &state->p30_after);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&second), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&second), 0);
}

static void atomic_finalizer_failure_identity_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct arm64_late_cpu_bootstrap_claim claim;
	struct arm64_late_cpu_bootstrap_claim wrong;

	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	wrong = claim;
	wrong.cookie++;
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_finalize_pristine(&wrong,
								 atomic_finalize_callback,
								 state), -ESTALE);
	KUNIT_EXPECT_EQ(test, state->finalizer_calls, (u32)0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);

	state->finalizer_result = -EPERM;
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_finalize_pristine(&claim,
								 atomic_finalize_callback,
								 state), -EPERM);
	KUNIT_EXPECT_EQ(test, state->finalizer_calls, (u32)1);
	KUNIT_EXPECT_EQ(test, claim.cookie, (u64)0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);
}

static void atomic_publication_success_repeat_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct arm64_late_cpu_bootstrap_claim claim;
	int ret;

	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	mt6797_a72_membership_snapshot(&state->owner_before);
	arm64_late_cpu_startup_snapshot(&state->p30_before);
	ret = mt6797_a72_membership_test_publish_bootstrap(&state->topology,
							   &state->replay, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)1);
	mt6797_a72_membership_snapshot(&state->owner_after);
	arm64_late_cpu_startup_snapshot(&state->p30_after);
	expect_available_owner(test, &state->owner_after);
	expect_p30_equal(test, &state->p30_before, &state->p30_after);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_preflight_up(8, CPUHP_ONLINE),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_validate_up(8, 0, CPUHP_ONLINE),
			-EOPNOTSUPP);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);

	state->owner_before = state->owner_after;
	ret = mt6797_a72_membership_test_publish_bootstrap(&state->topology,
							   &state->replay, false);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)1);
	mt6797_a72_membership_snapshot(&state->owner_after);
	expect_owner_equal(test, &state->owner_before, &state->owner_after);
}

static void atomic_publication_replay_rejections_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct mt6797_a72_a34_replay replay;

	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     NULL, false), -EINVAL);
	replay = state->replay;
	replay.abi++;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &replay, false), -EPROTO);
	replay = state->replay;
	replay.reserved[0] = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &replay, false), -EPROTO);
	replay = state->replay;
	replay.valid = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &replay, false), -ENODATA);
	replay = state->replay;
	replay.proof = MT6797_A72_A34_REPLAY_UNKNOWN;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &replay, false), -ENODATA);
	replay = state->replay;
	replay.private_replay_value = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &replay, false), -EPERM);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)0);
	mt6797_a72_membership_snapshot(&state->owner_after);
	KUNIT_EXPECT_EQ(test, state->owner_after.health,
			(u32)MT6797_A72_OWNER_CLOSED);
}

static void atomic_publication_source_rejections_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;

	mt6797_a72_membership_snapshot(&state->owner_before);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay,
								     false),
			-ENODEV);
	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	state->source_result = -EIO;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay, false), -EIO);
	state->source_result = 0;
	state->source.provider.control_a ^= 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay,
								     false),
			-EPERM);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)2);
	mt6797_a72_membership_snapshot(&state->owner_after);
	expect_owner_equal(test, &state->owner_before, &state->owner_after);
}

static void atomic_publication_topology_rejection_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;

	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	state->topology.cpu9_present = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay,
								     false),
			-EPERM);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)0);
	mt6797_a72_membership_snapshot(&state->owner_after);
	KUNIT_EXPECT_EQ(test, state->owner_after.health,
			(u32)MT6797_A72_OWNER_CLOSED);
}

static void atomic_publication_p30_busy_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct arm64_late_cpu_bootstrap_claim claim;

	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	mt6797_a72_membership_snapshot(&state->owner_before);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay,
								     false),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)1);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);
	mt6797_a72_membership_snapshot(&state->owner_after);
	expect_owner_equal(test, &state->owner_before, &state->owner_after);
}

static void atomic_publication_final_owner_mismatch_test(struct kunit *test)
{
	struct atomic_publication_test_state *state = test->priv;
	struct arm64_late_cpu_bootstrap_claim claim;

	KUNIT_ASSERT_EQ(test, atomic_register_source(test, state), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_a72_membership_test_publish_bootstrap(&state->topology,
								     &state->replay, true), -EPERM);
	KUNIT_EXPECT_EQ(test, state->source_calls, (u32)1);
	mt6797_a72_membership_snapshot(&state->owner_after);
	KUNIT_EXPECT_EQ(test, state->owner_after.health,
			(u32)MT6797_A72_OWNER_CLOSED);
	KUNIT_EXPECT_EQ(test, state->owner_after.phase,
			(u32)MT6797_A72_PHASE_UNINITIALIZED);
	KUNIT_ASSERT_EQ(test,
			arm64_late_cpu_startup_claim_pristine(&claim), 0);
	KUNIT_EXPECT_EQ(test,
			arm64_late_cpu_startup_release_pristine(&claim), 0);
}

static struct kunit_case atomic_publication_test_cases[] = {
	KUNIT_CASE(atomic_finalizer_success_test),
	KUNIT_CASE(atomic_finalizer_failure_identity_test),
	KUNIT_CASE(atomic_publication_success_repeat_test),
	KUNIT_CASE(atomic_publication_replay_rejections_test),
	KUNIT_CASE(atomic_publication_source_rejections_test),
	KUNIT_CASE(atomic_publication_topology_rejection_test),
	KUNIT_CASE(atomic_publication_p30_busy_test),
	KUNIT_CASE(atomic_publication_final_owner_mismatch_test),
	{}
};

static struct kunit_suite atomic_publication_test_suite = {
	.name = "mt6797-a72-atomic-publication",
	.init = atomic_publication_test_init,
	.test_cases = atomic_publication_test_cases,
};

kunit_test_suite(atomic_publication_test_suite);
#else
kunit_test_suite(mt6797_a72_owner_suite);
#endif

MODULE_LICENSE("GPL");
