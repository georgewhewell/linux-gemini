/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_MT6797_A72_MEMBERSHIP_H
#define __ASM_MT6797_A72_MEMBERSHIP_H

#include <linux/bits.h>
#include <linux/cpuhotplug.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/mt6797-a72-provider.h>
#include <linux/mt6797-a72-direct-state.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/late_cpu_startup.h>

/*
 * Version of a dormant C owner-model layout, not a P17/P18/P24 token ABI or
 * wire/hardware ABI. This slice consumes a P31 attempt and can mint a frozen
 * token only in an explicitly opened test state; A36 only validates an
 * immutable caller-supplied prestate record. A dormant P17/P18 ledger may
 * advance only the C phase to ON_ISSUED. The explicitly isolated provider
 * callback seam can return a read-only refusal, but it has no bootstrap,
 * CPUHP effect, P30 handoff, P14/P15, hardware mutation, or CPU_ON operation.
 */
#define MT6797_A72_TRANSACTION_ABI 1
#define MT6797_A72_OPERATION_SLOTS 4

enum mt6797_a72_owner_health {
	MT6797_A72_OWNER_CLOSED,
	MT6797_A72_OWNER_AVAILABLE,
	MT6797_A72_OWNER_FAULTED,
};

enum mt6797_a72_membership_phase {
	MT6797_A72_PHASE_UNINITIALIZED,
	MT6797_A72_PHASE_IDLE,
	MT6797_A72_PHASE_FROZEN,
	MT6797_A72_PHASE_ON_ISSUED,
	MT6797_A72_PHASE_OFF_COMMITTED,
	MT6797_A72_PHASE_QUERY_INFLIGHT,
	MT6797_A72_PHASE_OFF_PROVEN,
	MT6797_A72_PHASE_VERIFYING,
	MT6797_A72_PHASE_REJECTED,
	MT6797_A72_PHASE_FAULT,
	MT6797_A72_PHASE_CONSUMED,
};

enum mt6797_a72_public_admission {
	MT6797_A72_PUBLIC_ADMISSION_NONE,
	MT6797_A72_PUBLIC_ADMISSION_PREFLIGHT,
	MT6797_A72_PUBLIC_ADMISSION_CLAIMED,
};

enum mt6797_a72_membership_operation {
	MT6797_A72_OPERATION_NONE,
	MT6797_A72_OPERATION_CPU8_UP,
	MT6797_A72_OPERATION_CPU9_UP,
	MT6797_A72_OPERATION_CPU9_OFF,
	MT6797_A72_OPERATION_CPU8_LAST_OFF,
};

enum mt6797_a72_cpu8_derive_stage {
	MT6797_A72_DERIVE_NONE,
	MT6797_A72_DERIVE_TOPOLOGY,
	MT6797_A72_DERIVE_READY_TOKEN,
	MT6797_A72_DERIVE_BOOTSTRAP_OWNER,
	MT6797_A72_DERIVE_BOOTSTRAP_REPLAY,
	MT6797_A72_DERIVE_DIRECT_STATE,
	MT6797_A72_DERIVE_A34,
	MT6797_A72_DERIVE_P30_CLAIM,
	MT6797_A72_DERIVE_BOOTSTRAP_FINALIZE,
	MT6797_A72_DERIVE_P30_RELEASE,
	MT6797_A72_DERIVE_P31_CONSUME,
	MT6797_A72_DERIVE_ENTRY_VALIDATE,
	MT6797_A72_DERIVE_TOKEN_MINT,
	MT6797_A72_DERIVE_PRESTATE_VALIDATE,
	MT6797_A72_DERIVE_PRESTATE_BIND,
	MT6797_A72_DERIVE_COMPLETE,
};

enum mt6797_a72_provider_state {
	MT6797_A72_PROVIDER_NONE,
	MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT,
	MT6797_A72_PROVIDER_HELD,
	MT6797_A72_PROVIDER_RELEASE_INFLIGHT,
	MT6797_A72_PROVIDER_FAULT_UNKNOWN,
};

enum mt6797_a72_budget_state {
	MT6797_A72_BUDGET_NONE,
	MT6797_A72_BUDGET_AVAILABLE,
	MT6797_A72_BUDGET_CONSUMED,
};

enum mt6797_a72_mint_source {
	MT6797_A72_MINT_NONE,
	MT6797_A72_MINT_A34_BOOTSTRAP_OWNER,
};

