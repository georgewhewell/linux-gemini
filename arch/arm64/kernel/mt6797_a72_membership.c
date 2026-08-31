// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lifecycle-closed transaction owner for future MT6797 A72 membership.
 *
 * The transaction-begin path deliberately has no production caller or opener.
 * Read-only admission callbacks may inspect CLOSED. A dormant P31 ledger
 * consumes a one-shot attempt only in a test-seeded AVAILABLE state, then
 * retains the transition lock across A28, frozen-token minting, and the
 * immutable owner-derived A36 check. A dormant P17/P18 publication ledger may
 * advance only the C phase to ON_ISSUED; provider calls and all hardware,
 * CPUHP, and CPU_ON effects remain absent. R03/P29 only records a returned
 * before-vote rejection and an exact pre-isolation rollback; it performs no
 * inverse hardware operation.
 */

#include <linux/compiler.h>
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/mt6797-a72-provider.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include <asm/memory.h>
#include <asm/cpu_ops.h>
#include <asm/mt6797_a72_membership.h>
#include <asm/smp.h>
#ifdef CONFIG_ARM64_MT6797_A72_P30E_WIRE
#include <asm/mt6797_a72_p30e.h>
#endif

extern const struct cpu_operations mt6797_psci_ops;

static DEFINE_MUTEX(a72_transition_lock);
static DEFINE_RAW_SPINLOCK(a72_state_lock);
static DEFINE_MUTEX(a72_provider_registry_lock);

static const struct mt6797_a72_provider_ops *a72_provider_ops;
static void *a72_provider_context;

/* All callers retain a72_transition_lock while these workspaces are used. */
static struct mt6797_a72_owner_snapshot a72_direct_owner_workspace;
static struct mt6797_a72_direct_source_snapshot a72_direct_source_workspace;
static struct mt6797_a72_transaction a72_mint_workspace;

