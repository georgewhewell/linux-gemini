// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant generation-scoped late-CPU startup arbitration model
 *
 * This file deliberately has no production caller. It freezes a C-only P30
 * state machine without inventing the future membership token owner or
 * weakening the MT6797 CPU boot and disable vetoes. It is not an MMU-off wire
 * object and makes no P30E cache, point-of-coherency, or barrier claim.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <asm/late_cpu_startup.h>

struct arm64_late_cpu_startup_control {
	raw_spinlock_t lock;
	atomic_t state;
	struct arm64_late_cpu_up_token token;
	struct arm64_late_cpu_up_token
		retired_token[ARM64_LATE_CPU_STARTUP_OPERATION_SLOTS];
	struct arm64_late_cpu_up_token quarantine_token;
	struct arm64_late_cpu_startup_terminal terminal;
	u64 success_effects;
	struct completion published;
	atomic_t quarantined;
	u32 quarantine_cause;
	u32 retired_mask;
	bool completion_consumed;
	bool online_validated;
	bool park_committed;
	bool stuck_interlock;
#ifdef CONFIG_ARM64_LATE_CPU_STARTUP_KUNIT_TEST
	u32 test_online_cpu;
	bool test_online;
#endif
};

static struct arm64_late_cpu_startup_control late_startup = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(late_startup.lock),
	.state = ATOMIC_INIT(ARM64_LATE_CPU_STARTUP_FREE),
	.published = COMPLETION_INITIALIZER(late_startup.published),
	.quarantined = ATOMIC_INIT(0),
};

static int late_startup_operation_slot(u16 operation)
{
	switch (operation) {
	case ARM64_LATE_CPU_STARTUP_OP_CPU8_UP:
		return 0;
	case ARM64_LATE_CPU_STARTUP_OP_CPU9_UP:
		return 1;
	default:
		return -EINVAL;
	}
}

static bool late_startup_identity_empty(const u64 *identity)
{
	u64 value = 0;
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		value |= identity[i];

	return !value;
}

static bool
late_startup_token_valid(const struct arm64_late_cpu_up_token *token)
{
	if (!token || token->abi != ARM64_LATE_CPU_STARTUP_ABI ||
	    token->owner != ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP ||
	    token->reserved || !token->generation || !token->cookie ||
	    late_startup_identity_empty(token->plan_identity))
		return false;

	switch (token->operation) {
	case ARM64_LATE_CPU_STARTUP_OP_CPU8_UP:
		return token->target_cpu == 8 && token->target_mpidr == 0x200;
	case ARM64_LATE_CPU_STARTUP_OP_CPU9_UP:
		return token->target_cpu == 9 && token->target_mpidr == 0x201;
	default:
		return false;
	}
}

static bool
late_startup_token_matches_locked(const struct arm64_late_cpu_up_token *token)
{
	return token && !memcmp(token, &late_startup.token, sizeof(*token));
}

static bool late_startup_target_matches_locked(
	const struct arm64_late_cpu_startup_target *target)
{
	return target && !target->reserved &&
	       target->cpu == late_startup.token.target_cpu &&
	       target->mpidr == late_startup.token.target_mpidr &&
	       target->generation == late_startup.token.generation &&
	       target->cookie == late_startup.token.cookie;
}

static void
late_startup_latch_quarantine_locked(enum arm64_late_cpu_quarantine_cause cause)
{
	int state;

	if (atomic_read(&late_startup.quarantined))
		return;

	state = atomic_read(&late_startup.state);
	if (state == ARM64_LATE_CPU_STARTUP_FREE)
		return;
	late_startup.quarantine_token = late_startup.token;
	late_startup.quarantine_cause = cause;
	if (state == ARM64_LATE_CPU_STARTUP_PREPARED ||
	    state == ARM64_LATE_CPU_STARTUP_ABORTED ||
	    state == ARM64_LATE_CPU_STARTUP_ARMED)
		atomic_set_release(&late_startup.state,
				   ARM64_LATE_CPU_STARTUP_FAULTED);
	atomic_set_release(&late_startup.quarantined, 1);
}

