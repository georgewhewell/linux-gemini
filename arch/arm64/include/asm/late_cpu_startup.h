/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_LATE_CPU_STARTUP_H
#define __ASM_LATE_CPU_STARTUP_H

#include <linux/bits.h>
#include <linux/types.h>

#include <asm/late_cpu_profile.h>

/* Version of this dormant C protocol model, not an assembly wire ABI. */
#define ARM64_LATE_CPU_STARTUP_ABI 1
#define ARM64_LATE_CPU_STARTUP_OPERATION_SLOTS 2

enum arm64_late_cpu_startup_owner {
	ARM64_LATE_CPU_STARTUP_OWNER_NONE,
	ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP,
};

enum arm64_late_cpu_startup_operation {
	ARM64_LATE_CPU_STARTUP_OP_NONE,
	ARM64_LATE_CPU_STARTUP_OP_CPU8_UP,
	ARM64_LATE_CPU_STARTUP_OP_CPU9_UP,
};

/* P30K/C/P/E/U are closure cases, not membership phases. */
enum arm64_late_cpu_startup_branch {
	ARM64_LATE_CPU_STARTUP_BRANCH_NONE,
	ARM64_LATE_CPU_STARTUP_BRANCH_K,
	ARM64_LATE_CPU_STARTUP_BRANCH_C,
	ARM64_LATE_CPU_STARTUP_BRANCH_P,
	ARM64_LATE_CPU_STARTUP_BRANCH_E,
	ARM64_LATE_CPU_STARTUP_BRANCH_U,
};

/* C-model states only; P30E still requires a separate MMU-off wire object. */
enum arm64_late_cpu_startup_state {
	ARM64_LATE_CPU_STARTUP_FREE,
	ARM64_LATE_CPU_STARTUP_PREPARED,
	ARM64_LATE_CPU_STARTUP_ABORTED,
	ARM64_LATE_CPU_STARTUP_ARMED,
	ARM64_LATE_CPU_STARTUP_PUBLISHING,
	ARM64_LATE_CPU_STARTUP_PUBLISHED,
	ARM64_LATE_CPU_STARTUP_CANCELLED,
	ARM64_LATE_CPU_STARTUP_FAILING,
	ARM64_LATE_CPU_STARTUP_FAULTED,
	ARM64_LATE_CPU_STARTUP_PARKED,
	ARM64_LATE_CPU_STARTUP_PANICKED,
};

enum arm64_late_cpu_startup_claim {
	ARM64_LATE_CPU_STARTUP_CLAIM_WON,
	ARM64_LATE_CPU_STARTUP_CLAIM_PUBLISHER,
	ARM64_LATE_CPU_STARTUP_CLAIM_CANCELLED,
	ARM64_LATE_CPU_STARTUP_CLAIM_FAILURE,
	ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED,
	ARM64_LATE_CPU_STARTUP_CLAIM_TERMINAL,
	ARM64_LATE_CPU_STARTUP_CLAIM_STALE,
	ARM64_LATE_CPU_STARTUP_CLAIM_QUARANTINED,
};

enum arm64_late_cpu_quarantine_cause {
	ARM64_LATE_CPU_QUARANTINE_NONE,
	ARM64_LATE_CPU_QUARANTINE_CANCELLED,
	ARM64_LATE_CPU_QUARANTINE_CPU_ON_UNPROVEN,
	ARM64_LATE_CPU_QUARANTINE_CPU_ON_FAULT,
	ARM64_LATE_CPU_QUARANTINE_TARGET_FAILURE,
	ARM64_LATE_CPU_QUARANTINE_PARKED,
	ARM64_LATE_CPU_QUARANTINE_PANICKED,
	ARM64_LATE_CPU_QUARANTINE_ONLINE_MISMATCH,
	ARM64_LATE_CPU_QUARANTINE_IDENTITY,
	ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE,
	ARM64_LATE_CPU_QUARANTINE_REUSE,
};

/* Conservative prefix of effects possible before publication or parking. */
#define ARM64_LATE_CPU_EFFECT_MM BIT_ULL(0)
#define ARM64_LATE_CPU_EFFECT_RCU_STARTING BIT_ULL(1)
#define ARM64_LATE_CPU_EFFECT_GIC BIT_ULL(2)
#define ARM64_LATE_CPU_EFFECT_CPUINFO BIT_ULL(3)
#define ARM64_LATE_CPU_EFFECT_TOPOLOGY BIT_ULL(4)
#define ARM64_LATE_CPU_EFFECT_STARTING BIT_ULL(5)
#define ARM64_LATE_CPU_EFFECT_IPI BIT_ULL(6)
#define ARM64_LATE_CPU_EFFECT_NUMA BIT_ULL(7)
#define ARM64_LATE_CPU_EFFECT_PRESENT_CLEAR BIT_ULL(8)
#define ARM64_LATE_CPU_EFFECT_RCU_DEAD BIT_ULL(9)
#define ARM64_LATE_CPU_EFFECT_DIE_GUARD BIT_ULL(10)
#define ARM64_LATE_CPU_EFFECT_MASK GENMASK_ULL(10, 0)