int mt6797_a72_provider_register(const struct mt6797_a72_provider_ops *ops,
				 void *context)
{
	int ret = 0;

	if (!ops || !ops->acquire || !ops->release)
		return -EINVAL;

	mutex_lock(&a72_provider_registry_lock);
	if (a72_provider_ops) {
		ret = -EBUSY;
	} else {
		a72_provider_ops = ops;
		a72_provider_context = context;
	}
	mutex_unlock(&a72_provider_registry_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_register);

void mt6797_a72_provider_unregister(const struct mt6797_a72_provider_ops *ops,
				    void *context)
{
	mutex_lock(&a72_provider_registry_lock);
	if (a72_provider_ops == ops && a72_provider_context == context) {
		a72_provider_ops = NULL;
		a72_provider_context = NULL;
	}
	mutex_unlock(&a72_provider_registry_lock);
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_unregister);

bool mt6797_a72_provider_available(void)
{
	bool available;

	mutex_lock(&a72_provider_registry_lock);
	available = !!a72_provider_ops;
	mutex_unlock(&a72_provider_registry_lock);
	return available;
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_available);

int mt6797_a72_provider_acquire(
	const struct mt6797_a72_provider_request *request,
	struct mt6797_a72_provider_response *response)
{
	int ret;

	if (!request || !response)
		return -EINVAL;

	mutex_lock(&a72_provider_registry_lock);
	if (!a72_provider_ops)
		ret = -ENODEV;
	else
		ret = a72_provider_ops->acquire(a72_provider_context,
					       request, response);
	mutex_unlock(&a72_provider_registry_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_acquire);

int mt6797_a72_provider_release(
	const struct mt6797_a72_provider_handle *handle,
	struct mt6797_a72_provider_response *response)
{
	int ret;

	if (!handle || !response)
		return -EINVAL;

	mutex_lock(&a72_provider_registry_lock);
	if (!a72_provider_ops)
		ret = -ENODEV;
	else
		ret = a72_provider_ops->release(a72_provider_context, handle,
						response);
	mutex_unlock(&a72_provider_registry_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_release);

int mt6797_a72_provider_snapshot(struct mt6797_a72_provider_snapshot *state)
{
	struct mt6797_a72_provider_snapshot observed = { };
	u32 raw;
	int ret;

	if (!state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));

	mutex_lock(&a72_provider_registry_lock);
	if (!a72_provider_ops)
		ret = -ENODEV;
	else if (!a72_provider_ops->snapshot)
		ret = -EOPNOTSUPP;
	else
		ret = a72_provider_ops->snapshot(a72_provider_context,
						 &observed);
	if (!ret) {
		raw = observed.control_a | observed.status_b |
			observed.buckb_cont | observed.vbuckb_a |
			observed.vbuckb_b;
		if (observed.abi != MT6797_A72_PROVIDER_STATE_ABI ||
		    observed.valid != 1 || observed.reserved ||
		    (raw & ~0xffU))
			ret = -EPROTO;
		else
			*state = observed;
	}
	mutex_unlock(&a72_provider_registry_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_provider_snapshot);

struct mt6797_a72_owner_state {
	struct mt6797_a72_transaction active;
	struct mt6797_a72_transaction retired[MT6797_A72_OPERATION_SLOTS];
	struct mt6797_a72_provider_identity provider_identity;
	struct mt6797_a72_fault_record first_fault;
	struct task_struct *controller;
	u64 diagnostic_blockers;
	u64 controller_cookie;
	u64 next_generation;
	u64 next_cookie;
	u32 health;
	u32 phase;
	u32 members;
	u32 provider_state;
	u32 bootstrap_valid;
	u32 members_valid;
	u32 attempts_available;
	u32 attempts_consumed;
	u32 retired_mask;
};

static struct mt6797_a72_owner_state a72_owner = {
	.diagnostic_blockers = MT6797_A72_BLOCK_MASK,
	.health = MT6797_A72_OWNER_CLOSED,
	.phase = MT6797_A72_PHASE_UNINITIALIZED,
	.provider_state = MT6797_A72_PROVIDER_NONE,
};

#ifdef CONFIG_ARM64_MT6797_A72_DIRECT_STATE_COMPOSITOR
static DEFINE_MUTEX(a72_direct_source_registry_lock);
static const struct mt6797_a72_direct_source_ops *a72_direct_source_ops;
static void *a72_direct_source_context;

int mt6797_a72_direct_source_register(const struct mt6797_a72_direct_source_ops *ops,
				      void *context)
{
	int ret = 0;
	if (!ops || !ops->snapshot)
		return -EINVAL;
	mutex_lock(&a72_direct_source_registry_lock);
	if (a72_direct_source_ops)
		ret = -EBUSY;
	else {
		a72_direct_source_ops = ops;
		a72_direct_source_context = context;
	}
	mutex_unlock(&a72_direct_source_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_direct_source_register);

void mt6797_a72_direct_source_unregister(const struct mt6797_a72_direct_source_ops *ops,
					 void *context)
{
	mutex_lock(&a72_direct_source_registry_lock);
	if (a72_direct_source_ops == ops && a72_direct_source_context == context) {
		a72_direct_source_ops = NULL;
		a72_direct_source_context = NULL;
	}
	mutex_unlock(&a72_direct_source_registry_lock);
}
EXPORT_SYMBOL_GPL(mt6797_a72_direct_source_unregister);

static int mt6797_a72_direct_state_snapshot_locked(struct mt6797_a72_direct_state_snapshot *snapshot)
{
	int ret;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	if (nr_cpu_ids <= 9)
		return -ENODEV;
	if (!cpu_possible(8) || !cpu_possible(9) || !cpu_present(8) ||
	    !cpu_present(9) || cpu_online(8) || cpu_online(9)) {
		ret = -EPERM;
		return ret;
	}
	mutex_lock(&a72_direct_source_registry_lock);
	if (!a72_direct_source_ops) {
		ret = -ENODEV;
		goto out_source;
	}
	memset(&a72_direct_source_workspace, 0,
	       sizeof(a72_direct_source_workspace));
	ret = a72_direct_source_ops->snapshot(a72_direct_source_context,
					      &a72_direct_source_workspace);
	if (ret)
		goto out_source;
	if (a72_direct_source_workspace.abi != MT6797_A72_DIRECT_SOURCE_ABI ||
	    a72_direct_source_workspace.valid != 1 ||
	    a72_direct_source_workspace.reserved[0] ||
	    a72_direct_source_workspace.reserved[1] ||
	    !a72_direct_source_workspace.platform.valid ||
	    !a72_direct_source_workspace.provider.valid ||
	    !a72_direct_source_workspace.clock.sample_generation ||
	    !a72_direct_source_workspace.bigidvfs.sample_generation) {
		ret = -EPROTO;
		goto out_source;
	}
	mt6797_a72_membership_snapshot(&a72_direct_owner_workspace);
	if (a72_direct_owner_workspace.health != MT6797_A72_OWNER_CLOSED ||
	    a72_direct_owner_workspace.phase != MT6797_A72_PHASE_UNINITIALIZED ||
	    a72_direct_owner_workspace.provider_state != MT6797_A72_PROVIDER_NONE ||
	    a72_direct_owner_workspace.controller_present ||
	    a72_direct_owner_workspace.attempts_available ||
	    a72_direct_owner_workspace.attempts_consumed) {
		ret = -EPERM;
		goto out_source;
	}
	snapshot->abi = MT6797_A72_DIRECT_STATE_ABI;
	snapshot->valid = 1;
	snapshot->cpu8_possible = snapshot->cpu8_present = 1;
	snapshot->cpu9_possible = snapshot->cpu9_present = 1;
	snapshot->cpu8_online = snapshot->cpu9_online = 0;
	snapshot->cpu8_method_valid = snapshot->cpu9_method_valid = 1;
	snapshot->cpu8_mpidr = cpu_logical_map(8);
	snapshot->cpu9_mpidr = cpu_logical_map(9);
	snapshot->source = a72_direct_source_workspace;
	snapshot->owner = a72_direct_owner_workspace;
	ret = 0;
out_source:
	mutex_unlock(&a72_direct_source_registry_lock);
	return ret;
}

int mt6797_a72_direct_state_snapshot(struct mt6797_a72_direct_state_snapshot *snapshot)
{
	int ret;

	cpus_read_lock();
	mutex_lock(&a72_transition_lock);
	ret = mt6797_a72_direct_state_snapshot_locked(snapshot);
	mutex_unlock(&a72_transition_lock);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_direct_state_snapshot);
#endif

static void mt6797_a72_derive_stage_set(u32 *stage, u32 value)
{
	if (stage)
		*stage = value;
}

#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_A34_ELIGIBILITY_EVALUATOR)
int mt6797_a72_a34_evaluate(const struct mt6797_a72_a34_observation *observation)
{
	const struct mt6797_a72_direct_state_snapshot *direct;
	const struct mt6797_a72_owner_snapshot *owner;

	if (!observation || observation->abi != MT6797_A72_A34_ELIGIBILITY_ABI ||
	    observation->reserved[0] || observation->reserved[1] ||
	    observation->reserved[2])
		return -EINVAL;
	if (observation->replay.abi != MT6797_A72_A34_REPLAY_ABI ||
	    observation->replay.valid != 1 ||
	    observation->replay.proof !=
		MT6797_A72_A34_REPLAY_APPLICABLE_PRIMARY_BL31_CLEAR ||
	    observation->replay.private_replay_value ||
	    observation->replay.reserved[0] || observation->replay.reserved[1] ||
	    observation->replay.reserved[2] || observation->replay.reserved[3])
		return -EPERM;
	direct = &observation->direct;
	if (direct->abi != MT6797_A72_DIRECT_STATE_ABI || direct->valid != 1 ||
	    !direct->cpu8_possible || !direct->cpu9_possible ||
	    !direct->cpu8_present || !direct->cpu9_present ||
	    direct->cpu8_online || direct->cpu9_online ||
	    !direct->cpu8_method_valid || !direct->cpu9_method_valid ||
	    direct->cpu8_mpidr != 0x200 || direct->cpu9_mpidr != 0x201)
		return -EPERM;
	owner = &direct->owner;
	if (owner->abi != MT6797_A72_TRANSACTION_ABI ||
	    owner->health != MT6797_A72_OWNER_CLOSED ||
	    owner->phase != MT6797_A72_PHASE_UNINITIALIZED ||
	    owner->provider_state != MT6797_A72_PROVIDER_NONE ||
	    owner->diagnostic_blockers != MT6797_A72_BLOCK_MASK ||
	    owner->bootstrap_valid || owner->members_valid ||
	    owner->attempts_available || owner->attempts_consumed ||
	    owner->controller_present || owner->active.valid)
		return -EPERM;
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_BOOTSTRAP_PUBLISHER)
/*
 * Bootstrap runs under a72_transition_lock.  Keep the large observation
 * objects out of the kernel stack; this path is reachable from the CPU-start
 * admission machinery, where a several-kilobyte frame is not acceptable.
 */
static struct mt6797_a72_direct_state_snapshot a72_bootstrap_direct;
static struct mt6797_a72_a34_observation a72_bootstrap_observation;

static int mt6797_a72_membership_publish_bootstrap_locked(
	const struct mt6797_a72_direct_topology *topology,
	const struct mt6797_a72_a34_replay *replay,
	bool dirty_owner_before_finalize,
	struct mt6797_a72_direct_state_snapshot *direct,
	u32 *derive_stage)
{
	unsigned long flags;
	int ret;

	(void)dirty_owner_before_finalize;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_BOOTSTRAP_OWNER);
	if (!topology || !replay || !direct)
		return -EINVAL;
	if (topology->cpu8_possible != 1 || topology->cpu9_possible != 1 ||
	    topology->cpu8_present != 1 || topology->cpu9_present != 1 ||
	    topology->cpu8_online || topology->cpu9_online ||
	    !topology->cpu8_method_valid || !topology->cpu9_method_valid ||
	    topology->cpu8_mpidr != 0x200 || topology->cpu9_mpidr != 0x201)
		return -EPERM;
	ret = mt6797_a72_direct_state_snapshot_locked(direct);
	if (ret)
		return ret;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_A34);
	memset(&a72_bootstrap_observation, 0,
	       sizeof(a72_bootstrap_observation));
	a72_bootstrap_observation.abi = MT6797_A72_A34_ELIGIBILITY_ABI;
	a72_bootstrap_observation.direct = *direct;
	a72_bootstrap_observation.replay = *replay;
	ret = mt6797_a72_a34_evaluate(&a72_bootstrap_observation);
	if (ret)
		return ret;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_BOOTSTRAP_FINALIZE);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_CLOSED ||
	    a72_owner.phase != MT6797_A72_PHASE_UNINITIALIZED ||
	    a72_owner.next_generation || a72_owner.next_cookie ||
	    a72_owner.attempts_available || a72_owner.attempts_consumed ||
	    a72_owner.controller) {
		ret = -EBUSY;
	} else {
		a72_owner.diagnostic_blockers &= ~MT6797_A72_BLOCK_A34_BOOTSTRAP;
		a72_owner.health = MT6797_A72_OWNER_AVAILABLE;
		a72_owner.phase = MT6797_A72_PHASE_IDLE;
		a72_owner.bootstrap_valid = 1;
		a72_owner.members_valid = 1;
		a72_owner.attempts_available = MT6797_A72_ATTEMPT_MASK;
		a72_owner.next_generation = 1;
		a72_owner.next_cookie = 0xa7200001;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

static int mt6797_a72_membership_publish_bootstrap_public(
	const struct mt6797_a72_a34_replay *replay)
{
	struct mt6797_a72_direct_topology topology = { };

	if (nr_cpu_ids <= 9)
		return -ENODEV;
	topology.cpu8_possible = cpu_possible(8);
	topology.cpu9_possible = cpu_possible(9);
	topology.cpu8_present = cpu_present(8);
	topology.cpu9_present = cpu_present(9);
	topology.cpu8_online = cpu_online(8);
	topology.cpu9_online = cpu_online(9);
	topology.cpu8_method_valid = get_cpu_ops(8) == &mt6797_psci_ops;
	topology.cpu9_method_valid = get_cpu_ops(9) == &mt6797_psci_ops;
	topology.cpu8_mpidr = cpu_logical_map(8);
	topology.cpu9_mpidr = cpu_logical_map(9);
	memset(&a72_bootstrap_direct, 0, sizeof(a72_bootstrap_direct));
	return mt6797_a72_membership_publish_bootstrap_locked(&topology, replay,
								false, &a72_bootstrap_direct, NULL);
}

int mt6797_a72_membership_publish_bootstrap(
	const struct mt6797_a72_a34_replay *replay)
{
	int ret;

	cpus_read_lock();
	mutex_lock(&a72_transition_lock);
	ret = mt6797_a72_membership_publish_bootstrap_public(replay);
	mutex_unlock(&a72_transition_lock);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_membership_publish_bootstrap);
#endif

static bool
mt6797_a72_p32_target_locked(unsigned int cpu)
{
	const struct mt6797_a72_transaction *transaction = &a72_owner.active;

	return a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
		transaction->valid && transaction->p32_valid &&
		transaction->p32.state != MT6797_A72_P32_STATE_CONSUMED &&
		transaction->p32.abi == MT6797_A72_P32_ROLLBACK_ABI &&
		transaction->p32.target_cpu == cpu &&
		(transaction->identity.operation ==
		 ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
		 transaction->identity.operation ==
		 ARM64_LATE_CPU_STARTUP_OP_CPU9_UP) &&
		transaction->identity.operation == (cpu == 8 ?
		 ARM64_LATE_CPU_STARTUP_OP_CPU8_UP : ARM64_LATE_CPU_STARTUP_OP_CPU9_UP) &&
		transaction->identity.target_cpu == cpu &&
		transaction->p30_token.target_cpu == cpu &&
		transaction->p30_token.target_mpidr ==
			transaction->p32.target_mpidr &&
		transaction->identity.generation == transaction->p32.generation &&
		transaction->identity.cookie == transaction->p32.cookie;
}

int mt6797_a72_membership_publish_p32(unsigned int cpu,
				      enum cpuhp_state callback_state,
				      int error,
				      const struct cpu_up_rollback_trace *trace)
{
	unsigned long flags;
	struct mt6797_a72_transaction *transaction;
	int ret = -EPERM;

	if ((cpu != 8 && cpu != 9) || !error || !trace)
		return -EINVAL;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	transaction = &a72_owner.active;
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    (a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED &&
	     a72_owner.phase != MT6797_A72_PHASE_VERIFYING) ||
	    !transaction->valid || transaction->p32_valid ||
	    transaction->identity.target_cpu != cpu ||
	    (transaction->identity.operation !=
		 ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
	     transaction->identity.operation !=
		 ARM64_LATE_CPU_STARTUP_OP_CPU9_UP) ||
	    transaction->identity.operation != (cpu == 8 ?
		 ARM64_LATE_CPU_STARTUP_OP_CPU8_UP : ARM64_LATE_CPU_STARTUP_OP_CPU9_UP) ||
	    transaction->identity.generation == 0 ||
	    transaction->identity.cookie == 0) {
		ret = -EAGAIN;
		goto out;
	}

	transaction->p32.abi = MT6797_A72_P32_ROLLBACK_ABI;
	transaction->p32.state = MT6797_A72_P32_STATE_PUBLISHED;
	transaction->p32.branch = MT6797_A72_P32_BRANCH_A;
	transaction->p32.cpu_up_error = error;
	transaction->p32.callback_state = callback_state;
	transaction->p32.target_cpu = cpu;
	transaction->p32.target_mpidr = transaction->identity.target_mpidr;
	transaction->p32.generation = transaction->identity.generation;
	transaction->p32.cookie = transaction->identity.cookie;
	transaction->p32.callback_trace = *trace;
	transaction->p32.callback_trace_valid = 1;
	transaction->p32_valid = 1;
	a72_owner.diagnostic_blockers |= MT6797_A72_BLOCK_P32_ROLLBACK;
	a72_owner.phase = MT6797_A72_PHASE_FAULT;
	ret = 0;
out:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

int mt6797_a72_membership_p32_cpu_disable(unsigned int cpu)
{
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (mt6797_a72_p32_target_locked(cpu)) {
		if (a72_owner.active.p32.guard_flags &
		    MT6797_A72_P32_GUARD_DISABLE)
			ret = -EALREADY;
		else {
			a72_owner.active.p32.guard_flags |=
				MT6797_A72_P32_GUARD_DISABLE;
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_D;
			a72_owner.active.p32.state = MT6797_A72_P32_STATE_GUARDED;
			ret = -EIO;
		}
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

bool mt6797_a72_membership_p32_cpu_die(unsigned int cpu)
{
	unsigned long flags;
	bool park = false;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (mt6797_a72_p32_target_locked(cpu)) {
		a72_owner.active.p32.guard_flags |=
			MT6797_A72_P32_GUARD_DIE | MT6797_A72_P32_GUARD_PARK;
		a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_F;
		a72_owner.active.p32.state = MT6797_A72_P32_STATE_PARKED;
		park = true;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return park;
}

int mt6797_a72_membership_p32_cpu_kill(unsigned int cpu)
{
	unsigned int spin;

	for (spin = 0; spin < MT6797_A72_P32_KILL_SPINS; spin++) {
		unsigned long flags;
		bool matched;
		bool parked;

		raw_spin_lock_irqsave(&a72_state_lock, flags);
		matched = mt6797_a72_p32_target_locked(cpu);
		parked = matched &&
			a72_owner.active.p32.state == MT6797_A72_P32_STATE_PARKED;
		if (parked) {
			a72_owner.active.p32.guard_flags |=
				MT6797_A72_P32_GUARD_KILL;
			raw_spin_unlock_irqrestore(&a72_state_lock, flags);
			return -EIO;
		}
		raw_spin_unlock_irqrestore(&a72_state_lock, flags);
		if (!matched)
			return 0;
		cpu_relax();
	}

	return -ETIMEDOUT;
}
static void
mt6797_a72_p32r_capture_locked(struct mt6797_a72_transaction *transaction,
				       const struct cpu_up_rollback_trace *trace,
				       u32 disposition)
{
	struct mt6797_a72_p32r_ledger_handoff *handoff =
		&transaction->p32r;

	memset(handoff, 0, sizeof(*handoff));
	handoff->abi = MT6797_A72_P32R_HANDOFF_ABI;
	handoff->valid = 1;
	handoff->disposition = disposition;
	handoff->identity = transaction->identity;
	handoff->membership = transaction->entry;
	handoff->provider_identity = a72_owner.provider_identity;
	handoff->provider_state = a72_owner.provider_state;
	handoff->owner_phase = a72_owner.phase;
	handoff->members = a72_owner.members;
	handoff->diagnostic_blockers = a72_owner.diagnostic_blockers;
	if (trace) {
		handoff->callback_complete = trace->callback_complete;
		handoff->callback_overflow = trace->overflow;
		handoff->callback_unknown = trace->callback_unknown;
		handoff->callback_capacity = trace->callback_capacity;
		handoff->callback_inventory = trace->callback_inventory;
		handoff->effect_complete = trace->effect_complete;
		handoff->effect_overflow = trace->effect_overflow;
		handoff->effect_unknown = trace->effect_unknown;
		handoff->effect_required_mask = trace->effect_required_mask;
		handoff->effect_seen_mask = trace->effect_seen_mask;
		handoff->effect_missing_mask = trace->effect_missing_mask;
		handoff->effect_forbidden_mask = trace->effect_forbidden_mask;
	}
	transaction->p32r_valid = 1;
}

static void
mt6797_a72_p32r_lose_locked(struct mt6797_a72_transaction *transaction,
				    const struct cpu_up_rollback_trace *trace)
{
	mt6797_a72_p32r_capture_locked(transaction, trace,
					MT6797_A72_P32R_FAULT_ROLLBACK_LOST);
	a72_owner.health = MT6797_A72_OWNER_FAULTED;
	a72_owner.phase = MT6797_A72_PHASE_FAULT;
}

static int
mt6797_a72_p32r_retire_locked(struct mt6797_a72_transaction *transaction)
{
	unsigned int slot;

	if (transaction->identity.operation <
			ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
		transaction->identity.operation >
			ARM64_LATE_CPU_STARTUP_OP_CPU9_UP)
		return -EINVAL;
	slot = transaction->identity.operation -
		ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
	a72_owner.retired[slot] = *transaction;
	a72_owner.retired_mask |= BIT(slot);
	memset(&a72_owner.active, 0, sizeof(a72_owner.active));
	a72_owner.controller = NULL;
	a72_owner.controller_cookie = 0;
	a72_owner.health = MT6797_A72_OWNER_FAULTED;
	a72_owner.phase = MT6797_A72_PHASE_FAULT;
	return 0;
}

int mt6797_a72_membership_consume_p32(unsigned int cpu, int rollback_error,
					      const struct cpu_up_rollback_trace *trace)
{
	unsigned long flags;
	int ret = -EAGAIN;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (mt6797_a72_p32_target_locked(cpu)) {
		if (!trace) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, NULL);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EINVAL;
			goto out;
		}
		a72_owner.active.p32.callback_trace = *trace;
		a72_owner.active.p32.callback_trace_valid = 1;
		if (trace->overflow || !trace->outer_reset ||
		    !trace->reverse_complete) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EIO;
			goto out;
		}
		if (a72_owner.active.p32.state != MT6797_A72_P32_STATE_PARKED) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EIO;
			goto out;
		}
		if (a72_owner.active.p32.cpu_up_error != rollback_error) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EUCLEAN;
			goto out;
		}
		if (a72_owner.active.p32r_valid ||
		    (a72_owner.active.p32r_side_effects &
		     MT6797_A72_P32R_EFFECT_MASK)) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EPERM;
			goto out;
		}
		if (!trace->callback_complete || trace->callback_unknown ||
		    trace->effect_unknown || trace->effect_overflow ||
		    !trace->effect_complete ||
		    trace->callback_capacity == 0 ||
		    trace->callback_inventory > trace->callback_capacity ||
		    trace->event_count > trace->callback_inventory ||
		    trace->effect_missing_mask ||
		    trace->effect_forbidden_mask) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			ret = -EIO;
			goto out;
		}

		a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_R;
		a72_owner.active.p32.state = MT6797_A72_P32_STATE_CONSUMED;
		mt6797_a72_p32r_capture_locked(&a72_owner.active, trace,
					MT6797_A72_P32R_FAULT_ROLLBACK_RECORDED);
		if (a72_owner.provider_state != MT6797_A72_PROVIDER_NONE)
			a72_owner.provider_state = MT6797_A72_PROVIDER_FAULT_UNKNOWN;
		ret = mt6797_a72_p32r_retire_locked(&a72_owner.active);
		if (ret) {
			mt6797_a72_p32r_lose_locked(&a72_owner.active, trace);
			a72_owner.active.p32.branch = MT6797_A72_P32_BRANCH_X;
			goto out;
		}
		ret = 0;
	}
out:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

static enum mt6797_a72_membership_operation
mt6797_a72_up_operation(unsigned int cpu)
{
	switch (cpu) {
	case 8:
		return MT6797_A72_OPERATION_CPU8_UP;
	case 9:
		return MT6797_A72_OPERATION_CPU9_UP;
	default:
		return MT6797_A72_OPERATION_NONE;
	}
}

static bool
mt6797_a72_up_token_equal(const struct arm64_late_cpu_up_token *left,
			  const struct arm64_late_cpu_up_token *right)
{
	return !memcmp(left, right, sizeof(*left));
}

static bool
mt6797_a72_identity_nonzero(const u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (identity[i])
			return true;

	return false;
}

static bool
mt6797_a72_provider_identity_valid(const struct mt6797_a72_provider_identity *identity)
{
	return identity && identity->generation && identity->cookie &&
		identity->generation != ~0ULL && identity->cookie != ~0ULL;
}

static int
mt6797_a72_ready_token_validate(unsigned int cpu,
				const struct arm64_late_cpu_ready_token *ready)
{
	unsigned int slot;

	if (!ready)
		return -EAGAIN;
	if (ready->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    strcmp(ready->profile_id, "mt6797-a53-a72-a41-v7") ||
	    !mt6797_a72_identity_nonzero(ready->plan_identity) ||
	    !mt6797_a72_identity_nonzero(ready->source_parent_identity) ||
	    !mt6797_a72_identity_nonzero(ready->config_input_identity) ||
	    !mt6797_a72_identity_nonzero(ready->evidence_identity))
		return -EPERM;

	if (!cpumask_test_cpu(8, &ready->target_cpus) ||
	    !cpumask_test_cpu(9, &ready->target_cpus) ||
	    ready->target_cpu[0] != 8 || ready->target_cpu[1] != 9 ||
	    ready->expected_target_mpidr[0] != 0x200 ||
	    ready->expected_target_mpidr[1] != 0x201)
		return -EPERM;

	/* Dormant targets have expectations, but no target-local observations. */
	if (ready->observed_target_mpidr[0] ||
	    ready->observed_target_mpidr[1])
		return -EPERM;

	slot = cpu == 8 ? 0 : 1;
	if (ready->target_cpu[slot] != cpu)
		return -EPERM;

	return 0;
}

static int
mt6797_a72_membership_p31_consume_attempt(unsigned int cpu,
						enum cpuhp_state target,
						u32 attempt,
						const struct mt6797_a72_entry_snapshot *entry)
{
	/* Caller retains a72_transition_lock through A28 and token minting. */
	enum mt6797_a72_membership_operation operation;
	unsigned long flags;
	u32 expected_attempt;
	int ret;

	if (!entry || target != CPUHP_ONLINE)
		return -EINVAL;

	operation = mt6797_a72_up_operation(cpu);
	switch (operation) {
	case MT6797_A72_OPERATION_CPU8_UP:
		expected_attempt = MT6797_A72_ATTEMPT_CPU8_UP;
		break;
	case MT6797_A72_OPERATION_CPU9_UP:
		expected_attempt = MT6797_A72_ATTEMPT_CPU9_UP;
		break;
	default:
		return -EINVAL;
	}

	if (attempt != expected_attempt ||
	    entry->observer_window != MT6797_A72_OBSERVER_WINDOW_OPEN)
		return -EPERM;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	switch (a72_owner.health) {
	case MT6797_A72_OWNER_CLOSED:
		ret = -EAGAIN;
		break;
	case MT6797_A72_OWNER_FAULTED:
		ret = -ESHUTDOWN;
		break;
	case MT6797_A72_OWNER_AVAILABLE:
		if (a72_owner.phase != MT6797_A72_PHASE_IDLE) {
			ret = -EBUSY;
			break;
		}
		if (!(a72_owner.attempts_available & expected_attempt) ||
		    (a72_owner.attempts_consumed & expected_attempt)) {
			ret = -EALREADY;
			break;
		}
		a72_owner.attempts_available &= ~expected_attempt;
		a72_owner.attempts_consumed |= expected_attempt;
		ret = 0;
		break;
	default:
		ret = -EPROTO;
		break;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return ret;
}

int
mt6797_a72_membership_validate_up_prestate(const struct mt6797_a72_transaction *transaction,
					   const struct mt6797_a72_a36_prestate *prestate)
{
	const struct mt6797_a72_transaction_identity *identity;
	u64 expected_mpidr;

	if (!transaction || !prestate)
		return -EAGAIN;
	identity = &transaction->identity;
	if (!transaction->valid || !transaction->p30_token_valid ||
	    identity->abi != MT6797_A72_TRANSACTION_ABI ||
	    identity->owner != ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP ||
	    identity->cpuhp_target != CPUHP_ONLINE ||
	    (identity->operation != ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
	     identity->operation != ARM64_LATE_CPU_STARTUP_OP_CPU9_UP))
		return -EPERM;

	expected_mpidr = identity->operation ==
		ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ? 0x200 : 0x201;
	if (prestate->abi != MT6797_A72_A36_PRESTATE_ABI ||
	    prestate->operation != identity->operation ||
	    prestate->observer_window != MT6797_A72_OBSERVER_WINDOW_OPEN ||
	    prestate->call_shape != MT6797_A72_A36_CALL_SHAPE_TWO_ARG ||
	    prestate->reserved ||
	    prestate->target_mpidr != expected_mpidr ||
	    prestate->secondary_entry_pa != __pa_symbol(secondary_entry) ||
	    identity->cpu_on_entry_pa != prestate->secondary_entry_pa ||
	    prestate->generation != identity->generation ||
	    prestate->cookie != identity->cookie ||
	    identity->target_mpidr != expected_mpidr ||
	    transaction->p30_token.operation != identity->operation ||
	    transaction->p30_token.target_cpu != identity->target_cpu ||
	    transaction->p30_token.target_mpidr != expected_mpidr)
		return -EPERM;

	if (identity->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP) {
		if (prestate->cpu8_online || prestate->cpu9_online ||
		    prestate->da921x_page ||
		    prestate->buckb_enabled ||
		    prestate->buckb_vsel != MT6797_A72_A36_BUCKB_VSEL ||
		    prestate->spm_218 != MT6797_A72_A36_SPM_218 ||
		    prestate->spm_290 != MT6797_A72_A36_SPM_290 ||
		    prestate->toprgu_pwrap_reset || prestate->mp2_dcm ||
		    prestate->secure_sentinels_stable ||
		    prestate->protected_clock_valid != 1 ||
		    prestate->pstore_console_available ||
		    prestate->watchdog_owned ||
		    prestate->cpu8_cluster_dcm_published ||
		    prestate->shared_write_set)
			return -EPERM;
	} else {
		if (prestate->cpu8_online != 1 || prestate->cpu9_online ||
		    prestate->da921x_page || prestate->buckb_enabled ||
		    prestate->buckb_vsel || prestate->spm_218 ||
		    prestate->spm_290 || prestate->toprgu_pwrap_reset ||
		    prestate->mp2_dcm || prestate->secure_sentinels_stable ||
		    prestate->protected_clock_valid ||
		    prestate->pstore_console_available ||
		    prestate->watchdog_owned ||
		    prestate->cpu8_cluster_dcm_published != 1 ||
		    prestate->shared_write_set)
			return -EPERM;
	}

	return 0;
}

static void
mt6797_a72_init_up_budgets(enum mt6797_a72_membership_operation operation,
			   struct mt6797_a72_call_budgets *budgets)
{
	memset(budgets, 0, sizeof(*budgets));
	if (operation == MT6797_A72_OPERATION_CPU8_UP) {
		budgets->preparation = MT6797_A72_BUDGET_AVAILABLE;
		budgets->provider_acquire = MT6797_A72_BUDGET_AVAILABLE;
		budgets->postprovider_preparation = MT6797_A72_BUDGET_AVAILABLE;
		if (IS_ENABLED(CONFIG_ARM64_MT6797_A72_PRE_P28_PROVIDER_ABORT))
			budgets->provider_abort = MT6797_A72_BUDGET_AVAILABLE;
	}
	budgets->cpu_on = MT6797_A72_BUDGET_AVAILABLE;
}

static int
mt6797_a72_membership_mint_up_token(unsigned int cpu,
				    enum cpuhp_state target, u32 attempt,
				    const struct mt6797_a72_entry_snapshot *entry,
				    const struct arm64_late_cpu_ready_token *ready,
				    struct mt6797_a72_transaction *transaction)
{
	enum mt6797_a72_membership_operation operation;
	unsigned long flags;
	u16 startup_operation;
	u64 target_mpidr;
	u64 generation;
	u64 cookie;
	int ret;

	ret = mt6797_a72_ready_token_validate(cpu, ready);
	if (ret)
		return ret;
	memset(&a72_mint_workspace, 0, sizeof(a72_mint_workspace));

	operation = mt6797_a72_up_operation(cpu);
	switch (operation) {
	case MT6797_A72_OPERATION_CPU8_UP:
		startup_operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
		target_mpidr = 0x200;
		break;
	case MT6797_A72_OPERATION_CPU9_UP:
		startup_operation = ARM64_LATE_CPU_STARTUP_OP_CPU9_UP;
		target_mpidr = 0x201;
		break;
	default:
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE) {
		ret = a72_owner.health == MT6797_A72_OWNER_CLOSED ?
			-EAGAIN : -ESHUTDOWN;
	} else if (a72_owner.phase != MT6797_A72_PHASE_IDLE ||
		   a72_owner.active.valid || !a72_owner.bootstrap_valid) {
		ret = -EBUSY;
	} else if (attempt != (operation == MT6797_A72_OPERATION_CPU8_UP ?
				MT6797_A72_ATTEMPT_CPU8_UP :
				MT6797_A72_ATTEMPT_CPU9_UP) ||
				 !(a72_owner.attempts_consumed & attempt)) {
		ret = -EPERM;
	} else if (operation == MT6797_A72_OPERATION_CPU8_UP &&
		   a72_owner.provider_state != MT6797_A72_PROVIDER_NONE) {
		ret = -EPERM;
	} else if (operation == MT6797_A72_OPERATION_CPU9_UP &&
		   (a72_owner.provider_state != MT6797_A72_PROVIDER_HELD ||
		    !mt6797_a72_provider_identity_valid(&a72_owner.provider_identity))) {
		ret = -EPERM;
	} else if (!a72_owner.next_generation || !a72_owner.next_cookie ||
		   a72_owner.next_generation == ~0ULL ||
		   a72_owner.next_cookie == ~0ULL) {
		ret = -EPROTO;
	} else {
		generation = a72_owner.next_generation++;
		cookie = a72_owner.next_cookie++;
		a72_mint_workspace.identity.abi = MT6797_A72_TRANSACTION_ABI;
		a72_mint_workspace.identity.owner = ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP;
		a72_mint_workspace.identity.operation = startup_operation;
		a72_mint_workspace.identity.target_cpu = cpu;
		a72_mint_workspace.identity.cpuhp_target = target;
		a72_mint_workspace.identity.target_mpidr = target_mpidr;
		a72_mint_workspace.identity.cpu_on_entry_pa = __pa_symbol(secondary_entry);
		a72_mint_workspace.identity.generation = generation;
		a72_mint_workspace.identity.cookie = cookie;
		mt6797_a72_init_up_budgets(operation, &a72_mint_workspace.budgets);
		a72_mint_workspace.entry = *entry;
		if (operation == MT6797_A72_OPERATION_CPU9_UP)
			a72_mint_workspace.provider_identity = a72_owner.provider_identity;
		a72_mint_workspace.p30_token.abi = ARM64_LATE_CPU_STARTUP_ABI;
		a72_mint_workspace.p30_token.owner = ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP;
		a72_mint_workspace.p30_token.operation = startup_operation;
		a72_mint_workspace.p30_token.target_cpu = cpu;
		a72_mint_workspace.p30_token.target_mpidr = target_mpidr;
		a72_mint_workspace.p30_token.generation = generation;
		a72_mint_workspace.p30_token.cookie = cookie;
		memcpy(a72_mint_workspace.p30_token.plan_identity, ready->plan_identity,
		       sizeof(a72_mint_workspace.p30_token.plan_identity));
		a72_mint_workspace.controller_cookie = cookie;
		a72_mint_workspace.mint_source = MT6797_A72_MINT_A34_BOOTSTRAP_OWNER;
		a72_mint_workspace.p30_token_valid = 1;
		a72_mint_workspace.valid = 1;
		a72_owner.active = a72_mint_workspace;
		a72_owner.controller = current;
		a72_owner.controller_cookie = cookie;
		a72_owner.phase = MT6797_A72_PHASE_FROZEN;
		*transaction = a72_mint_workspace;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	memset(&a72_mint_workspace, 0, sizeof(a72_mint_workspace));

	return ret;
}

static int
mt6797_a72_membership_bind_a36_prestate(const struct mt6797_a72_transaction_identity *identity,
					const struct mt6797_a72_a36_prestate *prestate,
					struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_FROZEN &&
	    a72_owner.active.valid &&
	    !memcmp(&a72_owner.active.identity, identity,
		    sizeof(a72_owner.active.identity))) {
		a72_owner.active.a36_prestate = *prestate;
		a72_owner.active.a36_valid = 1;
		*transaction = a72_owner.active;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return ret;
}

static int
mt6797_a72_membership_reject_frozen(const struct mt6797_a72_transaction_identity *identity)
{
	unsigned int slot;
	unsigned long flags;
	int ret = -EPERM;

	if (!identity || identity->operation <
			ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    identity->operation > ARM64_LATE_CPU_STARTUP_OP_CPU9_UP)
		return -EINVAL;
	slot = identity->operation - ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_FROZEN &&
	    a72_owner.active.valid &&
	    !memcmp(&a72_owner.active.identity, identity,
		    sizeof(a72_owner.active.identity))) {
		a72_owner.retired[slot] = a72_owner.active;
		a72_owner.retired_mask |= BIT(slot);
		memset(&a72_owner.active, 0, sizeof(a72_owner.active));
		a72_owner.controller = NULL;
		a72_owner.controller_cookie = 0;
		a72_owner.phase = MT6797_A72_PHASE_REJECTED;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return ret;
}

int mt6797_a72_membership_publish_up(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_FROZEN ||
	    !a72_owner.active.valid || !a72_owner.active.a36_valid ||
	    !a72_owner.active.p30_token_valid ||
	    a72_owner.active.p17_p18_published ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity))) {
		ret = -EBUSY;
		goto out_state;
	}

	switch (a72_owner.active.identity.operation) {
	case ARM64_LATE_CPU_STARTUP_OP_CPU8_UP:
		if (a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
		    a72_owner.active.provider_identity.generation ||
		    a72_owner.active.provider_identity.cookie)
			goto out_state;
		break;
	case ARM64_LATE_CPU_STARTUP_OP_CPU9_UP:
		if (a72_owner.provider_state != MT6797_A72_PROVIDER_HELD ||
		    !mt6797_a72_provider_identity_valid(&a72_owner.provider_identity) ||
		    memcmp(&a72_owner.active.provider_identity,
			   &a72_owner.provider_identity,
			   sizeof(a72_owner.provider_identity)))
			goto out_state;
		break;
	default:
		goto out_state;
	}

	a72_owner.active.p17_p18_published = 1;
	a72_owner.phase = MT6797_A72_PHASE_ON_ISSUED;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