enum mt6797_a72_owner_fault {
	MT6797_A72_FAULT_NONE,
	MT6797_A72_FAULT_ILLEGAL_EDGE,
	MT6797_A72_FAULT_IDENTITY,
	MT6797_A72_FAULT_DOUBLE_RELEASE,
	MT6797_A72_FAULT_P30_QUARANTINE,
	MT6797_A72_FAULT_PROVIDER_RELEASE_RETURN,
};

#define MT6797_A72_ATTEMPT_CPU8_UP BIT(0)
#define MT6797_A72_ATTEMPT_CPU9_UP BIT(1)
#define MT6797_A72_ATTEMPT_CPU9_OFF BIT(2)
#define MT6797_A72_ATTEMPT_CPU8_LAST_OFF BIT(3)
#define MT6797_A72_ATTEMPT_MASK GENMASK(3, 0)

/* Exact generic A28 entry snapshot bits; no bit is a capability. */
#define MT6797_A72_ENTRY_CPU8_PRESENT BIT(0)
#define MT6797_A72_ENTRY_CPU9_PRESENT BIT(1)
#define MT6797_A72_ENTRY_CPU8_POSSIBLE BIT(2)
#define MT6797_A72_ENTRY_CPU9_POSSIBLE BIT(3)
#define MT6797_A72_ENTRY_FLAGS_MASK GENMASK(3, 0)
#define MT6797_A72_OBSERVER_WINDOW_CLOSED 0
#define MT6797_A72_OBSERVER_WINDOW_OPEN 1

/* A36 is an immutable prestate record, not a hardware access interface. */
#define MT6797_A72_A36_PRESTATE_ABI 2
#define MT6797_A72_A36_CALL_SHAPE_NONE 0
#define MT6797_A72_A36_CALL_SHAPE_TWO_ARG 1
#define MT6797_A72_A36_DA921X_PAGE 0x80
#define MT6797_A72_A36_BUCKB_VSEL 0x46
#define MT6797_A72_A36_SPM_218 0x00010132
#define MT6797_A72_A36_SPM_290 0x00000002

#define MT6797_A72_A34_REPLAY_ABI 1
enum mt6797_a72_a34_replay_proof {
	MT6797_A72_A34_REPLAY_UNKNOWN,
	MT6797_A72_A34_REPLAY_APPLICABLE_PRIMARY_BL31_CLEAR,
};
struct mt6797_a72_a34_replay {
	u32 abi;
	u32 valid;
	u32 proof;
	u32 private_replay_value;
	u32 reserved[4];
};

/* P27 is an attested source-only preparation ledger, not an MMIO API. */
#define MT6797_A72_P27_PREPARATION_ABI 1
#define MT6797_A72_P27_MP2_RESET_RELEASED BIT(0)
#define MT6797_A72_P27_BPLL_ORDER_READ BIT(1)
#define MT6797_A72_P27_PWRAP_ASSERTED BIT(2)
#define MT6797_A72_P27_EFFECT_MASK (MT6797_A72_P27_MP2_RESET_RELEASED | \
					MT6797_A72_P27_BPLL_ORDER_READ | \
					MT6797_A72_P27_PWRAP_ASSERTED)

enum mt6797_a72_p27_stage {
	MT6797_A72_P27_STAGE_NONE,
	MT6797_A72_P27_STAGE_INFLIGHT,
	MT6797_A72_P27_STAGE_COMPLETE,
};

/* R01/R02/R03 are an attested source-only provider boundary, not a call shim. */
#define MT6797_A72_PROVIDER_ACQUIRE_ABI 1
#define MT6797_A72_PROVIDER_ACQUIRE_SETTLE_US 1000
#define MT6797_A72_PROVIDER_ORIGIN_M01 1
#define MT6797_A72_PROVIDER_REJECT_ABI 1
#define MT6797_A72_PROVIDER_REJECTED_BEFORE_VOTE 1
#define MT6797_A72_PROVIDER_ABORT_ABI 1
#define MT6797_A72_PROVIDER_ABORT_EXACT_RELEASE 1