/* Immutable identity copied from the future P17/P18/P24 transaction owner. */
struct arm64_late_cpu_up_token {
	u32 abi;
	u16 owner;
	u16 operation;
	u32 target_cpu;
	u32 reserved;
	u64 target_mpidr;
	u64 generation;
	u64 cookie;
	u64 plan_identity[ARM64_LATE_CPU_ID_WORDS];
};

/* Exact target-side subset of the immutable controller token. */
struct arm64_late_cpu_startup_target {
	u32 cpu;
	u32 reserved;
	u64 mpidr;
	u64 generation;
	u64 cookie;
};

struct arm64_late_cpu_startup_terminal {
	u32 terminal_from;
	u32 claimed_branch;
	u32 branch;
	u32 reserved;
	u64 reason;
	u64 effects;
};

struct arm64_late_cpu_startup_snapshot {
	struct arm64_late_cpu_up_token token;
	struct arm64_late_cpu_up_token
		retired_token[ARM64_LATE_CPU_STARTUP_OPERATION_SLOTS];
	struct arm64_late_cpu_up_token quarantine_token;
	struct arm64_late_cpu_startup_terminal terminal;
	u64 success_effects;
	u32 state;
	u32 completion_consumed;
	u32 online_validated;
	u32 park_committed;
	u32 stuck_interlock;
	u32 quarantined;
	u32 quarantine_cause;
	u32 retired_mask;
};

#ifdef CONFIG_ARM64_LATE_CPU_STARTUP_FAILSTOP
int arm64_late_cpu_startup_prepare(const struct arm64_late_cpu_up_token *token);
int arm64_late_cpu_startup_abort_unissued(
	const struct arm64_late_cpu_up_token *token);
int arm64_late_cpu_startup_release_aborted(
	const struct arm64_late_cpu_up_token *token);
int arm64_late_cpu_startup_arm_before_cpu_on(
	const struct arm64_late_cpu_up_token *token);
int arm64_late_cpu_startup_fault_cpu_on_unproven(
	const struct arm64_late_cpu_up_token *token,
	enum arm64_late_cpu_startup_state *winner);
int arm64_late_cpu_startup_cpu_on_fault(
	const struct arm64_late_cpu_up_token *token,
	enum arm64_late_cpu_startup_state *winner);
enum arm64_late_cpu_startup_claim
arm64_late_cpu_startup_cancel(const struct arm64_late_cpu_up_token *token);
enum arm64_late_cpu_startup_claim arm64_late_cpu_startup_claim_publish(
	const struct arm64_late_cpu_startup_target *target);
int arm64_late_cpu_startup_publish(
	const struct arm64_late_cpu_startup_target *target, u64 effects);
int arm64_late_cpu_startup_consume_published(
	const struct arm64_late_cpu_up_token *token);
int arm64_late_cpu_startup_retire_published_after_p14_p15(
	const struct arm64_late_cpu_up_token *token);
enum arm64_late_cpu_startup_claim arm64_late_cpu_startup_claim_failure(
	const struct arm64_late_cpu_startup_target *target,
	enum arm64_late_cpu_startup_branch branch);
int arm64_late_cpu_startup_park(
	const struct arm64_late_cpu_startup_target *target,
	enum arm64_late_cpu_startup_branch final_branch, u64 reason,
	u64 effects);
int arm64_late_cpu_startup_panic_park(
	const struct arm64_late_cpu_startup_target *target, u64 reason,
	u64 effects);
int arm64_late_cpu_startup_publish_panicked(
	const struct arm64_late_cpu_up_token *token);
void arm64_late_cpu_startup_snapshot(
	struct arm64_late_cpu_startup_snapshot *snapshot);
bool arm64_late_cpu_startup_quarantined(void);
#else
static inline bool arm64_late_cpu_startup_quarantined(void)
{
	return false;
}
#endif

#ifdef CONFIG_ARM64_LATE_CPU_STARTUP_KUNIT_TEST
void arm64_late_cpu_startup_test_reset(void);
void arm64_late_cpu_startup_test_set_online(u32 cpu, bool online);
#endif

#endif /* __ASM_LATE_CPU_STARTUP_H */