#ifdef CONFIG_ARM64_MT6797_A72_P30E_WIRE
int mt6797_a72_membership_prepare_p30e_handoff(
	const struct mt6797_a72_transaction *transaction,
	const struct arm64_late_cpu_ready_token *ready,
	struct mt6797_a72_p30e_handoff *handoff)
{
	const struct mt6797_a72_transaction_identity *identity;
	phys_addr_t slot_pa;
	unsigned int cpu;
	unsigned long flags;
	u64 operation;
	int ret;

	if (!transaction || !ready || !handoff)
		return -EINVAL;

	memset(handoff, 0, sizeof(*handoff));
	identity = &transaction->identity;
	cpu = identity->target_cpu;
	ret = mt6797_a72_ready_token_validate(cpu, ready);
	if (ret)
		return ret;
	if (!transaction->valid || !transaction->a36_valid ||
	    !transaction->p30_token_valid ||
	    identity->cpu_on_entry_pa != __pa_symbol(secondary_entry) ||
	    transaction->a36_prestate.secondary_entry_pa !=
	    __pa_symbol(secondary_entry) ||
	    identity->abi != MT6797_A72_TRANSACTION_ABI ||
	    identity->owner != ARM64_LATE_CPU_STARTUP_OWNER_MEMBERSHIP ||
	    identity->cpuhp_target != CPUHP_ONLINE ||
	    (identity->operation != ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
	     identity->operation != ARM64_LATE_CPU_STARTUP_OP_CPU9_UP) ||
	    !identity->generation || !identity->cookie ||
	    identity->generation == ~0ULL || identity->cookie == ~0ULL ||
	    memcmp(transaction->p30_token.plan_identity,
		   ready->plan_identity,
		   sizeof(ready->plan_identity)) ||
	    transaction->p30_token.operation != identity->operation ||
	    transaction->p30_token.target_cpu != cpu ||
	    transaction->p30_token.target_mpidr != identity->target_mpidr)
		return -EPERM;

	switch (identity->operation) {
	case ARM64_LATE_CPU_STARTUP_OP_CPU8_UP:
		if (cpu != ARM64_MT6797_A72_P30E_CPU8 ||
		    identity->target_mpidr != ARM64_MT6797_A72_P30E_MPIDR_CPU8)
			return -EPERM;
		operation = ARM64_MT6797_A72_P30E_OPERATION_CPU8_UP;
		break;
	case ARM64_LATE_CPU_STARTUP_OP_CPU9_UP:
		if (cpu != ARM64_MT6797_A72_P30E_CPU9 ||
		    identity->target_mpidr != ARM64_MT6797_A72_P30E_MPIDR_CPU9)
			return -EPERM;
		operation = ARM64_MT6797_A72_P30E_OPERATION_CPU9_UP;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_FROZEN ||
	    !a72_owner.active.valid || !a72_owner.active.a36_valid ||
	    !a72_owner.active.p30_token_valid ||
	    memcmp(&a72_owner.active.identity, identity,
		   sizeof(a72_owner.active.identity)))
		ret = -EBUSY;
	else {
		slot_pa = arm64_mt6797_a72_p30e_slot_pa(cpu);
		if (!slot_pa ||
		    slot_pa & (ARM64_MT6797_A72_P30E_SLOT_BYTES - 1)) {
			ret = -EPROTO;
		} else {
			handoff->target_cpu = cpu;
			handoff->slot_pa = slot_pa;
			handoff->entry_pa = __pa_symbol(secondary_entry);
			handoff->operation = operation;
			memcpy(handoff->wire_boot_identity,
			       transaction->p30_token.plan_identity,
			       sizeof(handoff->wire_boot_identity));
			memcpy(handoff->target_boot_identity,
			       ready->plan_identity,
			       sizeof(handoff->target_boot_identity));
			handoff->generation = identity->generation;
			handoff->cookie = identity->cookie;
			ret = 0;
		}
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}
#endif
int mt6797_a72_membership_begin_p27_preparation(
	struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
	    a72_owner.active.budgets.preparation !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_NONE ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity))) {
		goto out_state;
	}

	/* Consume the one-shot budget before any real P27 effect is allowed. */
	a72_owner.active.budgets.preparation = MT6797_A72_BUDGET_CONSUMED;
	a72_owner.active.p27_preparation.abi =
		MT6797_A72_P27_PREPARATION_ABI;
	a72_owner.active.p27_preparation.operation =
		ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
	a72_owner.active.p27_preparation.stage =
		MT6797_A72_P27_STAGE_INFLIGHT;
	a72_owner.active.p27_preparation.generation =
		a72_owner.active.identity.generation;
	a72_owner.active.p27_preparation.cookie =
		a72_owner.active.identity.cookie;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_complete_p27_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p27_preparation *preparation)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !preparation)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_INFLIGHT ||
	    a72_owner.active.budgets.preparation !=
		    MT6797_A72_BUDGET_CONSUMED ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    preparation->abi != MT6797_A72_P27_PREPARATION_ABI ||
	    preparation->operation != ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    preparation->stage != MT6797_A72_P27_STAGE_COMPLETE ||
	    preparation->effect_mask != MT6797_A72_P27_EFFECT_MASK ||
	    preparation->generation != a72_owner.active.identity.generation ||
	    preparation->cookie != a72_owner.active.identity.cookie)
		goto out_state;

	a72_owner.active.p27_preparation = *preparation;
	a72_owner.active.p27_valid = 1;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static bool
mt6797_a72_provider_acquire_proof_valid(
	const struct mt6797_a72_provider_acquire_proof *proof,
	const struct mt6797_a72_transaction_identity *identity)
{
	if (!proof || !identity)
		return false;

	return proof->abi == MT6797_A72_PROVIDER_ACQUIRE_ABI &&
		proof->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
		proof->settle_us == MT6797_A72_PROVIDER_ACQUIRE_SETTLE_US &&
		proof->da921x_page == MT6797_A72_A36_DA921X_PAGE &&
		proof->buckb_enabled == 1 &&
		proof->buckb_vsel == MT6797_A72_A36_BUCKB_VSEL &&
		proof->origin == MT6797_A72_PROVIDER_ORIGIN_M01 &&
		!proof->reserved &&
		mt6797_a72_provider_identity_valid(&proof->held_identity) &&
		proof->transaction_generation == identity->generation &&
		proof->transaction_cookie == identity->cookie &&
		proof->origin_generation == proof->held_identity.generation;
}