/* P28 is an attested postprovider preparation, not a hardware API. */
#define MT6797_A72_P28_PREPARATION_ABI 1
#define MT6797_A72_P28_ISOLATION_FROM 0x00000002
#define MT6797_A72_P28_ISOLATION_TO 0x00000000
#define MT6797_A72_P28_WAIT_US 240
#define MT6797_A72_P28_SRAM_LDO_MV 1100
#define MT6797_A72_P28_SELECTOR 0x8fb
#define MT6797_A72_P28_ISOLATION_CLEARED BIT(0)
#define MT6797_A72_P28_PWRAP_DEASSERTED BIT(1)
#define MT6797_A72_P28_GUARD_RELEASED BIT(2)
#define MT6797_A72_P28_SRAM_REQUESTED BIT(3)
#define MT6797_A72_P28_SELECTOR_VERIFIED BIT(4)
#define MT6797_A72_P28_EFFECT_MASK \
	(MT6797_A72_P28_ISOLATION_CLEARED | \
	 MT6797_A72_P28_PWRAP_DEASSERTED | \
	 MT6797_A72_P28_GUARD_RELEASED | \
	 MT6797_A72_P28_SRAM_REQUESTED | \
	 MT6797_A72_P28_SELECTOR_VERIFIED)

enum mt6797_a72_p28_stage {
	MT6797_A72_P28_STAGE_NONE,
	MT6797_A72_P28_STAGE_INFLIGHT,
	MT6797_A72_P28_STAGE_COMPLETE,
};

/* P29 is an attested pre-isolation rollback, not an inverse-MMIO API. */
#define MT6797_A72_P29_ROLLBACK_ABI 1

/* P32 is a generation-bound rollback side channel, not a CPU_OFF API. */
#define MT6797_A72_P32_ROLLBACK_ABI 1
#define MT6797_A72_P32_KILL_SPINS 100000U
#define MT6797_A72_P32_GUARD_DISABLE BIT(0)
#define MT6797_A72_P32_GUARD_DIE BIT(1)
#define MT6797_A72_P32_GUARD_KILL BIT(2)
#define MT6797_A72_P32_GUARD_PARK BIT(3)

enum mt6797_a72_p32_state {
	MT6797_A72_P32_STATE_NONE,
	MT6797_A72_P32_STATE_PUBLISHED,
	MT6797_A72_P32_STATE_GUARDED,
	MT6797_A72_P32_STATE_PARKED,
	MT6797_A72_P32_STATE_CONSUMED,
};

enum mt6797_a72_p32_branch {
	MT6797_A72_P32_BRANCH_NONE,
	MT6797_A72_P32_BRANCH_A,
	MT6797_A72_P32_BRANCH_D,
	MT6797_A72_P32_BRANCH_F,
	MT6797_A72_P32_BRANCH_X,
	MT6797_A72_P32_BRANCH_R,
};

struct mt6797_a72_p32_record {
	u32 abi;
	u32 state;
	u32 branch;
	u32 guard_flags;
	u32 cpu_up_error;
	u32 callback_state;
	u32 target_cpu;
	u32 reserved;
	u64 target_mpidr;
	u64 generation;
	u64 cookie;
	struct cpu_up_rollback_trace callback_trace;
	u32 callback_trace_valid;
};
#define MT6797_A72_P32R_HANDOFF_ABI 1
#define MT6797_A72_P32R_EFFECT_HPS_SUCCESS BIT(0)
#define MT6797_A72_P32R_EFFECT_PROVIDER_RELEASE BIT(1)
#define MT6797_A72_P32R_EFFECT_RETRY BIT(2)
#define MT6797_A72_P32R_EFFECT_MEMBERSHIP_COMMIT BIT(3)
#define MT6797_A72_P32R_EFFECT_MASK \
	(MT6797_A72_P32R_EFFECT_HPS_SUCCESS | \
	 MT6797_A72_P32R_EFFECT_PROVIDER_RELEASE | \
	 MT6797_A72_P32R_EFFECT_RETRY | \
	 MT6797_A72_P32R_EFFECT_MEMBERSHIP_COMMIT)

enum mt6797_a72_p32r_disposition {
	MT6797_A72_P32R_NONE,
	MT6797_A72_P32R_FAULT_ROLLBACK_RECORDED,
	MT6797_A72_P32R_FAULT_ROLLBACK_LOST,
};