static enum arm64_late_cpu_startup_claim
late_startup_classify_locked(enum arm64_late_cpu_startup_state state)
{
	switch (state) {
	case ARM64_LATE_CPU_STARTUP_PUBLISHING:
	case ARM64_LATE_CPU_STARTUP_PUBLISHED:
		return ARM64_LATE_CPU_STARTUP_CLAIM_PUBLISHER;
	case ARM64_LATE_CPU_STARTUP_CANCELLED:
		return ARM64_LATE_CPU_STARTUP_CLAIM_CANCELLED;
	case ARM64_LATE_CPU_STARTUP_FAILING:
		return ARM64_LATE_CPU_STARTUP_CLAIM_FAILURE;
	case ARM64_LATE_CPU_STARTUP_FAULTED:
		return ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED;
	case ARM64_LATE_CPU_STARTUP_PARKED:
	case ARM64_LATE_CPU_STARTUP_PANICKED:
		return ARM64_LATE_CPU_STARTUP_CLAIM_TERMINAL;
	default:
		return ARM64_LATE_CPU_STARTUP_CLAIM_STALE;
	}
}

static int
late_startup_require_token_locked(const struct arm64_late_cpu_up_token *token)
{
	if (late_startup_token_matches_locked(token))
		return 0;
	if (atomic_read(&late_startup.state) >= ARM64_LATE_CPU_STARTUP_ARMED)
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
	return -ESTALE;
}

static void late_startup_retire_token_locked(void)
{
	int slot = late_startup_operation_slot(late_startup.token.operation);

	if (slot < 0)
		return;
	late_startup.retired_token[slot] = late_startup.token;
	late_startup.retired_mask |= BIT(slot);
}

static void late_startup_clear_active_locked(void)
{
	memset(&late_startup.token, 0, sizeof(late_startup.token));
	memset(&late_startup.terminal, 0, sizeof(late_startup.terminal));
	late_startup.success_effects = 0;
	late_startup.completion_consumed = false;
	late_startup.online_validated = false;
	late_startup.park_committed = false;
	late_startup.stuck_interlock = false;
}

static bool late_startup_target_online_locked(u32 cpu)
{
#ifdef CONFIG_ARM64_LATE_CPU_STARTUP_KUNIT_TEST
	return cpu == late_startup.test_online_cpu && late_startup.test_online;
#else
	return cpu_online(cpu);
#endif
}

int arm64_late_cpu_startup_prepare(const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int slot;
	int state;
	int ret = 0;

	if (!late_startup_token_valid(token))
		return -EINVAL;
	slot = late_startup_operation_slot(token->operation);
	if (slot < 0)
		return slot;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	if (atomic_read(&late_startup.quarantined)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	state = atomic_read(&late_startup.state);
	if (state != ARM64_LATE_CPU_STARTUP_FREE) {
		if (state >= ARM64_LATE_CPU_STARTUP_ARMED)
			late_startup_latch_quarantine_locked(
				ARM64_LATE_CPU_QUARANTINE_REUSE);
		ret = -EBUSY;
		goto out;
	}
	if (late_startup.retired_mask & BIT(slot)) {
		ret = -EALREADY;
		goto out;
	}

	late_startup.token = *token;
	memset(&late_startup.terminal, 0, sizeof(late_startup.terminal));
	late_startup.success_effects = 0;
	late_startup.completion_consumed = false;
	late_startup.online_validated = false;
	late_startup.park_committed = false;
	late_startup.stuck_interlock = false;
	reinit_completion(&late_startup.published);
	atomic_set_release(&late_startup.state,
			   ARM64_LATE_CPU_STARTUP_PREPARED);
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_abort_unissued(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto out;
	if (atomic_read(&late_startup.quarantined)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (atomic_read(&late_startup.state) !=
	    ARM64_LATE_CPU_STARTUP_PREPARED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EPROTO;
		goto out;
	}

	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_ABORTED);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_release_aborted(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto out;
	if (atomic_read(&late_startup.quarantined)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (atomic_read(&late_startup.state) !=
	    ARM64_LATE_CPU_STARTUP_ABORTED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_REUSE);
		ret = -EPROTO;
		goto out;
	}

	late_startup_retire_token_locked();
	late_startup_clear_active_locked();
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FREE);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_arm_before_cpu_on(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (atomic_read(&late_startup.quarantined)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!late_startup_token_matches_locked(token)) {
		if (state == ARM64_LATE_CPU_STARTUP_PREPARED)
			atomic_set_release(&late_startup.state,
					   ARM64_LATE_CPU_STARTUP_FAULTED);
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		ret = -ESTALE;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_PREPARED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EPROTO;
		goto out;
	}

	/* The future owner must call this immediately before its first CPU_ON. */
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_ARMED);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_fault_cpu_on_unproven(
	const struct arm64_late_cpu_up_token *token,
	enum arm64_late_cpu_startup_state *winner)
{
	unsigned long flags;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (!late_startup_token_matches_locked(token)) {
		if (state == ARM64_LATE_CPU_STARTUP_PREPARED)
			atomic_set_release(&late_startup.state,
					   ARM64_LATE_CPU_STARTUP_FAULTED);
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		ret = -ESTALE;
		goto record;
	}
	if (state != ARM64_LATE_CPU_STARTUP_PREPARED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_CPU_ON_UNPROVEN);
		ret = -EUCLEAN;
		goto record;
	}

	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FAULTED);
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_CPU_ON_UNPROVEN);
	ret = 0;
record:
	if (winner)
		*winner = atomic_read(&late_startup.state);
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_cpu_on_fault(
	const struct arm64_late_cpu_up_token *token,
	enum arm64_late_cpu_startup_state *winner)
{
	unsigned long flags;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto record;
	if (state != ARM64_LATE_CPU_STARTUP_ARMED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_CPU_ON_FAULT);
		ret = -EUCLEAN;
		goto record;
	}

	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FAULTED);
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_CPU_ON_FAULT);
	ret = 0;