int mt6797_a72_membership_begin_provider_acquire(
	struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
	    a72_owner.active.budgets.provider_acquire !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.provider_acquire_valid ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)))
		goto out_state;

	/* R01 consumes the acquire budget before a synchronous call boundary. */
	a72_owner.active.budgets.provider_acquire =
		MT6797_A72_BUDGET_CONSUMED;
	a72_owner.provider_state = MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_confirm_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_acquire_proof *proof)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !proof)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state !=
		    MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT ||
	    a72_owner.active.budgets.provider_acquire !=
		    MT6797_A72_BUDGET_CONSUMED ||
	    a72_owner.active.provider_acquire_valid ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    !mt6797_a72_provider_acquire_proof_valid(proof,
						      &a72_owner.active.identity))
		goto out_state;

	a72_owner.provider_identity = proof->held_identity;
	a72_owner.active.provider_identity = proof->held_identity;
	a72_owner.active.provider_acquire_proof = *proof;
	a72_owner.active.provider_acquire_valid = 1;
	a72_owner.provider_state = MT6797_A72_PROVIDER_HELD;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static bool
mt6797_a72_provider_rejection_valid(
	const struct mt6797_a72_provider_rejection *rejection,
	const struct mt6797_a72_transaction_identity *identity)
{
	if (!rejection || !identity)
		return false;

	return rejection->abi == MT6797_A72_PROVIDER_REJECT_ABI &&
		rejection->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
		rejection->result == MT6797_A72_PROVIDER_REJECTED_BEFORE_VOTE &&
		rejection->returned == 1 && !rejection->vote_requested &&
		!rejection->provider_mutated && !rejection->rail_mutated &&
		!rejection->reserved &&
		rejection->transaction_generation == identity->generation &&
		rejection->transaction_cookie == identity->cookie;
}

int mt6797_a72_membership_reject_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_provider_rejection *rejection)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !rejection)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_ACQUIRE_INFLIGHT ||
	    a72_owner.active.budgets.provider_acquire !=
		    MT6797_A72_BUDGET_CONSUMED ||
	    a72_owner.active.provider_acquire_valid ||
	    a72_owner.active.provider_rejection_valid ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    !mt6797_a72_provider_rejection_valid(rejection,
						      &a72_owner.active.identity))
		goto out_state;

	/* R03 proves a returned refusal before any provider or rail mutation. */
	memset(&a72_owner.provider_identity, 0,
	       sizeof(a72_owner.provider_identity));
	memset(&a72_owner.active.provider_identity, 0,
	       sizeof(a72_owner.active.provider_identity));
	a72_owner.active.provider_rejection = *rejection;
	a72_owner.active.provider_rejection_valid = 1;
	a72_owner.provider_state = MT6797_A72_PROVIDER_NONE;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_run_provider_acquire(
	struct mt6797_a72_transaction *transaction,
	struct mt6797_a72_provider_response *response)
{
	struct mt6797_a72_provider_request request = { };
	struct mt6797_a72_provider_acquire_proof proof = { };
	struct mt6797_a72_provider_rejection rejection = { };
	int ret;

	if (!transaction || !response)
		return -EINVAL;

	memset(response, 0, sizeof(*response));
	ret = mt6797_a72_membership_begin_provider_acquire(transaction);
	if (ret)
		return ret;

	request.abi = MT6797_A72_PROVIDER_CALL_ABI;
	request.operation = transaction->identity.operation;
	request.settle_us = MT6797_A72_PROVIDER_ACQUIRE_SETTLE_US;
	request.da921x_page = MT6797_A72_A36_DA921X_PAGE;
	request.buckb_vsel = MT6797_A72_A36_BUCKB_VSEL;
	request.transaction_generation = transaction->identity.generation;
	request.transaction_cookie = transaction->identity.cookie;
	ret = mt6797_a72_provider_acquire(&request, response);
	if (!ret) {
		proof.abi = MT6797_A72_PROVIDER_ACQUIRE_ABI;
		proof.operation = transaction->identity.operation;
		proof.settle_us = response->settle_us;
		proof.da921x_page = response->da921x_page;
		proof.buckb_enabled = response->buckb_enabled;
		proof.buckb_vsel = response->buckb_vsel;
		proof.origin = response->origin;
		proof.held_identity.generation = response->held_handle.generation;
		proof.held_identity.cookie = response->held_handle.cookie;
		proof.transaction_generation = transaction->identity.generation;
		proof.transaction_cookie = transaction->identity.cookie;
		proof.origin_generation = response->origin_generation;
		return mt6797_a72_membership_confirm_provider_acquire(transaction,
								       &proof);
	}

	/* The read-only provider may return a structured refusal before any vote. */
	if (ret != -EOPNOTSUPP ||
	    response->abi != MT6797_A72_PROVIDER_CALL_ABI ||
	    response->returned != 1 || response->vote_requested ||
	    response->provider_mutated || response->rail_mutated ||
	    response->reserved)
		return ret;

	rejection.abi = MT6797_A72_PROVIDER_REJECT_ABI;
	rejection.operation = transaction->identity.operation;
	rejection.result = MT6797_A72_PROVIDER_REJECTED_BEFORE_VOTE;
	rejection.returned = response->returned;
	rejection.vote_requested = response->vote_requested;
	rejection.provider_mutated = response->provider_mutated;
	rejection.rail_mutated = response->rail_mutated;
	rejection.transaction_generation = transaction->identity.generation;
	rejection.transaction_cookie = transaction->identity.cookie;
	ret = mt6797_a72_membership_reject_provider_acquire(
		transaction, &rejection);
	if (!ret)
		return -EOPNOTSUPP;
	return ret;
}

static bool
mt6797_a72_provider_abort_proof_valid(const struct mt6797_a72_provider_abort_proof *proof,
				      const struct mt6797_a72_transaction *transaction)
{
	if (!proof || !transaction)
		return false;

	return proof->abi == MT6797_A72_PROVIDER_ABORT_ABI &&
		proof->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
		proof->result == MT6797_A72_PROVIDER_ABORT_EXACT_RELEASE &&
		proof->returned == 1 && proof->vote_requested == 1 &&
		proof->provider_mutated == 1 && proof->rail_mutated == 1 &&
		proof->settle_us == MT6797_A72_PROVIDER_ACQUIRE_SETTLE_US &&
		proof->da921x_page == MT6797_A72_A36_DA921X_PAGE &&
		!proof->buckb_enabled &&
		proof->buckb_vsel == MT6797_A72_A36_BUCKB_VSEL &&
		proof->origin == MT6797_A72_PROVIDER_ORIGIN_M01 &&
		!proof->reserved &&
		mt6797_a72_provider_identity_valid(&proof->released_identity) &&
		!memcmp(&proof->released_identity,
			&transaction->provider_acquire_proof.held_identity,
			sizeof(proof->released_identity)) &&
		proof->transaction_generation ==
			transaction->identity.generation &&
		proof->transaction_cookie == transaction->identity.cookie &&
		proof->origin_generation == proof->released_identity.generation;
}

int
mt6797_a72_membership_begin_provider_abort(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_ARM64_MT6797_A72_PRE_P28_PROVIDER_ABORT))
		return -EOPNOTSUPP;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_HELD ||
	    !a72_owner.active.provider_acquire_valid ||
	    !mt6797_a72_provider_identity_valid(&a72_owner.provider_identity) ||
	    a72_owner.active.provider_abort_valid ||
	    a72_owner.active.budgets.provider_abort !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.p28_preparation.stage !=
		    MT6797_A72_P28_STAGE_NONE ||
	    a72_owner.active.p28_valid ||
	    a72_owner.active.budgets.postprovider_preparation !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.budgets.cpu_on != MT6797_A72_BUDGET_AVAILABLE ||
	    memcmp(&a72_owner.active.provider_identity,
		   &a72_owner.provider_identity,
		   sizeof(a72_owner.provider_identity)) ||
	    memcmp(&transaction->provider_identity,
		   &a72_owner.provider_identity,
		   sizeof(a72_owner.provider_identity)) ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)))
		goto out_state;

	a72_owner.active.budgets.provider_abort =
		MT6797_A72_BUDGET_CONSUMED;
	a72_owner.provider_state = MT6797_A72_PROVIDER_RELEASE_INFLIGHT;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int
mt6797_a72_membership_confirm_abort(struct mt6797_a72_transaction *transaction,
				    const struct mt6797_a72_provider_abort_proof *proof)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !proof)
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_ARM64_MT6797_A72_PRE_P28_PROVIDER_ABORT))
		return -EOPNOTSUPP;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_RELEASE_INFLIGHT ||
	    a72_owner.active.budgets.provider_abort !=
		    MT6797_A72_BUDGET_CONSUMED ||
	    a72_owner.active.provider_abort_valid ||
	    a72_owner.active.p28_preparation.stage !=
		    MT6797_A72_P28_STAGE_NONE ||
	    a72_owner.active.p28_valid ||
	    a72_owner.active.budgets.postprovider_preparation !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.budgets.cpu_on != MT6797_A72_BUDGET_AVAILABLE ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    !mt6797_a72_provider_abort_proof_valid(proof, &a72_owner.active))
		goto out_state;

	a72_owner.active.provider_abort_proof = *proof;
	a72_owner.active.provider_abort_valid = 1;
	memset(&a72_owner.provider_identity, 0,
	       sizeof(a72_owner.provider_identity));
	memset(&a72_owner.active.provider_identity, 0,
	       sizeof(a72_owner.active.provider_identity));
	a72_owner.provider_state = MT6797_A72_PROVIDER_NONE;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static void mt6797_a72_membership_latch_provider_fault(
	const struct mt6797_a72_transaction *transaction,
	enum mt6797_a72_provider_state expected_state,
	enum mt6797_a72_owner_fault cause, int error)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.provider_state == expected_state) {
		a72_owner.provider_state = MT6797_A72_PROVIDER_FAULT_UNKNOWN;
		a72_owner.health = MT6797_A72_OWNER_FAULTED;
		a72_owner.phase = MT6797_A72_PHASE_FAULT;
		if (!a72_owner.first_fault.valid && transaction) {
			a72_owner.first_fault.identity = transaction->identity;
			a72_owner.first_fault.detail = (u32)(-error);
			a72_owner.first_fault.cause = cause;
			a72_owner.first_fault.valid = 1;
		}
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
}

int
mt6797_a72_membership_run_provider_abort(struct mt6797_a72_transaction *transaction,
					 struct mt6797_a72_provider_response *response)
{
	struct mt6797_a72_provider_abort_proof proof = { };
	struct mt6797_a72_provider_handle handle;
	enum mt6797_a72_provider_state expected_state =
		MT6797_A72_PROVIDER_RELEASE_INFLIGHT;
	enum mt6797_a72_owner_fault fault_cause =
		MT6797_A72_FAULT_PROVIDER_RELEASE_RETURN;
	int ret;

	if (!transaction || !response)
		return -EINVAL;
	memset(response, 0, sizeof(*response));
	ret = mt6797_a72_membership_begin_provider_abort(transaction);
	if (ret)
		return ret;

	handle.generation = transaction->provider_identity.generation;
	handle.cookie = transaction->provider_identity.cookie;
	ret = mt6797_a72_provider_release(&handle, response);
	if (ret)
		goto out_fault;

	proof.abi = MT6797_A72_PROVIDER_ABORT_ABI;
	proof.operation = transaction->identity.operation;
	proof.result = MT6797_A72_PROVIDER_ABORT_EXACT_RELEASE;
	proof.returned = response->returned;
	proof.vote_requested = response->vote_requested;
	proof.provider_mutated = response->provider_mutated;
	proof.rail_mutated = response->rail_mutated;
	proof.settle_us = response->settle_us;
	proof.da921x_page = response->da921x_page;
	proof.buckb_enabled = response->buckb_enabled;
	proof.buckb_vsel = response->buckb_vsel;
	proof.origin = response->origin;
	proof.reserved = response->reserved;
	proof.released_identity.generation = response->held_handle.generation;
	proof.released_identity.cookie = response->held_handle.cookie;
	proof.transaction_generation = transaction->identity.generation;
	proof.transaction_cookie = transaction->identity.cookie;
	proof.origin_generation = response->origin_generation;
	ret = mt6797_a72_membership_confirm_abort(transaction, &proof);
	if (!ret)
		return 0;

out_fault:
	if (!ret)
		ret = -EPROTO;
	mt6797_a72_membership_latch_provider_fault(transaction,
						   expected_state,
						   fault_cause,
						   ret);
	return ret;
}