/* Diagnostic only: this non-exhaustive list can never authorize a request. */
#define MT6797_A72_BLOCK_A34_BOOTSTRAP BIT_ULL(0)
#define MT6797_A72_BLOCK_A41_READY BIT_ULL(1)
#define MT6797_A72_BLOCK_A25_CALLBACKS BIT_ULL(2)
#define MT6797_A72_BLOCK_PROVIDER_OWNER BIT_ULL(3)
#define MT6797_A72_BLOCK_A36_PRESTATE BIT_ULL(4)
#define MT6797_A72_BLOCK_P30_INTEGRATION BIT_ULL(5)
#define MT6797_A72_BLOCK_P30E_MMU_OFF BIT_ULL(6)
#define MT6797_A72_BLOCK_P14_P15 BIT_ULL(7)
#define MT6797_A72_BLOCK_P32_ROLLBACK BIT_ULL(8)
#define MT6797_A72_BLOCK_A33_COMMIT BIT_ULL(9)
#define MT6797_A72_BLOCK_FAILSTOP_RESET BIT_ULL(10)
#define MT6797_A72_BLOCK_A26_VETO BIT_ULL(11)
#define MT6797_A72_BLOCK_OWNER_IMPLEMENTATION BIT_ULL(12)
#define MT6797_A72_BLOCK_MASK GENMASK_ULL(12, 0)

struct mt6797_a72_provider_identity {
	u64 generation;
	u64 cookie;
};

struct mt6797_a72_transaction_identity {
	u32 abi;
	u16 owner;
	u16 operation;
	u32 target_cpu;
	u32 cpuhp_target;
	u64 target_mpidr;
	u64 cpu_on_entry_pa;
	u64 generation;
	u64 cookie;
};

struct mt6797_a72_call_budgets {
	u8 preparation;
	u8 provider_acquire;
	u8 postprovider_preparation;
	u8 cpu_on;
	u8 affinity;
	u8 provider_release;
	u8 provider_abort;
	u8 reserved[2];
};

struct mt6797_a72_entry_snapshot {
	u32 members;
	u32 provider_state;
	u32 online_mask;
	u32 cpuhp_state_cpu8;
	u32 cpuhp_state_cpu9;
	u32 observer_window;
	u32 flags;
	u64 cpu8_mpidr;
	u64 cpu9_mpidr;
};
struct mt6797_a72_p32r_ledger_handoff {
	u32 abi;
	u32 valid;
	u32 disposition;
	u32 callback_complete;
	u32 callback_overflow;
	u32 callback_unknown;
	u32 callback_capacity;
	u32 callback_inventory;
	u32 effect_complete;
	u32 effect_overflow;
	u32 effect_unknown;
	u32 effect_required_mask;
	u32 effect_seen_mask;
	u32 effect_missing_mask;
	u32 effect_forbidden_mask;
	u32 provider_state;
	u32 owner_phase;
	u32 members;
	u32 reserved;
	struct mt6797_a72_transaction_identity identity;
	struct mt6797_a72_entry_snapshot membership;
	struct mt6797_a72_provider_identity provider_identity;
	u64 diagnostic_blockers;
};

struct mt6797_a72_a36_prestate {
	u32 abi;
	u32 operation;
	u32 observer_window;
	u32 call_shape;
	u32 cpu8_online;
	u32 cpu9_online;
	u32 da921x_page;
	u32 buckb_enabled;
	u32 buckb_vsel;
	u32 spm_218;
	u32 spm_290;
	u32 toprgu_pwrap_reset;
	u32 mp2_dcm;
	u32 secure_sentinels_stable;
	u32 protected_clock_valid;
	u32 pstore_console_available;
	u32 watchdog_owned;
	u32 cpu8_cluster_dcm_published;
	u32 shared_write_set;
	u32 reserved;
	u64 target_mpidr;
	u64 secondary_entry_pa;
	u64 generation;
	u64 cookie;
};

struct mt6797_a72_p27_preparation {
	u32 abi;
	u32 operation;
	u32 stage;
	u32 effect_mask;
	u64 generation;
	u64 cookie;
};

struct mt6797_a72_provider_acquire_proof {
	u32 abi;
	u32 operation;
	u32 settle_us;
	u32 da921x_page;
	u32 buckb_enabled;
	u32 buckb_vsel;
	u32 origin;
	u32 reserved;
	struct mt6797_a72_provider_identity held_identity;
	u64 transaction_generation;
	u64 transaction_cookie;
	u64 origin_generation;
};

struct mt6797_a72_provider_rejection {
	u32 abi;
	u32 operation;
	u32 result;
	u32 returned;
	u32 vote_requested;
	u32 provider_mutated;
	u32 rail_mutated;
	u32 reserved;
	u64 transaction_generation;
	u64 transaction_cookie;
};