record:
	if (winner)
		*winner = atomic_read(&late_startup.state);
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

enum arm64_late_cpu_startup_claim
arm64_late_cpu_startup_cancel(const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	enum arm64_late_cpu_startup_claim claim;
	int state;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (late_startup_require_token_locked(token)) {
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_STALE;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_ARMED) {
		claim = late_startup_classify_locked(state);
		goto out;
	}

	atomic_set_release(&late_startup.state,
			   ARM64_LATE_CPU_STARTUP_CANCELLED);
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_CANCELLED);
	claim = ARM64_LATE_CPU_STARTUP_CLAIM_WON;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return claim;
}

enum arm64_late_cpu_startup_claim arm64_late_cpu_startup_claim_publish(
	const struct arm64_late_cpu_startup_target *target)
{
	unsigned long flags;
	enum arm64_late_cpu_startup_claim claim;
	int state;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	/* The lock acquire-observes state before any plain token field is read. */
	state = atomic_read(&late_startup.state);
	if (!late_startup_target_matches_locked(target)) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_STALE;
		goto out;
	}
	if (state == ARM64_LATE_CPU_STARTUP_PREPARED ||
	    state == ARM64_LATE_CPU_STARTUP_ABORTED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_ARMED) {
		if (state == ARM64_LATE_CPU_STARTUP_PUBLISHED &&
		    atomic_read(&late_startup.quarantined))
			claim = ARM64_LATE_CPU_STARTUP_CLAIM_QUARANTINED;
		else
			claim = late_startup_classify_locked(state);
		goto out;
	}
	if (atomic_read(&late_startup.quarantined)) {
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_QUARANTINED;
		goto out;
	}

	atomic_set_release(&late_startup.state,
			   ARM64_LATE_CPU_STARTUP_PUBLISHING);
	claim = ARM64_LATE_CPU_STARTUP_CLAIM_WON;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return claim;
}

int arm64_late_cpu_startup_publish(
	const struct arm64_late_cpu_startup_target *target, u64 effects)
{
	unsigned long flags;
	bool complete_published = false;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (!late_startup_target_matches_locked(target)) {
		if (state == ARM64_LATE_CPU_STARTUP_PREPARED)
			atomic_set_release(&late_startup.state,
					   ARM64_LATE_CPU_STARTUP_FAULTED);
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		ret = -ESTALE;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_PUBLISHING) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EPROTO;
		goto out;
	}
	if (effects & ~ARM64_LATE_CPU_EFFECT_MASK) {
		/* The owner retains PUBLISHING and may finish with a valid record. */
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EINVAL;
		goto out;
	}

	/* No cancellable check is permitted inside this owned publication. */
	late_startup.success_effects = effects;
	atomic_set_release(&late_startup.state,
			   ARM64_LATE_CPU_STARTUP_PUBLISHED);
	complete_published = true;
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	if (complete_published)
		complete(&late_startup.published);
	return ret;
}