static bool
mt6797_a72_p28_preparation_valid(
	const struct mt6797_a72_p28_preparation *preparation,
	const struct mt6797_a72_transaction *transaction)
{
	if (!preparation || !transaction)
		return false;

	return preparation->abi == MT6797_A72_P28_PREPARATION_ABI &&
		preparation->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
		preparation->stage == MT6797_A72_P28_STAGE_COMPLETE &&
		preparation->effect_mask == MT6797_A72_P28_EFFECT_MASK &&
		preparation->isolation_from == MT6797_A72_P28_ISOLATION_FROM &&
		preparation->isolation_to == MT6797_A72_P28_ISOLATION_TO &&
		preparation->pwrap_deasserted == 1 &&
		preparation->software_guard_released == 1 &&
		preparation->wait_before_sram_us == MT6797_A72_P28_WAIT_US &&
		preparation->sram_ldo_mv == MT6797_A72_P28_SRAM_LDO_MV &&
		preparation->wait_after_sram_us == MT6797_A72_P28_WAIT_US &&
		preparation->selector == MT6797_A72_P28_SELECTOR &&
		preparation->calibration_stable == 1 &&
		preparation->calibration_valid == 1 &&
		!preparation->reserved &&
		!memcmp(&preparation->provider_identity,
			&transaction->provider_identity,
			sizeof(preparation->provider_identity)) &&
		preparation->transaction_generation ==
			transaction->identity.generation &&
		preparation->transaction_cookie == transaction->identity.cookie;
}

int mt6797_a72_membership_begin_p28_preparation(
	struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_HELD ||
	    !a72_owner.active.provider_acquire_valid ||
	    !mt6797_a72_provider_identity_valid(&a72_owner.provider_identity) ||
	    a72_owner.active.p28_preparation.stage !=
		    MT6797_A72_P28_STAGE_NONE ||
	    a72_owner.active.p28_valid ||
	    a72_owner.active.budgets.postprovider_preparation !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    memcmp(&a72_owner.active.provider_identity,
		   &a72_owner.provider_identity,
		   sizeof(a72_owner.provider_identity)) ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)))
		goto out_state;

	/* Consume P28 before its future isolation/SRAM effect sequence. */
	a72_owner.active.budgets.postprovider_preparation =
		MT6797_A72_BUDGET_CONSUMED;
	a72_owner.active.p28_preparation.abi =
		MT6797_A72_P28_PREPARATION_ABI;
	a72_owner.active.p28_preparation.operation =
		ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
	a72_owner.active.p28_preparation.stage =
		MT6797_A72_P28_STAGE_INFLIGHT;
	a72_owner.active.p28_preparation.provider_identity =
		a72_owner.active.provider_identity;
	a72_owner.active.p28_preparation.transaction_generation =
		a72_owner.active.identity.generation;
	a72_owner.active.p28_preparation.transaction_cookie =
		a72_owner.active.identity.cookie;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_complete_p28_preparation(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p28_preparation *preparation)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !preparation)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_HELD ||
	    !a72_owner.active.provider_acquire_valid ||
	    a72_owner.active.p28_preparation.stage !=
		    MT6797_A72_P28_STAGE_INFLIGHT ||
	    a72_owner.active.p28_valid ||
	    a72_owner.active.budgets.postprovider_preparation !=
		    MT6797_A72_BUDGET_CONSUMED ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    !mt6797_a72_p28_preparation_valid(preparation,
					   &a72_owner.active))
		goto out_state;

	a72_owner.active.p28_preparation = *preparation;
	a72_owner.active.p28_valid = 1;
	*transaction = a72_owner.active;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static bool
mt6797_a72_p29_provider_predecessor_valid(const struct mt6797_a72_transaction *transaction)
{
	if (!transaction ||
	    transaction->provider_rejection_valid ==
		    transaction->provider_abort_valid)
		return false;

	if (transaction->provider_rejection_valid)
		return !transaction->provider_acquire_valid &&
			(!IS_ENABLED(CONFIG_ARM64_MT6797_A72_PRE_P28_PROVIDER_ABORT) ||
			 transaction->budgets.provider_abort ==
				MT6797_A72_BUDGET_AVAILABLE);

	return transaction->provider_acquire_valid &&
		transaction->provider_abort_valid &&
		transaction->budgets.provider_abort ==
			MT6797_A72_BUDGET_CONSUMED &&
		mt6797_a72_provider_abort_proof_valid(&transaction->provider_abort_proof,
						      transaction);
}

static bool
mt6797_a72_p29_rollback_valid(
	const struct mt6797_a72_p29_rollback_proof *rollback,
	const struct mt6797_a72_transaction_identity *identity)
{
	if (!rollback || !identity)
		return false;

	return rollback->abi == MT6797_A72_P29_ROLLBACK_ABI &&
		rollback->operation == ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
		rollback->restored_effect_mask == MT6797_A72_P27_EFFECT_MASK &&
		rollback->residual_effect_mask == 0 &&
		!rollback->p28_started && !rollback->cpu_on_issued &&
		!rollback->reserved &&
		rollback->transaction_generation == identity->generation &&
		rollback->transaction_cookie == identity->cookie;
}

int mt6797_a72_membership_complete_p29_rollback(
	struct mt6797_a72_transaction *transaction,
	const struct mt6797_a72_p29_rollback_proof *rollback)
{
	unsigned int slot;
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction || !rollback)
		return -EINVAL;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE ||
	    a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
	    !a72_owner.active.valid || !a72_owner.active.p17_p18_published ||
	    !a72_owner.active.p27_valid ||
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE ||
	    a72_owner.active.identity.operation !=
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
	    a72_owner.members ||
	    a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
	    a72_owner.provider_identity.generation ||
	    a72_owner.provider_identity.cookie ||
	    a72_owner.active.provider_identity.generation ||
	    a72_owner.active.provider_identity.cookie ||
	    !mt6797_a72_p29_provider_predecessor_valid(&a72_owner.active) ||
	    a72_owner.active.p28_preparation.stage !=
		    MT6797_A72_P28_STAGE_NONE ||
	    a72_owner.active.p28_valid ||
	    a72_owner.active.budgets.postprovider_preparation !=
		    MT6797_A72_BUDGET_AVAILABLE ||
	    a72_owner.active.p29_valid ||
	    a72_owner.active.budgets.cpu_on != MT6797_A72_BUDGET_AVAILABLE ||
	    memcmp(&a72_owner.active.identity, &transaction->identity,
		   sizeof(a72_owner.active.identity)) ||
	    !mt6797_a72_p29_rollback_valid(rollback,
					 &a72_owner.active.identity))
		goto out_state;

	/* P29 records exact P27 restoration, then P21 retires the generation. */
	a72_owner.active.p29_rollback = *rollback;
	a72_owner.active.p29_valid = 1;
	slot = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP -
		ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
	a72_owner.retired[slot] = a72_owner.active;
	a72_owner.retired_mask |= BIT(slot);
	*transaction = a72_owner.retired[slot];
	memset(&a72_owner.active, 0, sizeof(a72_owner.active));
	a72_owner.controller = NULL;
	a72_owner.controller_cookie = 0;
	a72_owner.phase = MT6797_A72_PHASE_REJECTED;
	ret = 0;

out_state:
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static int
mt6797_a72_membership_check_up(unsigned int cpu, int tasks_frozen,
			       enum cpuhp_state target, bool preflight)
{
	struct mt6797_a72_transaction *transaction;
	unsigned long flags;
	int ret = -EPERM;

	if (cpu != 8 || target != CPUHP_ONLINE)
		return -EINVAL;
	if (tasks_frozen)
		return -EPERM;
	if (!IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER))
		return -EOPNOTSUPP;

	/* This leaf-only gate must remain safe under cpu_add_remove_lock. */
	if (READ_ONCE(a72_owner.health) == MT6797_A72_OWNER_CLOSED)
		return -EAGAIN;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	transaction = &a72_owner.active;
	if (a72_owner.health == MT6797_A72_OWNER_CLOSED) {
		ret = -EAGAIN;
	} else if (a72_owner.health == MT6797_A72_OWNER_FAULTED) {
		ret = -ESHUTDOWN;
	} else if (a72_owner.health != MT6797_A72_OWNER_AVAILABLE) {
		ret = -EPROTO;
	} else if (a72_owner.phase != MT6797_A72_PHASE_ON_ISSUED ||
		   !transaction->valid || !transaction->p17_p18_published ||
		   !transaction->p30_token_valid ||
		   transaction->identity.operation !=
			   ARM64_LATE_CPU_STARTUP_OP_CPU8_UP ||
		   transaction->identity.target_cpu != 8 ||
		   transaction->identity.cpuhp_target != CPUHP_ONLINE ||
		   transaction->controller_cookie !=
			   transaction->identity.cookie ||
		   a72_owner.controller != current ||
		   a72_owner.controller_cookie !=
			   transaction->identity.cookie ||
		   a72_owner.members ||
		   a72_owner.provider_state != MT6797_A72_PROVIDER_NONE ||
		   cpu_online(8) || cpu_online(9)) {
		ret = -EPERM;
	} else if (preflight) {
		if (transaction->public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_CLAIMED) {
			ret = -EALREADY;
		} else {
			transaction->public_preflight =
				MT6797_A72_PUBLIC_ADMISSION_PREFLIGHT;
			ret = 0;
		}
	} else if (transaction->public_preflight ==
		   MT6797_A72_PUBLIC_ADMISSION_PREFLIGHT) {
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return ret;
}

int mt6797_a72_membership_preflight_up(unsigned int cpu,
				       enum cpuhp_state target)
{
	return mt6797_a72_membership_check_up(cpu, 0, target, true);
}

int mt6797_a72_membership_validate_up(unsigned int cpu, int tasks_frozen,
				      enum cpuhp_state target)
{
	return mt6797_a72_membership_check_up(cpu, tasks_frozen, target, false);
}