struct mt6797_a72_provider_abort_proof {
	u32 abi;
	u32 operation;
	u32 result;
	u32 returned;
	u32 vote_requested;
	u32 provider_mutated;
	u32 rail_mutated;
	u32 settle_us;
	u32 da921x_page;
	u32 buckb_enabled;
	u32 buckb_vsel;
	u32 origin;
	u32 reserved;
	struct mt6797_a72_provider_identity released_identity;
	u64 transaction_generation;
	u64 transaction_cookie;
	u64 origin_generation;
};

struct mt6797_a72_p28_preparation {
	u32 abi;
	u32 operation;
	u32 stage;
	u32 effect_mask;
	u32 isolation_from;
	u32 isolation_to;
	u32 pwrap_deasserted;
	u32 software_guard_released;
	u32 wait_before_sram_us;
	u32 sram_ldo_mv;
	u32 wait_after_sram_us;
	u32 selector;
	u32 calibration_stable;
	u32 calibration_valid;
	u32 reserved;
	struct mt6797_a72_provider_identity provider_identity;
	u64 transaction_generation;
	u64 transaction_cookie;
};

struct mt6797_a72_p29_rollback_proof {
	u32 abi;
	u32 operation;
	u32 restored_effect_mask;
	u32 residual_effect_mask;
	u32 p28_started;
	u32 cpu_on_issued;
	u32 reserved;
	u64 transaction_generation;
	u64 transaction_cookie;
};

struct mt6797_a72_transaction {
	struct mt6797_a72_transaction_identity identity;
	struct mt6797_a72_call_budgets budgets;
	struct mt6797_a72_entry_snapshot entry;
	struct mt6797_a72_a36_prestate a36_prestate;
	struct mt6797_a72_p27_preparation p27_preparation;
	struct mt6797_a72_provider_acquire_proof provider_acquire_proof;
	struct mt6797_a72_provider_identity provider_identity;
	struct mt6797_a72_provider_rejection provider_rejection;
	struct mt6797_a72_provider_abort_proof provider_abort_proof;
	struct mt6797_a72_p29_rollback_proof p29_rollback;
	struct mt6797_a72_p28_preparation p28_preparation;
	struct arm64_late_cpu_up_token p30_token;
	struct mt6797_a72_p32_record p32;
	struct mt6797_a72_p32r_ledger_handoff p32r;
	u64 controller_cookie;
	u64 effect_prefix;
	u32 mint_source;
	u32 p30_token_valid;
	u32 public_preflight;
	u32 a36_valid;
	u32 p17_p18_published;
	u32 p27_valid;
	u32 provider_acquire_valid;
	u32 provider_rejection_valid;
	u32 provider_abort_valid;
	u32 cpu8_success_published;
	u32 p28_valid;
	u32 p29_valid;
	u32 p32_valid;
	u32 p32r_valid;
	u32 p32r_side_effects;
	u32 freeze_release_count;
	u32 valid;
};

struct mt6797_a72_fault_record {
	struct mt6797_a72_transaction_identity identity;
	u64 detail;
	u32 cause;
	u32 valid;
};

struct mt6797_a72_owner_snapshot {
	struct mt6797_a72_transaction active;
	struct mt6797_a72_transaction retired[MT6797_A72_OPERATION_SLOTS];
	struct mt6797_a72_provider_identity provider_identity;
	struct mt6797_a72_fault_record first_fault;
	u64 diagnostic_blockers;
	u64 controller_cookie;
	u32 abi;
	u32 health;
	u32 phase;
	u32 members;
	u32 provider_state;
	u32 bootstrap_valid;
	u32 members_valid;
	u32 attempts_available;
	u32 attempts_consumed;
	u32 retired_mask;
	u32 controller_present;
};