int arm64_late_cpu_startup_consume_published(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	bool quarantined;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto out;
	if (atomic_read(&late_startup.state) !=
	    ARM64_LATE_CPU_STARTUP_PUBLISHED) {
		ret = -EAGAIN;
		goto out;
	}
	if (late_startup.completion_consumed) {
		ret = atomic_read(&late_startup.quarantined) ? -ESHUTDOWN :
							       -EALREADY;
		goto out;
	}
	if (!try_wait_for_completion(&late_startup.published)) {
		ret = -EAGAIN;
		goto out;
	}

	late_startup.completion_consumed = true;
	quarantined = atomic_read(&late_startup.quarantined);
	/* Sample the exact CPU only after observing PUBLISHED and completion. */
	if (!late_startup_target_online_locked(token->target_cpu)) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ONLINE_MISMATCH);
		ret = quarantined ? -ESHUTDOWN : -EUCLEAN;
		goto out;
	}
	late_startup.online_validated = true;
	ret = quarantined ? -ESHUTDOWN : 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_retire_published_after_p14_p15(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto out;
	if (atomic_read(&late_startup.quarantined)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (atomic_read(&late_startup.state) !=
		    ARM64_LATE_CPU_STARTUP_PUBLISHED ||
	    !late_startup.completion_consumed ||
	    !late_startup.online_validated) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_REUSE);
		ret = -EPROTO;
		goto out;
	}

	late_startup_retire_token_locked();
	late_startup_clear_active_locked();
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FREE);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

enum arm64_late_cpu_startup_claim arm64_late_cpu_startup_claim_failure(
	const struct arm64_late_cpu_startup_target *target,
	enum arm64_late_cpu_startup_branch branch)
{
	unsigned long flags;
	enum arm64_late_cpu_startup_claim claim;
	int state;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	/* The lock acquire-observes state before any plain token field is read. */
	state = atomic_read(&late_startup.state);
	if (!late_startup_target_matches_locked(target)) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_STALE;
		goto out;
	}
	if (state == ARM64_LATE_CPU_STARTUP_PREPARED ||
	    state == ARM64_LATE_CPU_STARTUP_ABORTED) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_FAULTED;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_ARMED) {
		claim = late_startup_classify_locked(state);
		goto out;
	}
	if (branch < ARM64_LATE_CPU_STARTUP_BRANCH_K ||
	    branch > ARM64_LATE_CPU_STARTUP_BRANCH_U) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		claim = ARM64_LATE_CPU_STARTUP_CLAIM_STALE;
		goto out;
	}

	late_startup.terminal.claimed_branch = branch;
	late_startup.terminal.branch = branch;
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FAILING);
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_TARGET_FAILURE);
	claim = ARM64_LATE_CPU_STARTUP_CLAIM_WON;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return claim;
}

int arm64_late_cpu_startup_park(
	const struct arm64_late_cpu_startup_target *target,
	enum arm64_late_cpu_startup_branch final_branch, u64 reason,
	u64 effects)
{
	unsigned long flags;
	enum arm64_late_cpu_startup_branch claimed;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (!late_startup_target_matches_locked(target)) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		ret = -ESTALE;
		goto out;
	}
	if (!reason || effects & ~ARM64_LATE_CPU_EFFECT_MASK) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EINVAL;
		goto out;
	}
	if (late_startup.park_committed)
		goto illegal;

	claimed = late_startup.terminal.claimed_branch;
	if (state == ARM64_LATE_CPU_STARTUP_CANCELLED ||
	    state == ARM64_LATE_CPU_STARTUP_FAULTED) {
		if (final_branch != ARM64_LATE_CPU_STARTUP_BRANCH_U)
			goto illegal;
	} else if (state == ARM64_LATE_CPU_STARTUP_FAILING) {
		/* P must use panic_park() and the controller interlock. */
		if (claimed == ARM64_LATE_CPU_STARTUP_BRANCH_P)
			goto illegal;
		if (final_branch != claimed &&
		    !(claimed == ARM64_LATE_CPU_STARTUP_BRANCH_K &&
		      final_branch == ARM64_LATE_CPU_STARTUP_BRANCH_C))
			goto illegal;
	} else {
		goto illegal;
	}

	/* Branch-specific effect policy belongs to the future integrated hooks. */
	late_startup.terminal.terminal_from = state;
	late_startup.terminal.branch = final_branch;
	late_startup.terminal.reason = reason;
	late_startup.terminal.effects = effects;
	late_startup.park_committed = true;
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_PARKED);
	late_startup_latch_quarantine_locked(ARM64_LATE_CPU_QUARANTINE_PARKED);
	ret = 0;
	goto out;