int mt6797_a72_membership_claim_cpu8(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	if (!IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER))
		return -EOPNOTSUPP;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_ON_ISSUED &&
	    a72_owner.active.valid && a72_owner.active.p17_p18_published &&
	    a72_owner.active.p30_token_valid &&
	    a72_owner.active.public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_PREFLIGHT &&
	    a72_owner.active.identity.operation ==
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
	    a72_owner.active.identity.target_cpu == 8 &&
	    a72_owner.active.identity.cpuhp_target == CPUHP_ONLINE &&
	    a72_owner.controller == current &&
	    a72_owner.controller_cookie == a72_owner.active.identity.cookie &&
	    !a72_owner.members &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_NONE &&
	    !cpu_online(8) && !cpu_online(9)) {
		a72_owner.active.public_preflight =
			MT6797_A72_PUBLIC_ADMISSION_CLAIMED;
		*transaction = a72_owner.active;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

int mt6797_a72_membership_reject_cpu8(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_ON_ISSUED &&
	    a72_owner.active.valid &&
	    a72_owner.active.public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_CLAIMED &&
	    a72_owner.active.p27_preparation.stage !=
		    MT6797_A72_P27_STAGE_COMPLETE &&
	    !a72_owner.active.p27_valid &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_NONE &&
	    a72_owner.active.p28_preparation.stage ==
		    MT6797_A72_P28_STAGE_NONE &&
	    !a72_owner.active.p28_valid &&
	    a72_owner.active.budgets.cpu_on ==
		    MT6797_A72_BUDGET_AVAILABLE &&
	    !memcmp(&a72_owner.active.identity, &transaction->identity,
		    sizeof(a72_owner.active.identity))) {
		a72_owner.retired[0] = a72_owner.active;
		a72_owner.retired_mask |= BIT(0);
		*transaction = a72_owner.retired[0];
		memset(&a72_owner.active, 0, sizeof(a72_owner.active));
		a72_owner.controller = NULL;
		a72_owner.controller_cookie = 0;
		a72_owner.phase = MT6797_A72_PHASE_REJECTED;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_begin_cpu8_on(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_ON_ISSUED &&
	    a72_owner.active.valid &&
	    a72_owner.active.public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_CLAIMED &&
	    a72_owner.active.p27_valid && a72_owner.active.p28_valid &&
	    a72_owner.active.p28_preparation.stage ==
		    MT6797_A72_P28_STAGE_COMPLETE &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_HELD &&
	    a72_owner.active.provider_acquire_valid &&
	    a72_owner.active.budgets.cpu_on ==
		    MT6797_A72_BUDGET_AVAILABLE &&
	    !memcmp(&a72_owner.active.identity, &transaction->identity,
		    sizeof(a72_owner.active.identity)) &&
	    !cpu_online(8) && !cpu_online(9)) {
		a72_owner.active.budgets.cpu_on = MT6797_A72_BUDGET_CONSUMED;
		*transaction = a72_owner.active;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

static int mt6797_a72_publish_cpu8_success_state(struct mt6797_a72_transaction *transaction,
						 bool cpu8_online,
						 bool cpu9_online)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_ON_ISSUED &&
	    a72_owner.active.valid &&
	    a72_owner.active.public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_CLAIMED &&
	    a72_owner.active.p27_valid && a72_owner.active.p28_valid &&
	    a72_owner.active.p28_preparation.stage ==
		    MT6797_A72_P28_STAGE_COMPLETE &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_HELD &&
	    a72_owner.active.provider_acquire_valid &&
	    a72_owner.active.budgets.cpu_on ==
		    MT6797_A72_BUDGET_CONSUMED &&
	    !a72_owner.active.cpu8_success_published &&
	    !a72_owner.members && cpu8_online && !cpu9_online &&
	    !memcmp(&a72_owner.active.identity, &transaction->identity,
		    sizeof(a72_owner.active.identity))) {
		a72_owner.members = BIT(0);
		a72_owner.active.cpu8_success_published = 1;
		a72_owner.phase = MT6797_A72_PHASE_VERIFYING;
		*transaction = a72_owner.active;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_publish_cpu8_success(struct mt6797_a72_transaction *transaction)
{
	return mt6797_a72_publish_cpu8_success_state(transaction,
						     cpu_online(8), cpu_online(9));
}

static int mt6797_a72_finalize_cpu8_success_state(struct mt6797_a72_transaction *transaction,
						  bool cpu8_online,
						  bool cpu9_online)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_VERIFYING &&
	    a72_owner.active.valid &&
	    a72_owner.active.cpu8_success_published &&
	    a72_owner.members == BIT(0) &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_HELD &&
	    cpu8_online && !cpu9_online &&
	    !memcmp(&a72_owner.active.identity, &transaction->identity,
		    sizeof(a72_owner.active.identity))) {
		a72_owner.retired[0] = a72_owner.active;
		a72_owner.retired_mask |= BIT(0);
		*transaction = a72_owner.retired[0];
		memset(&a72_owner.active, 0, sizeof(a72_owner.active));
		a72_owner.controller = NULL;
		a72_owner.controller_cookie = 0;
		a72_owner.phase = MT6797_A72_PHASE_IDLE;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
	return ret;
}

int mt6797_a72_membership_finalize_cpu8_success(struct mt6797_a72_transaction *transaction)
{
	return mt6797_a72_finalize_cpu8_success_state(transaction,
						      cpu_online(8), cpu_online(9));
}

int mt6797_a72_membership_validate_entry(unsigned int cpu,
					 enum cpuhp_state target, u32 attempt,
					 const struct mt6797_a72_entry_snapshot *entry)
{
	enum mt6797_a72_membership_operation operation;
	u32 expected_attempt;
	u32 expected_members;
	u32 expected_provider;
	u32 expected_online;
	u32 expected_cpu8_state;

	if (!entry || target != CPUHP_ONLINE)
		return -EINVAL;
	/* P31 owns attempt consumption; this A28 check is read-only. */

	operation = mt6797_a72_up_operation(cpu);
	switch (operation) {
	case MT6797_A72_OPERATION_CPU8_UP:
		expected_attempt = MT6797_A72_ATTEMPT_CPU8_UP;
		expected_members = 0;
		expected_provider = MT6797_A72_PROVIDER_NONE;
		expected_online = 0;
		expected_cpu8_state = CPUHP_OFFLINE;
		break;
	case MT6797_A72_OPERATION_CPU9_UP:
		expected_attempt = MT6797_A72_ATTEMPT_CPU9_UP;
		expected_members = BIT(0);
		expected_provider = MT6797_A72_PROVIDER_HELD;
		expected_online = BIT(0);
		expected_cpu8_state = CPUHP_ONLINE;
		break;
	default:
		return -EINVAL;
	}

	if (attempt != expected_attempt ||
	    entry->flags != MT6797_A72_ENTRY_FLAGS_MASK ||
	    entry->members != expected_members ||
	    entry->provider_state != expected_provider ||
	    entry->online_mask != expected_online ||
	    entry->cpuhp_state_cpu8 != expected_cpu8_state ||
	    entry->cpuhp_state_cpu9 != CPUHP_OFFLINE ||
	    entry->cpu8_mpidr != 0x200 || entry->cpu9_mpidr != 0x201)
		return -EPERM;

	return 0;
}

#if IS_ENABLED(CONFIG_ARM64_MT6797_A72_DERIVED_ADMISSION)

struct mt6797_a72_derived_workspace {
	struct mt6797_a72_direct_state_snapshot direct;
	struct mt6797_a72_a34_replay replay;
	struct mt6797_a72_entry_snapshot entry;
	struct mt6797_a72_a36_prestate prestate;
};

static struct mt6797_a72_derived_workspace a72_derived_workspace;

static void
mt6797_a72_derive_cpu8_entry(const struct mt6797_a72_direct_state_snapshot *direct,
			     struct mt6797_a72_entry_snapshot *entry)
{
	entry->members = direct->owner.members;
	entry->provider_state = direct->owner.provider_state;
	entry->online_mask = (direct->cpu8_online ? BIT(0) : 0) |
		(direct->cpu9_online ? BIT(1) : 0);
	entry->cpuhp_state_cpu8 = direct->cpu8_online ?
		CPUHP_ONLINE : CPUHP_OFFLINE;
	entry->cpuhp_state_cpu9 = direct->cpu9_online ?
		CPUHP_ONLINE : CPUHP_OFFLINE;
	entry->observer_window = MT6797_A72_OBSERVER_WINDOW_OPEN;
	entry->flags = (direct->cpu8_present ? MT6797_A72_ENTRY_CPU8_PRESENT : 0) |
		(direct->cpu9_present ? MT6797_A72_ENTRY_CPU9_PRESENT : 0) |
		(direct->cpu8_possible ? MT6797_A72_ENTRY_CPU8_POSSIBLE : 0) |
		(direct->cpu9_possible ? MT6797_A72_ENTRY_CPU9_POSSIBLE : 0);
	entry->cpu8_mpidr = direct->cpu8_mpidr;
	entry->cpu9_mpidr = direct->cpu9_mpidr;
}

static void
mt6797_a72_derive_cpu8_prestate(const struct mt6797_a72_direct_state_snapshot *direct,
				const struct mt6797_a72_transaction *transaction,
				struct mt6797_a72_a36_prestate *prestate)
{
	prestate->abi = MT6797_A72_A36_PRESTATE_ABI;
	prestate->operation = ARM64_LATE_CPU_STARTUP_OP_CPU8_UP;
	prestate->observer_window = MT6797_A72_OBSERVER_WINDOW_OPEN;
	prestate->call_shape = MT6797_A72_A36_CALL_SHAPE_TWO_ARG;
	prestate->cpu8_online = direct->cpu8_online;
	prestate->cpu9_online = direct->cpu9_online;
	prestate->buckb_enabled = direct->source.provider.buckb_cont;
	prestate->buckb_vsel = direct->source.provider.vbuckb_b;
	prestate->spm_218 = direct->source.platform.spm_mp2_cpusys_pwr_con;
	prestate->spm_290 = direct->source.platform.spm_cpu_ext_buck_iso;
	prestate->toprgu_pwrap_reset =
		direct->source.platform.pwrap_reset_asserted;
	prestate->mp2_dcm = direct->source.platform.mp2_sync_dcm;
	prestate->protected_clock_valid =
		direct->source.clock.abi == MT6797_DVFSP_CLOCK_BACKEND_ABI &&
		direct->source.clock.sample_generation != 0;
	prestate->target_mpidr = direct->cpu8_mpidr;
	prestate->secondary_entry_pa = transaction->identity.cpu_on_entry_pa;
	prestate->generation = transaction->identity.generation;
	prestate->cookie = transaction->identity.cookie;
}

static int
mt6797_a72_membership_derive_cpu8_locked(const struct mt6797_a72_direct_topology *topology,
					 const struct arm64_late_cpu_ready_token *ready,
					 struct mt6797_a72_transaction *transaction,
					 u32 *derive_stage)
{
	struct mt6797_a72_derived_workspace *workspace = &a72_derived_workspace;
	int ret;

	memset(workspace, 0, sizeof(*workspace));
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_READY_TOKEN);
	ret = mt6797_a72_ready_token_validate(8, ready);
	if (ret)
		goto out_clear;
	workspace->replay.abi = MT6797_A72_A34_REPLAY_ABI;
	workspace->replay.valid = 1;
	workspace->replay.proof =
		MT6797_A72_A34_REPLAY_APPLICABLE_PRIMARY_BL31_CLEAR;
	ret = mt6797_a72_membership_publish_bootstrap_locked(topology,
							     &workspace->replay, false,
							     &workspace->direct,
							     derive_stage);
	if (ret)
		goto out_clear;

	mt6797_a72_derive_cpu8_entry(&workspace->direct, &workspace->entry);
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_P31_CONSUME);
	ret = mt6797_a72_membership_p31_consume_attempt(8, CPUHP_ONLINE,
							MT6797_A72_ATTEMPT_CPU8_UP,
							&workspace->entry);
	if (ret)
		goto out_clear;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_ENTRY_VALIDATE);
	ret = mt6797_a72_membership_validate_entry(8, CPUHP_ONLINE,
						   MT6797_A72_ATTEMPT_CPU8_UP,
						   &workspace->entry);
	if (ret)
		goto out_clear;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_TOKEN_MINT);
	ret = mt6797_a72_membership_mint_up_token(8, CPUHP_ONLINE,
						  MT6797_A72_ATTEMPT_CPU8_UP,
						  &workspace->entry, ready, transaction);
	if (ret)
		goto out_clear;

	mt6797_a72_derive_cpu8_prestate(&workspace->direct, transaction,
					&workspace->prestate);
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_PRESTATE_VALIDATE);
	ret = mt6797_a72_membership_validate_up_prestate(transaction,
							 &workspace->prestate);
	if (ret)
		goto reject_frozen;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_PRESTATE_BIND);
	ret = mt6797_a72_membership_bind_a36_prestate(&transaction->identity,
						      &workspace->prestate, transaction);
	if (ret)
		goto reject_frozen;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_COMPLETE);
	goto out_clear;

reject_frozen:
	mt6797_a72_membership_reject_frozen(&transaction->identity);
	memset(transaction, 0, sizeof(*transaction));
out_clear:
	memset(workspace, 0, sizeof(*workspace));
	return ret;
}

static void
mt6797_a72_membership_fill_cpu8_topology(struct mt6797_a72_direct_topology *topology)
{
	topology->cpu8_possible = cpu_possible(8);
	topology->cpu9_possible = cpu_possible(9);
	topology->cpu8_present = cpu_present(8);
	topology->cpu9_present = cpu_present(9);
	topology->cpu8_online = cpu_online(8);
	topology->cpu9_online = cpu_online(9);
	topology->cpu8_method_valid = get_cpu_ops(8) == &mt6797_psci_ops;
	topology->cpu9_method_valid = get_cpu_ops(9) == &mt6797_psci_ops;
	topology->cpu8_mpidr = cpu_logical_map(8);
	topology->cpu9_mpidr = cpu_logical_map(9);
}

int
mt6797_a72_membership_derive_cpu8_diagnostic(const struct arm64_late_cpu_ready_token *ready, struct mt6797_a72_transaction *transaction, u32 *derive_stage)
{
	struct mt6797_a72_direct_topology topology = {};
	int ret;

	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_NONE);
	if (!transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	cpus_read_lock();
	mutex_lock(&a72_transition_lock);
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_TOPOLOGY);
	if (nr_cpu_ids <= 9) {
		ret = -ENODEV;
		goto out_unlock;
	}
	mt6797_a72_membership_fill_cpu8_topology(&topology);
	ret = mt6797_a72_membership_derive_cpu8_locked(&topology, ready,
						       transaction, derive_stage);