#define MT6797_A72_DIRECT_STATE_ABI 1
struct mt6797_a72_direct_topology {
	u32 cpu8_possible, cpu9_possible;
	u32 cpu8_present, cpu9_present;
	u32 cpu8_online, cpu9_online;
	u32 cpu8_method_valid, cpu9_method_valid;
	u64 cpu8_mpidr, cpu9_mpidr;
};
struct mt6797_a72_direct_state_snapshot {
	u32 abi, valid;
	u32 cpu8_possible, cpu9_possible;
	u32 cpu8_present, cpu9_present;
	u32 cpu8_online, cpu9_online;
	u32 cpu8_method_valid, cpu9_method_valid;
	u64 cpu8_mpidr, cpu9_mpidr;
	struct mt6797_a72_direct_source_snapshot source;
	struct mt6797_a72_owner_snapshot owner;
};
#ifdef CONFIG_ARM64_MT6797_A72_DIRECT_STATE_COMPOSITOR
int mt6797_a72_direct_source_register(const struct mt6797_a72_direct_source_ops *ops, void *context);
void mt6797_a72_direct_source_unregister(const struct mt6797_a72_direct_source_ops *ops, void *context);
int mt6797_a72_direct_state_snapshot(struct mt6797_a72_direct_state_snapshot *snapshot);
#else
static inline int mt6797_a72_direct_source_register(const struct mt6797_a72_direct_source_ops *ops, void *context)
{ (void)ops; (void)context; return -EOPNOTSUPP; }
static inline void mt6797_a72_direct_source_unregister(const struct mt6797_a72_direct_source_ops *ops, void *context)
{ (void)ops; (void)context; }
static inline int mt6797_a72_direct_state_snapshot(struct mt6797_a72_direct_state_snapshot *snapshot)
{ if (snapshot) memset(snapshot, 0, sizeof(*snapshot)); return -EOPNOTSUPP; }
#endif

#define MT6797_A72_A34_ELIGIBILITY_ABI 1
struct mt6797_a72_a34_observation {
	u32 abi;
	u32 reserved[3];
	struct mt6797_a72_direct_state_snapshot direct;
	struct mt6797_a72_a34_replay replay;
};

#ifdef CONFIG_ARM64_MT6797_A72_P24_TRANSACTION_OWNER_MODEL
int
mt6797_a72_membership_begin_up(unsigned int cpu, enum cpuhp_state target,
			       u32 attempt,
			       const struct mt6797_a72_entry_snapshot *entry,
			       const struct arm64_late_cpu_ready_token *ready,
			       const struct mt6797_a72_a36_prestate *prestate,
			       struct mt6797_a72_transaction *transaction);
#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_DERIVED_ADMISSION)
int
mt6797_a72_membership_derive_cpu8(const struct arm64_late_cpu_ready_token *ready,
				  struct mt6797_a72_transaction *transaction);
int
mt6797_a72_membership_derive_cpu8_diagnostic(const struct arm64_late_cpu_ready_token *ready, struct mt6797_a72_transaction *transaction, u32 *derive_stage);
int
mt6797_a72_membership_derive_cpu9_diagnostic(const struct arm64_late_cpu_ready_token *ready, struct mt6797_a72_transaction *transaction, u32 *derive_stage);
#else
static inline int
mt6797_a72_membership_derive_cpu8_diagnostic(const struct arm64_late_cpu_ready_token *ready, struct mt6797_a72_transaction *transaction, u32 *derive_stage)
{
	(void)ready;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	if (derive_stage)
		*derive_stage = MT6797_A72_DERIVE_NONE;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_derive_cpu8(const struct arm64_late_cpu_ready_token *ready,
				  struct mt6797_a72_transaction *transaction)
{
	return mt6797_a72_membership_derive_cpu8_diagnostic(ready, transaction,
							    NULL);
}
#endif
#ifdef CONFIG_ARM64_MT6797_A72_DERIVED_ADMISSION_KUNIT_TEST
int
mt6797_a72_membership_test_derive_cpu8(const struct mt6797_a72_direct_topology *topology,
				       const struct arm64_late_cpu_ready_token *ready,
				       struct mt6797_a72_transaction *transaction);
#endif
int
mt6797_a72_membership_validate_up_prestate(const struct mt6797_a72_transaction *transaction,
							   const struct mt6797_a72_a36_prestate *prestate);
int mt6797_a72_membership_publish_up(struct mt6797_a72_transaction *transaction);
#ifdef CONFIG_ARM64_MT6797_A72_A34_ELIGIBILITY_EVALUATOR
int mt6797_a72_a34_evaluate(const struct mt6797_a72_a34_observation *observation);
#else
static inline int mt6797_a72_a34_evaluate(const struct mt6797_a72_a34_observation *observation)
{
	(void)observation;
	return -EOPNOTSUPP;
}
#endif
#ifdef CONFIG_ARM64_MT6797_A72_BOOTSTRAP_PUBLISHER
int mt6797_a72_membership_publish_bootstrap(const struct mt6797_a72_a34_replay *replay);
#else
static inline int mt6797_a72_membership_publish_bootstrap(const struct mt6797_a72_a34_replay *replay)
{
	(void)replay;
	return -EOPNOTSUPP;
}
#endif
#ifdef CONFIG_ARM64_MT6797_A72_P30E_WIRE
struct mt6797_a72_p30e_handoff {
	u32 target_cpu;
	u32 reserved;
	u64 slot_pa;
	u64 entry_pa;
	u64 wire_boot_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 target_boot_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 operation;
	u64 generation;
	u64 cookie;
};

int mt6797_a72_membership_prepare_p30e_handoff(
	const struct mt6797_a72_transaction *transaction,
	const struct arm64_late_cpu_ready_token *ready,
	struct mt6797_a72_p30e_handoff *handoff);
#else
struct mt6797_a72_p30e_handoff;

static inline int mt6797_a72_membership_prepare_p30e_handoff(
	const struct mt6797_a72_transaction *transaction,
	const struct arm64_late_cpu_ready_token *ready,
	struct mt6797_a72_p30e_handoff *handoff)
{
	(void)transaction;
	(void)ready;
	(void)handoff;
	return -EOPNOTSUPP;
}
#endif
int mt6797_a72_membership_begin_p27_preparation(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_complete_p27_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p27_preparation *preparation);
int mt6797_a72_membership_begin_provider_acquire(
	struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_confirm_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_acquire_proof *proof);
int mt6797_a72_membership_reject_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_rejection *rejection);
int mt6797_a72_membership_run_provider_acquire(struct mt6797_a72_transaction *transaction,
	struct mt6797_a72_provider_response *response);