illegal:
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
	ret = -EPROTO;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_panic_park(
	const struct arm64_late_cpu_startup_target *target, u64 reason,
	u64 effects)
{
	unsigned long flags;
	int state;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	state = atomic_read(&late_startup.state);
	if (!late_startup_target_matches_locked(target)) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_IDENTITY);
		ret = -ESTALE;
		goto out;
	}
	if (!reason || effects & ~ARM64_LATE_CPU_EFFECT_MASK) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EINVAL;
		goto out;
	}
	if (state != ARM64_LATE_CPU_STARTUP_FAILING ||
	    late_startup.terminal.claimed_branch !=
		    ARM64_LATE_CPU_STARTUP_BRANCH_P ||
	    late_startup.park_committed) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EPROTO;
		goto out;
	}

	late_startup.terminal.terminal_from = state;
	late_startup.terminal.branch = ARM64_LATE_CPU_STARTUP_BRANCH_P;
	late_startup.terminal.reason = reason;
	late_startup.terminal.effects = effects;
	late_startup.park_committed = true;
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

int arm64_late_cpu_startup_publish_panicked(
	const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	ret = late_startup_require_token_locked(token);
	if (ret)
		goto out;
	if (atomic_read(&late_startup.state) !=
		    ARM64_LATE_CPU_STARTUP_FAILING ||
	    late_startup.terminal.claimed_branch !=
		    ARM64_LATE_CPU_STARTUP_BRANCH_P ||
	    late_startup.terminal.branch != ARM64_LATE_CPU_STARTUP_BRANCH_P ||
	    !late_startup.park_committed || late_startup.stuck_interlock) {
		late_startup_latch_quarantine_locked(
			ARM64_LATE_CPU_QUARANTINE_ILLEGAL_EDGE);
		ret = -EPROTO;
		goto out;
	}

	late_startup.stuck_interlock = true;
	atomic_set_release(&late_startup.state,
			   ARM64_LATE_CPU_STARTUP_PANICKED);
	late_startup_latch_quarantine_locked(
		ARM64_LATE_CPU_QUARANTINE_PANICKED);
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
	return ret;
}

void arm64_late_cpu_startup_snapshot(
	struct arm64_late_cpu_startup_snapshot *snapshot)
{
	unsigned long flags;

	if (!snapshot)
		return;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	snapshot->state = atomic_read(&late_startup.state);
	snapshot->token = late_startup.token;
	memcpy(snapshot->retired_token, late_startup.retired_token,
	       sizeof(snapshot->retired_token));
	snapshot->quarantine_token = late_startup.quarantine_token;
	snapshot->terminal = late_startup.terminal;
	snapshot->success_effects = late_startup.success_effects;
	snapshot->completion_consumed = late_startup.completion_consumed;
	snapshot->online_validated = late_startup.online_validated;
	snapshot->park_committed = late_startup.park_committed;
	snapshot->stuck_interlock = late_startup.stuck_interlock;
	snapshot->quarantined = atomic_read(&late_startup.quarantined);
	snapshot->quarantine_cause = late_startup.quarantine_cause;
	snapshot->retired_mask = late_startup.retired_mask;
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
}

bool arm64_late_cpu_startup_quarantined(void)
{
	return atomic_read_acquire(&late_startup.quarantined);
}

#ifdef CONFIG_ARM64_LATE_CPU_STARTUP_KUNIT_TEST
void arm64_late_cpu_startup_test_reset(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	memset(&late_startup.token, 0, sizeof(late_startup.token));
	memset(late_startup.retired_token, 0,
	       sizeof(late_startup.retired_token));
	memset(&late_startup.quarantine_token, 0,
	       sizeof(late_startup.quarantine_token));
	memset(&late_startup.terminal, 0, sizeof(late_startup.terminal));
	late_startup.success_effects = 0;
	late_startup.quarantine_cause = ARM64_LATE_CPU_QUARANTINE_NONE;
	late_startup.retired_mask = 0;
	late_startup.completion_consumed = false;
	late_startup.online_validated = false;
	late_startup.park_committed = false;
	late_startup.stuck_interlock = false;
	late_startup.test_online_cpu = 0;
	late_startup.test_online = false;
	reinit_completion(&late_startup.published);
	atomic_set(&late_startup.quarantined, 0);
	atomic_set_release(&late_startup.state, ARM64_LATE_CPU_STARTUP_FREE);
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
}

void arm64_late_cpu_startup_test_set_online(u32 cpu, bool online)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&late_startup.lock, flags);
	late_startup.test_online_cpu = cpu;
	late_startup.test_online = online;
	raw_spin_unlock_irqrestore(&late_startup.lock, flags);
}
#endif