out_unlock:
	mutex_unlock(&a72_transition_lock);
	cpus_read_unlock();
	return ret;
}

/* CPU9 inherits the live A72 cluster; it must not replay CPU8 power work. */
int
mt6797_a72_membership_derive_cpu9_diagnostic(
	const struct arm64_late_cpu_ready_token *ready,
	struct mt6797_a72_transaction *transaction, u32 *derive_stage)
{
	struct mt6797_a72_direct_state_snapshot direct = { };
	struct mt6797_a72_entry_snapshot entry = { };
	struct mt6797_a72_a36_prestate prestate = { };
	int ret;

	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_NONE);
	if (!transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	if (nr_cpu_ids <= 9)
		return -ENODEV;

	cpus_read_lock();
	ret = mt6797_a72_direct_state_snapshot(&direct);
	if (ret)
		goto out_unlock;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_READY_TOKEN);
	ret = mt6797_a72_ready_token_validate(9, ready);
	if (ret)
		goto out_unlock;
	mt6797_a72_derive_cpu8_entry(&direct, &entry);
	prestate.abi = MT6797_A72_A36_PRESTATE_ABI;
	prestate.operation = ARM64_LATE_CPU_STARTUP_OP_CPU9_UP;
	prestate.observer_window = MT6797_A72_OBSERVER_WINDOW_OPEN;
	prestate.call_shape = MT6797_A72_A36_CALL_SHAPE_TWO_ARG;
	prestate.cpu8_online = direct.cpu8_online;
	prestate.cpu9_online = direct.cpu9_online;
	prestate.target_mpidr = direct.cpu9_mpidr;
	prestate.secondary_entry_pa = __pa_symbol(secondary_entry);

	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_P31_CONSUME);
	ret = mt6797_a72_membership_p31_consume_attempt(9, CPUHP_ONLINE,
							MT6797_A72_ATTEMPT_CPU9_UP, &entry);
	if (ret)
		goto out_unlock;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_ENTRY_VALIDATE);
	ret = mt6797_a72_membership_validate_entry(9, CPUHP_ONLINE,
						       MT6797_A72_ATTEMPT_CPU9_UP, &entry);
	if (ret)
		goto out_unlock;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_TOKEN_MINT);
	ret = mt6797_a72_membership_mint_up_token(9, CPUHP_ONLINE,
						       MT6797_A72_ATTEMPT_CPU9_UP, &entry, ready,
						       transaction);
	if (ret)
		goto out_unlock;
	prestate.generation = transaction->identity.generation;
	prestate.cookie = transaction->identity.cookie;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_PRESTATE_VALIDATE);
	ret = mt6797_a72_membership_validate_up_prestate(transaction, &prestate);
	if (ret)
		goto reject_frozen;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_PRESTATE_BIND);
	ret = mt6797_a72_membership_bind_a36_prestate(&transaction->identity,
							      &prestate, transaction);
	if (ret)
		goto reject_frozen;
	mt6797_a72_derive_stage_set(derive_stage, MT6797_A72_DERIVE_COMPLETE);
	goto out_unlock;
reject_frozen:
	mt6797_a72_membership_reject_frozen(&transaction->identity);
	memset(transaction, 0, sizeof(*transaction));
out_unlock:
	memset(&direct, 0, sizeof(direct));
	memset(&entry, 0, sizeof(entry));
	memset(&prestate, 0, sizeof(prestate));
	cpus_read_unlock();
	return ret;
}

int
mt6797_a72_membership_derive_cpu8(const struct arm64_late_cpu_ready_token *ready,
				  struct mt6797_a72_transaction *transaction)
{
	return mt6797_a72_membership_derive_cpu8_diagnostic(ready, transaction,
							    NULL);
}

#ifdef CONFIG_ARM64_MT6797_A72_DERIVED_ADMISSION_KUNIT_TEST
int
mt6797_a72_membership_test_derive_cpu8(const struct mt6797_a72_direct_topology *topology,
				       const struct arm64_late_cpu_ready_token *ready,
				       struct mt6797_a72_transaction *transaction)
{
	int ret;

	if (!topology || !transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));
	cpus_read_lock();
	mutex_lock(&a72_transition_lock);
	ret = mt6797_a72_membership_derive_cpu8_locked(topology, ready,
						       transaction, NULL);
	mutex_unlock(&a72_transition_lock);
	cpus_read_unlock();
	return ret;
}
#endif

#endif

int
mt6797_a72_membership_begin_up(unsigned int cpu, enum cpuhp_state target,
			       u32 attempt,
			       const struct mt6797_a72_entry_snapshot *entry,
			       const struct arm64_late_cpu_ready_token *ready,
			       const struct mt6797_a72_a36_prestate *prestate,
			       struct mt6797_a72_transaction *transaction)
{
	int ret;

	if (!transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));

	mutex_lock(&a72_transition_lock);
	ret = mt6797_a72_membership_p31_consume_attempt(cpu, target, attempt, entry);
	if (ret)
		goto out_unlock;

	ret = mt6797_a72_membership_validate_entry(cpu, target, attempt,
						   entry);
	if (ret)
		goto out_unlock;

	ret = mt6797_a72_membership_mint_up_token(cpu, target, attempt, entry,
						  ready, transaction);
	if (ret)
		goto out_unlock;

	ret = mt6797_a72_membership_validate_up_prestate(transaction, prestate);
	if (ret) {
		mt6797_a72_membership_reject_frozen(&transaction->identity);
		memset(transaction, 0, sizeof(*transaction));
		goto out_unlock;
	}

	ret = mt6797_a72_membership_bind_a36_prestate(&transaction->identity,
						      prestate, transaction);
	if (ret) {
		mt6797_a72_membership_reject_frozen(&transaction->identity);
		memset(transaction, 0, sizeof(*transaction));
	}

out_unlock:
	mutex_unlock(&a72_transition_lock);

	return ret;
}

bool
mt6797_a72_membership_owns_up_token(const struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	bool owned;

	if (!token)
		return false;

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	owned = a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
		a72_owner.bootstrap_valid && a72_owner.active.valid &&
		a72_owner.active.p30_token_valid &&
		mt6797_a72_up_token_equal(&a72_owner.active.p30_token, token);
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return owned;
}

int mt6797_a72_membership_copy_up_token(struct arm64_late_cpu_up_token *token)
{
	unsigned long flags;
	int ret = -ENOENT;

	if (!token)
		return -EINVAL;
	memset(token, 0, sizeof(*token));

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_FAULTED) {
		ret = -ESHUTDOWN;
	} else if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
		   a72_owner.bootstrap_valid && a72_owner.active.valid &&
		   a72_owner.active.p30_token_valid) {
		*token = a72_owner.active.p30_token;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);

	return ret;
}

void mt6797_a72_membership_snapshot(struct mt6797_a72_owner_snapshot *snapshot)
{
	unsigned long flags;

	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	snapshot->active = a72_owner.active;
	memcpy(snapshot->retired, a72_owner.retired,
	       sizeof(snapshot->retired));
	snapshot->provider_identity = a72_owner.provider_identity;
	snapshot->first_fault = a72_owner.first_fault;
	snapshot->diagnostic_blockers = a72_owner.diagnostic_blockers;
	snapshot->controller_cookie = a72_owner.controller_cookie;
	snapshot->abi = MT6797_A72_TRANSACTION_ABI;
	snapshot->health = a72_owner.health;
	snapshot->phase = a72_owner.phase;
	snapshot->members = a72_owner.members;
	snapshot->provider_state = a72_owner.provider_state;
	snapshot->bootstrap_valid = a72_owner.bootstrap_valid;
	snapshot->members_valid = a72_owner.members_valid;
	snapshot->attempts_available = a72_owner.attempts_available;
	snapshot->attempts_consumed = a72_owner.attempts_consumed;
	snapshot->retired_mask = a72_owner.retired_mask;
	snapshot->controller_present = !!a72_owner.controller;
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
}

#ifdef CONFIG_ARM64_MT6797_A72_P24_OWNER_KUNIT_TEST
void mt6797_a72_membership_test_reset(void)
{
	unsigned long flags;

	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	memset(&a72_owner, 0, sizeof(a72_owner));
	a72_owner.diagnostic_blockers = MT6797_A72_BLOCK_MASK;
	a72_owner.health = MT6797_A72_OWNER_CLOSED;
	a72_owner.phase = MT6797_A72_PHASE_UNINITIALIZED;
	a72_owner.provider_state = MT6797_A72_PROVIDER_NONE;
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
}

void mt6797_a72_membership_test_seed_available(void)
{
	unsigned long flags;

	/* Harness-only state; no production path can open the CLOSED owner. */
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	memset(&a72_owner, 0, sizeof(a72_owner));
	a72_owner.diagnostic_blockers = MT6797_A72_BLOCK_MASK;
	a72_owner.health = MT6797_A72_OWNER_AVAILABLE;
	a72_owner.phase = MT6797_A72_PHASE_IDLE;
	a72_owner.provider_state = MT6797_A72_PROVIDER_NONE;
	a72_owner.bootstrap_valid = 1;
	a72_owner.members_valid = 1;
	a72_owner.attempts_available = MT6797_A72_ATTEMPT_MASK;
	a72_owner.next_generation = 1;
	a72_owner.next_cookie = 0xa7200001;
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
}

int
mt6797_a72_membership_test_claim_cpu8(struct mt6797_a72_transaction *transaction)
{
	unsigned long flags;
	int ret = -EPERM;

	if (!transaction)
		return -EINVAL;
	memset(transaction, 0, sizeof(*transaction));

	raw_spin_lock_irqsave(&a72_state_lock, flags);
	if (a72_owner.health == MT6797_A72_OWNER_AVAILABLE &&
	    a72_owner.phase == MT6797_A72_PHASE_ON_ISSUED &&
	    a72_owner.active.valid && a72_owner.active.p17_p18_published &&
	    a72_owner.active.p30_token_valid &&
	    a72_owner.active.public_preflight ==
		    MT6797_A72_PUBLIC_ADMISSION_NONE &&
	    a72_owner.active.identity.operation ==
		    ARM64_LATE_CPU_STARTUP_OP_CPU8_UP &&
	    a72_owner.active.identity.target_cpu == 8 &&
	    a72_owner.active.identity.cpuhp_target == CPUHP_ONLINE &&
	    a72_owner.controller == current &&
	    a72_owner.controller_cookie == a72_owner.active.identity.cookie &&
	    !a72_owner.members &&
	    a72_owner.provider_state == MT6797_A72_PROVIDER_NONE &&
	    !cpu_online(8) && !cpu_online(9)) {
		a72_owner.active.public_preflight =
			MT6797_A72_PUBLIC_ADMISSION_CLAIMED;
		*transaction = a72_owner.active;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	return ret;
}

int mt6797_a72_membership_test_publish_cpu8_success(struct mt6797_a72_transaction *transaction,
						    bool cpu8_online, bool cpu9_online)
{
	return mt6797_a72_publish_cpu8_success_state(transaction,
						     cpu8_online, cpu9_online);
}

int mt6797_a72_membership_test_finalize_cpu8_success(struct mt6797_a72_transaction *transaction,
						     bool cpu8_online, bool cpu9_online)
{
	return mt6797_a72_finalize_cpu8_success_state(transaction,
						      cpu8_online, cpu9_online);
}

void mt6797_a72_membership_test_seed_available_cpu9(void)
{
	unsigned long flags;

	mt6797_a72_membership_test_seed_available();
	mutex_lock(&a72_transition_lock);
	raw_spin_lock_irqsave(&a72_state_lock, flags);
	/* Harness-only durable M01 identity; no provider call is made. */
	a72_owner.provider_state = MT6797_A72_PROVIDER_HELD;
	a72_owner.provider_identity.generation = 1;
	a72_owner.provider_identity.cookie = 0xa72000f0;
	raw_spin_unlock_irqrestore(&a72_state_lock, flags);
	mutex_unlock(&a72_transition_lock);
}
#endif