int
mt6797_a72_membership_begin_provider_abort(struct mt6797_a72_transaction *transaction);
int
mt6797_a72_membership_confirm_abort(struct mt6797_a72_transaction *transaction,
				    const struct mt6797_a72_provider_abort_proof *proof);
int
mt6797_a72_membership_run_provider_abort(struct mt6797_a72_transaction *transaction,
					 struct mt6797_a72_provider_response *response);
int mt6797_a72_membership_begin_p28_preparation(
	struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_complete_p28_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p28_preparation *preparation);
int mt6797_a72_membership_complete_p29_rollback(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p29_rollback_proof *rollback);
int mt6797_a72_membership_validate_entry(unsigned int cpu,
					 enum cpuhp_state target, u32 attempt,
					 const struct mt6797_a72_entry_snapshot *entry);
int mt6797_a72_membership_preflight_up(unsigned int cpu,
				       enum cpuhp_state target);
int mt6797_a72_membership_validate_up(unsigned int cpu, int tasks_frozen,
				      enum cpuhp_state target);
int mt6797_a72_membership_claim_cpu8(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_reject_cpu8(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_begin_cpu8_on(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_publish_cpu8_success(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_finalize_cpu8_success(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_claim_cpu9(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_reject_cpu9(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_begin_cpu9_on(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_publish_cpu9_success(struct mt6797_a72_transaction *transaction);
int mt6797_a72_membership_finalize_cpu9_success(struct mt6797_a72_transaction *transaction);
bool
mt6797_a72_membership_owns_up_token(const struct arm64_late_cpu_up_token *token);
int mt6797_a72_membership_copy_up_token(struct arm64_late_cpu_up_token *token);
int mt6797_a72_membership_publish_p32(unsigned int cpu,
				      enum cpuhp_state callback_state,
				      int error,
				      const struct cpu_up_rollback_trace *trace);
int mt6797_a72_membership_p32_cpu_disable(unsigned int cpu);
bool mt6797_a72_membership_p32_cpu_die(unsigned int cpu);
int mt6797_a72_membership_p32_cpu_kill(unsigned int cpu);
int mt6797_a72_membership_consume_p32(unsigned int cpu, int rollback_error,
					     const struct cpu_up_rollback_trace *trace);
void mt6797_a72_membership_snapshot(struct mt6797_a72_owner_snapshot *snapshot);
#else
static inline int
mt6797_a72_membership_begin_up(unsigned int cpu, enum cpuhp_state target,
			       u32 attempt,
			       const struct mt6797_a72_entry_snapshot *entry,
			       const struct arm64_late_cpu_ready_token *ready,
			       const struct mt6797_a72_a36_prestate *prestate,
			       struct mt6797_a72_transaction *transaction)
{
	(void)attempt;
	(void)entry;
	(void)ready;
	(void)prestate;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_validate_up_prestate(const struct mt6797_a72_transaction *transaction,
						   const struct mt6797_a72_a36_prestate *prestate)
{
	(void)transaction;
	(void)prestate;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_publish_up(struct mt6797_a72_transaction *transaction)
{
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_begin_p27_preparation(struct mt6797_a72_transaction *transaction)
{
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_complete_p27_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p27_preparation *preparation)
{
	(void)preparation;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_begin_provider_acquire(
	struct mt6797_a72_transaction *transaction)
{
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_confirm_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_acquire_proof *proof)
{
	(void)proof;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_reject_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_rejection *rejection)
{
	(void)rejection;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_run_provider_acquire(struct mt6797_a72_transaction *transaction,
	struct mt6797_a72_provider_response *response)
{
	(void)response;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_begin_provider_abort(struct mt6797_a72_transaction *transaction)
{
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_confirm_abort(struct mt6797_a72_transaction *transaction,
				    const struct mt6797_a72_provider_abort_proof *proof)
{
	(void)proof;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_run_provider_abort(struct mt6797_a72_transaction *transaction,
					 struct mt6797_a72_provider_response *response)
{
	(void)response;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_begin_p28_preparation(
	struct mt6797_a72_transaction *transaction)
{
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_complete_p28_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p28_preparation *preparation)
{
	(void)preparation;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_complete_p29_rollback(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p29_rollback_proof *rollback)
{
	(void)rollback;
	if (transaction)
		memset(transaction, 0, sizeof(*transaction));
	return -EOPNOTSUPP;
}

static inline int mt6797_a72_membership_validate_entry(unsigned int cpu,
						 enum cpuhp_state target, u32 attempt,
						 const struct mt6797_a72_entry_snapshot *entry)
{
	(void)cpu;
	(void)target;
	(void)attempt;
	(void)entry;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_preflight_up(unsigned int cpu, enum cpuhp_state target)
{
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_validate_up(unsigned int cpu, int tasks_frozen,
				  enum cpuhp_state target)
{
	return -EOPNOTSUPP;
}

static inline bool
mt6797_a72_membership_owns_up_token(const struct arm64_late_cpu_up_token *token)
{
	return false;
}

static inline int
mt6797_a72_membership_copy_up_token(struct arm64_late_cpu_up_token *token)
{
	if (token)
		memset(token, 0, sizeof(*token));
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_membership_publish_p32(unsigned int cpu,
				  enum cpuhp_state callback_state, int error,
				  const struct cpu_up_rollback_trace *trace)
{
	(void)cpu;
	(void)callback_state;
	(void)error;
	(void)trace;
	return -EOPNOTSUPP;
}

static inline int mt6797_a72_membership_p32_cpu_disable(unsigned int cpu)
{
	(void)cpu;
	return 0;
}

static inline bool mt6797_a72_membership_p32_cpu_die(unsigned int cpu)
{
	(void)cpu;
	return false;
}

static inline int mt6797_a72_membership_p32_cpu_kill(unsigned int cpu)
{
	(void)cpu;
	return 0;
}

static inline int mt6797_a72_membership_consume_p32(unsigned int cpu,
						    int rollback_error,
						    const struct cpu_up_rollback_trace *trace)
{
	(void)cpu;
	(void)rollback_error;
	(void)trace;
	return -EOPNOTSUPP;
}

static inline void
mt6797_a72_membership_snapshot(struct mt6797_a72_owner_snapshot *snapshot)
{
	if (snapshot)
		memset(snapshot, 0, sizeof(*snapshot));
}
#endif

#ifdef CONFIG_ARM64_MT6797_A72_P24_OWNER_KUNIT_TEST
void mt6797_a72_membership_test_reset(void);
void mt6797_a72_membership_test_seed_available(void);
void mt6797_a72_membership_test_seed_available_cpu9(void);
int mt6797_a72_membership_test_publish_cpu8_success(struct mt6797_a72_transaction *transaction,
						    bool cpu8_online, bool cpu9_online);
int mt6797_a72_membership_test_finalize_cpu8_success(struct mt6797_a72_transaction *transaction,
						     bool cpu8_online, bool cpu9_online);
#endif

#endif /* __ASM_MT6797_A72_MEMBERSHIP_H */
