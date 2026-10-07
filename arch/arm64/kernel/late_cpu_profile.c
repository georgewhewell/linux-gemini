// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed arm64 profile for CPUs expected after capability finalization.
 *
 * A profile callback may collect fallible evidence, but only the architecture
 * core can normalize and publish an immutable plan. A separate receipt tracks
 * commit and verification without modifying that plan. This lifecycle does
 * not admit a CPU_ON request.
 */

#define pr_fmt(fmt) "arm64-late-cpu-profile: " fmt

#include <crypto/sha2.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <asm/cpu.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/late_cpu_profile.h>
#include <asm/mmu_context.h>
#include <asm/sysreg.h>

static struct arm64_late_cpu_profile late_profile __initdata;
static bool late_profile_active __initdata;
static cpumask_t late_profile_targets __initdata;
static bool late_profile_registration_fault __initdata;

/*
 * Runtime observations have a separate owner from profile expectations.
 * The architecture collects system capabilities and mitigation policy before
 * finalization; profiles never receive a writable reference to this record.
 */
enum late_runtime_evidence_state {
	LATE_RUNTIME_EVIDENCE_OPEN,
	LATE_RUNTIME_EVIDENCE_SEALED,
	LATE_RUNTIME_EVIDENCE_FAULT,
};

static struct arm64_late_cpu_evidence late_runtime_evidence __initdata = {
	.abi = ARM64_LATE_CPU_PLAN_ABI,
};

static u32 late_runtime_evidence_state __initdata;

static struct arm64_late_cpu_evidence profile_evidence __initdata;
static struct arm64_late_cpu_plan draft __initdata;
static struct arm64_late_cpu_plan late_plan __ro_after_init;
static struct arm64_late_cpu_receipt late_receipt __ro_after_init = {
	.abi = ARM64_LATE_CPU_PLAN_ABI,
	.state = ARM64_LATE_CPU_PROFILE_NONE,
};

static struct arm64_late_cpu_ready_token late_ready_token __ro_after_init;

static void __init late_profile_block(u64 blockers, const char *reason)
{
	if (!late_receipt.profile_id[0])
		strscpy(late_receipt.profile_id, "invalid-registration",
			sizeof(late_receipt.profile_id));
	late_receipt.blocker_mask |= blockers;
	/* Pairs with state loads before any blocker receipt is consumed. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_BLOCKED);
	pr_warn("%.*s blocked: %s (proof mask %#llx)\n",
		ARM64_LATE_CPU_PROFILE_ID_LEN, late_receipt.profile_id, reason,
		late_receipt.blocker_mask);
}

static bool __init late_profile_plan_has_identity(const struct arm64_late_cpu_plan *plan)
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (plan->identity[i])
			return true;

	return false;
}

static bool __init
late_profile_identity_empty(const u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (identity[i])
			return false;

	return true;
}

static const u8 late_evidence_identity_domain[] __initconst =
	"gemini-late-cpu-evidence-v2";
static const u8 late_plan_identity_domain[] __initconst =
	"gemini-late-cpu-plan-v2";

static void __init
late_canonical_update_u8(struct sha256_ctx *ctx, u8 value)
{
	sha256_update(ctx, &value, sizeof(value));
}

static void __init
late_canonical_update_u32(struct sha256_ctx *ctx, u32 value)
{
	__be32 encoded = cpu_to_be32(value);

	sha256_update(ctx, (const u8 *)&encoded, sizeof(encoded));
}

static void __init
late_canonical_update_u64(struct sha256_ctx *ctx, u64 value)
{
	__be64 encoded = cpu_to_be64(value);

	sha256_update(ctx, (const u8 *)&encoded, sizeof(encoded));
}

static void __init
late_canonical_update_identity(struct sha256_ctx *ctx,
			       const u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		late_canonical_update_u64(ctx, identity[i]);
}

static void __init
late_canonical_digest_identity(struct sha256_ctx *ctx,
			       u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	u8 digest[SHA256_DIGEST_SIZE];
	unsigned int i;

	sha256_final(ctx, digest);
	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		identity[i] = get_unaligned_be64(digest + i * sizeof(u64));
}

static void __init
late_canonical_update_registers(struct sha256_ctx *ctx,
				const struct arm64_late_cpu_register_image *registers)
{
	const struct arm64_late_cpu_aarch32_register_image *aarch32 =
		&registers->aarch32;

	late_canonical_update_u64(ctx, registers->ctr);
	late_canonical_update_u64(ctx, registers->cntfrq);
	late_canonical_update_u64(ctx, registers->dczid);
	late_canonical_update_u64(ctx, registers->midr);
	late_canonical_update_u64(ctx, registers->revidr);
	late_canonical_update_u64(ctx, registers->aidr);
	late_canonical_update_u64(ctx, registers->gmid);
	late_canonical_update_u64(ctx, registers->smidr);
	late_canonical_update_u64(ctx, registers->mpamidr);
	late_canonical_update_u64(ctx, registers->id_aa64dfr0);
	late_canonical_update_u64(ctx, registers->id_aa64dfr1);
	late_canonical_update_u64(ctx, registers->id_aa64isar0);
	late_canonical_update_u64(ctx, registers->id_aa64isar1);
	late_canonical_update_u64(ctx, registers->id_aa64isar2);
	late_canonical_update_u64(ctx, registers->id_aa64isar3);
	late_canonical_update_u64(ctx, registers->id_aa64mmfr0);
	late_canonical_update_u64(ctx, registers->id_aa64mmfr1);
	late_canonical_update_u64(ctx, registers->id_aa64mmfr2);
	late_canonical_update_u64(ctx, registers->id_aa64mmfr3);
	late_canonical_update_u64(ctx, registers->id_aa64mmfr4);
	late_canonical_update_u64(ctx, registers->id_aa64pfr0);
	late_canonical_update_u64(ctx, registers->id_aa64pfr1);
	late_canonical_update_u64(ctx, registers->id_aa64pfr2);
	late_canonical_update_u64(ctx, registers->id_aa64zfr0);
	late_canonical_update_u64(ctx, registers->id_aa64smfr0);
	late_canonical_update_u64(ctx, registers->id_aa64fpfr0);
	late_canonical_update_u32(ctx, aarch32->id_dfr0);
	late_canonical_update_u32(ctx, aarch32->id_dfr1);
	late_canonical_update_u32(ctx, aarch32->id_isar0);
	late_canonical_update_u32(ctx, aarch32->id_isar1);
	late_canonical_update_u32(ctx, aarch32->id_isar2);
	late_canonical_update_u32(ctx, aarch32->id_isar3);
	late_canonical_update_u32(ctx, aarch32->id_isar4);
	late_canonical_update_u32(ctx, aarch32->id_isar5);
	late_canonical_update_u32(ctx, aarch32->id_isar6);
	late_canonical_update_u32(ctx, aarch32->id_mmfr0);
	late_canonical_update_u32(ctx, aarch32->id_mmfr1);
	late_canonical_update_u32(ctx, aarch32->id_mmfr2);
	late_canonical_update_u32(ctx, aarch32->id_mmfr3);
	late_canonical_update_u32(ctx, aarch32->id_mmfr4);
	late_canonical_update_u32(ctx, aarch32->id_mmfr5);
	late_canonical_update_u32(ctx, aarch32->id_pfr0);
	late_canonical_update_u32(ctx, aarch32->id_pfr1);
	late_canonical_update_u32(ctx, aarch32->id_pfr2);
	late_canonical_update_u32(ctx, aarch32->mvfr0);
	late_canonical_update_u32(ctx, aarch32->mvfr1);
	late_canonical_update_u32(ctx, aarch32->mvfr2);
}

static void __init
late_canonical_update_expected_pair(struct sha256_ctx *ctx,
				    const struct arm64_late_cpu_expected_pair *expected)
{
	unsigned int target;

	late_canonical_update_u32(ctx, expected->abi);
	late_canonical_update_u32(ctx, expected->target_count);
	late_canonical_update_u64(ctx, expected->valid);
	late_canonical_update_identity(ctx, expected->source_identity);
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		late_canonical_update_u64(ctx,
					  expected->capsule_identity[target]);
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		late_canonical_update_u64(ctx, expected->mpidr[target]);
	late_canonical_update_u64(ctx, expected->midr);
	late_canonical_update_u64(ctx, expected->revidr);
	late_canonical_update_u64(ctx, expected->cntfrq);
	late_canonical_update_u64(ctx, expected->ctr);
	late_canonical_update_u64(ctx, expected->dczid);
	late_canonical_update_u64(ctx, expected->clidr_el1);
	late_canonical_update_u64(ctx, expected->id_aa64dfr0);
	late_canonical_update_u64(ctx, expected->id_aa64isar0);
	late_canonical_update_u64(ctx, expected->id_aa64isar1);
	late_canonical_update_u64(ctx, expected->id_aa64mmfr0);
	late_canonical_update_u64(ctx, expected->id_aa64mmfr1);
	late_canonical_update_u64(ctx, expected->id_aa64pfr0);
	late_canonical_update_u64(ctx, expected->id_aa64pfr1);
	late_canonical_update_u32(ctx, expected->id_isar0);
	late_canonical_update_u32(ctx, expected->id_isar1);
	late_canonical_update_u32(ctx, expected->id_isar2);
	late_canonical_update_u32(ctx, expected->id_isar3);
	late_canonical_update_u32(ctx, expected->id_isar4);
	late_canonical_update_u32(ctx, expected->id_isar5);
	late_canonical_update_u32(ctx, expected->id_mmfr0);
	late_canonical_update_u32(ctx, expected->id_mmfr1);
	late_canonical_update_u32(ctx, expected->id_mmfr2);
	late_canonical_update_u32(ctx, expected->id_mmfr3);
	late_canonical_update_u32(ctx, expected->id_pfr0);
	late_canonical_update_u32(ctx, expected->id_pfr1);
}

static void __init
late_canonical_update_target_cap(struct sha256_ctx *ctx,
				 const struct arm64_late_cpu_target_cap_evidence *target)
{
	late_canonical_update_u32(ctx, target->valid);
	late_canonical_update_registers(ctx, &target->registers);
	late_canonical_update_u64(ctx, target->clidr_el1);
	late_canonical_update_u64(ctx, target->ctr_effective);
	late_canonical_update_u64(ctx, target->icc_sre_el1);
	late_canonical_update_u64(ctx, target->icc_idr0_el1);
	late_canonical_update_u64(ctx, target->ich_vtr_el2);
	late_canonical_update_u32(ctx, (u32)target->ich_vtr_status);
	late_canonical_update_u32(ctx, (u32)target->smccc_wa1);
	late_canonical_update_u32(ctx, (u32)target->smccc_wa2);
	late_canonical_update_u32(ctx, (u32)target->smccc_wa3);
	late_canonical_update_u32(ctx, target->asid_bits);
	late_canonical_update_u8(ctx, target->page_shift);
	late_canonical_update_u8(ctx, target->va_bits);
	late_canonical_update_u8(ctx, target->hyp_available);
	late_canonical_update_u8(ctx, target->kernel_in_hyp_mode);
	late_canonical_update_u8(ctx, target->gic_sre_usable);
	late_canonical_update_u8(ctx, target->ich_vtr_source);
}

static void __init
late_canonical_update_target_policy(struct sha256_ctx *ctx,
				    const struct arm64_late_cpu_target_policy_evidence *policy)
{
	late_canonical_update_u32(ctx, policy->valid);
	late_canonical_update_u8(ctx, policy->smccc_conduit);
	late_canonical_update_u8(ctx, policy->mitigations_off);
	late_canonical_update_u8(ctx, policy->nospectre_v2);
	late_canonical_update_u8(ctx, policy->spectre_v4_policy);
}

static void __init
late_canonical_update_system_cap(struct sha256_ctx *ctx,
				 const struct arm64_late_cpu_system_cap_evidence *system)
{
	late_canonical_update_u32(ctx, system->valid);
	late_canonical_update_u64(ctx, system->ctr_sys_val);
	late_canonical_update_u64(ctx, system->ctr_strict_mask);
	late_canonical_update_u8(ctx, system->ssbs);
	late_canonical_update_u8(ctx, system->spectre_v2_state);
	late_canonical_update_u8(ctx, system->spectre_v4_state);
	late_canonical_update_u8(ctx, system->bhb_state);
	late_canonical_update_u8(ctx, system->bhb_matcher_loop_count);
	late_canonical_update_u8(ctx, system->bhb_system_method);
	late_canonical_update_u8(ctx, system->gicv5_legacy);
	late_canonical_update_u8(ctx, system->ich_hcr_tdir);
}

static void __init
late_canonical_hash_evidence(const struct arm64_late_cpu_evidence *evidence,
			     u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	struct sha256_ctx ctx;
	unsigned int target;

	sha256_init(&ctx);
	sha256_update(&ctx, late_evidence_identity_domain,
		      sizeof(late_evidence_identity_domain));
	late_canonical_update_u32(&ctx, evidence->abi);
	late_canonical_update_expected_pair(&ctx, &evidence->expected_pair);
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		late_canonical_update_u32(&ctx, evidence->target_cpu[target]);
		late_canonical_update_u64(&ctx,
					  evidence->expected_target_mpidr[target]);
		late_canonical_update_u64(&ctx,
					  evidence->observed_target_mpidr[target]);
		late_canonical_update_u32(&ctx,
					  evidence->expected_target_midr[target]);
		late_canonical_update_u32(&ctx,
					  evidence->observed_target_midr[target]);
		late_canonical_update_u32(&ctx,
					  evidence->observed_target_revidr[target]);
		late_canonical_update_target_cap(&ctx,
						 &evidence->target_cap[target]);
		late_canonical_update_target_policy(&ctx,
						    &evidence->target_policy[target]);
	}
	late_canonical_update_system_cap(&ctx, &evidence->system_cap);
	/* blocker_mask is a derived admission result, not input evidence. */
	late_canonical_digest_identity(&ctx, identity);
}

static void __init
late_canonical_update_bitmap(struct sha256_ctx *ctx,
			     const unsigned long *bitmap)
{
	unsigned int cap;

	late_canonical_update_u32(ctx, ARM64_NCAPS);
	for (cap = 0; cap < ARM64_NCAPS; cap++)
		late_canonical_update_u8(ctx, test_bit(cap, bitmap));
}

static void __init
late_canonical_update_target_effect(struct sha256_ctx *ctx,
				    const struct arm64_late_cpu_target_effect_plan *effect)
{
	late_canonical_update_u32(ctx, effect->valid);
	late_canonical_update_u8(ctx, effect->spectre_v2_state);
	late_canonical_update_u8(ctx, effect->spectre_v2_conduit);
	late_canonical_update_u8(ctx, effect->spectre_v2_callback);
	late_canonical_update_u8(ctx, effect->spectre_v2_hyp_vector);
	late_canonical_update_u8(ctx, effect->spectre_v4_state);
	late_canonical_update_u8(ctx, effect->spectre_v4_method);
	late_canonical_update_u8(ctx, effect->spectre_v4_conduit);
	late_canonical_update_u8(ctx, effect->spectre_v4_policy);
	late_canonical_update_u8(ctx,
				 effect->spectre_v4_callback_required);
	late_canonical_update_u8(ctx, effect->bhb_method);
	late_canonical_update_u8(ctx, effect->bhb_loop_count);
	late_canonical_update_u8(ctx, effect->bhb_matcher_loop_count);
	late_canonical_update_u8(ctx, effect->bhb_conduit);
	late_canonical_update_u8(ctx, effect->bhb_mitigation_state);
	late_canonical_update_u8(ctx, effect->bhb_vector_template);
	late_canonical_update_u8(ctx, effect->bhb_hyp_vector);
	late_canonical_update_u8(ctx, effect->bhb_v2_non_vulnerable);
}

static void __init
late_canonical_update_effects(struct sha256_ctx *ctx,
			      const struct arm64_late_cpu_effect_plan *effects)
{
	unsigned int target;

	late_canonical_update_u8(ctx, effects->ctr_mismatch.required);
	late_canonical_update_u8(ctx, effects->ctr_mismatch.target_mask);
	late_canonical_update_u8(ctx, effects->ctr_mismatch.trap_ctr_el0);
	late_canonical_update_u8(ctx, effects->ctr_mismatch.alternative);
	late_canonical_update_u8(ctx, effects->spectre_v2.required);
	late_canonical_update_u8(ctx, effects->spectre_v2.target_mask);
	late_canonical_update_u8(ctx, effects->spectre_v2.mitigation_state);
	late_canonical_update_u8(ctx, effects->spectre_v2.conduit);
	late_canonical_update_u8(ctx, effects->spectre_v2.callback);
	late_canonical_update_u8(ctx, effects->spectre_v2.hyp_vector);
	late_canonical_update_u8(ctx, effects->spectre_v2.alternative);
	late_canonical_update_u8(ctx, effects->spectre_v4.required);
	late_canonical_update_u8(ctx, effects->spectre_v4.target_mask);
	late_canonical_update_u8(ctx, effects->spectre_v4.mitigation_state);
	late_canonical_update_u8(ctx, effects->spectre_v4.method);
	late_canonical_update_u8(ctx, effects->spectre_v4.conduit);
	late_canonical_update_u8(ctx, effects->spectre_v4.policy);
	late_canonical_update_u8(ctx,
				 effects->spectre_v4.callback_required_mask);
	late_canonical_update_u8(ctx,
				 effects->spectre_v4.firmware_alternative);
	late_canonical_update_u8(ctx, effects->bhb.required);
	late_canonical_update_u8(ctx, effects->bhb.target_mask);
	late_canonical_update_u8(ctx, effects->bhb.method);
	late_canonical_update_u8(ctx, effects->bhb.loop_count);
	late_canonical_update_u8(ctx, effects->bhb.matcher_loop_count);
	late_canonical_update_u8(ctx, effects->bhb.conduit);
	late_canonical_update_u8(ctx, effects->bhb.system_method);
	late_canonical_update_u8(ctx, effects->bhb.mitigation_state);
	late_canonical_update_u8(ctx, effects->bhb.vector_template);
	late_canonical_update_u8(ctx, effects->bhb.hyp_vector);
	late_canonical_update_u8(ctx, effects->bhb.alternative);
	late_canonical_update_u8(ctx, effects->bhb.v2_non_vulnerable);
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		late_canonical_update_target_effect(ctx,
						    &effects->target[target]);
	late_canonical_update_u8(ctx, effects->compat_aes_clear);
	late_canonical_update_u8(ctx, effects->speculative_at_finalization);
}

static void __init
late_canonical_hash_plan(const struct arm64_late_cpu_plan *plan,
			 u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	struct sha256_ctx ctx;
	unsigned int target;
	unsigned int cpu;
	unsigned int i;

	sha256_init(&ctx);
	sha256_update(&ctx, late_plan_identity_domain,
		      sizeof(late_plan_identity_domain));
	late_canonical_update_u32(&ctx, plan->abi);
	late_canonical_update_u32(&ctx, strlen(plan->profile_id));
	sha256_update(&ctx, (const u8 *)plan->profile_id,
		      strlen(plan->profile_id));
	late_canonical_update_u32(&ctx, cpumask_weight(&plan->target_cpus));
	for_each_cpu(cpu, &plan->target_cpus)
		late_canonical_update_u32(&ctx, cpu);
	late_canonical_update_identity(&ctx,
				       plan->evidence.evidence_identity);
	late_canonical_update_bitmap(&ctx, plan->canonical_caps);
	late_canonical_update_bitmap(&ctx, plan->compiled_local_caps);
	late_canonical_update_bitmap(&ctx, plan->classified_local_caps);
	late_canonical_update_bitmap(&ctx, plan->early_local_caps);
	late_canonical_update_bitmap(&ctx, plan->target_local_caps);
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		late_canonical_update_bitmap(&ctx,
					     plan->target[target].classified_local_caps);
		late_canonical_update_bitmap(&ctx,
					     plan->target[target].local_caps);
	}
	late_canonical_update_bitmap(&ctx, plan->required_local_caps);
	late_canonical_update_bitmap(&ctx, plan->conflicting_local_caps);
	late_canonical_update_effects(&ctx, &plan->effects);
	for (i = 0; i < ARRAY_SIZE(plan->expected_elf_hwcap); i++)
		late_canonical_update_u64(&ctx, plan->expected_elf_hwcap[i]);
	late_canonical_update_u32(&ctx, plan->expected_compat_hwcap);
	late_canonical_update_u32(&ctx, plan->expected_compat_hwcap2);
	late_canonical_update_u8(&ctx, plan->local_caps_planned);
	late_canonical_update_u8(&ctx, plan->effects_planned);
	late_canonical_update_u8(&ctx, plan->hwcaps_planned);
	late_canonical_digest_identity(&ctx, identity);
}

static int __init
late_profile_finalize_plan_identity(struct arm64_late_cpu_plan *plan)
{
	if (!plan || !plan->local_caps_planned || !plan->effects_planned ||
	    !plan->hwcaps_planned ||
	    strnlen(plan->profile_id, sizeof(plan->profile_id)) >=
		    sizeof(plan->profile_id) ||
	    !late_profile_identity_empty(plan->evidence.evidence_identity) ||
	    !late_profile_identity_empty(plan->identity))
		return -EINVAL;

	late_canonical_hash_evidence(&plan->evidence,
				     plan->evidence.evidence_identity);
	if (late_profile_identity_empty(plan->evidence.evidence_identity))
		return -EKEYREJECTED;
	late_canonical_hash_plan(plan, plan->identity);
	if (late_profile_identity_empty(plan->identity)) {
		memset(plan->evidence.evidence_identity, 0,
		       sizeof(plan->evidence.evidence_identity));
		return -EKEYREJECTED;
	}

	return 0;
}

static bool __init late_runtime_evidence_storage_empty(void);

void __init arm64_collect_late_cpu_runtime_system_policy(void)
{
	struct arm64_late_cpu_target_policy_evidence policy = {};
	struct arm64_late_cpu_system_cap_evidence system = {};
	unsigned int target;
	int ret;

	if (READ_ONCE(late_runtime_evidence_state) !=
		    LATE_RUNTIME_EVIDENCE_OPEN ||
	    system_capabilities_finalized() ||
	    cpus_have_cap(ARM64_ALWAYS_SYSTEM) ||
	    !late_runtime_evidence_storage_empty()) {
		WRITE_ONCE(late_runtime_evidence_state,
			   LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	ret = arm64_late_cpu_collect_system(&system);
	if (!ret)
		ret = arm64_late_cpu_collect_policy(&policy, &system);
	if (ret ||
	    system.valid != ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK ||
	    policy.valid != ARM64_LATE_CPU_TARGET_POLICY_VALID_MASK) {
		WRITE_ONCE(late_runtime_evidence_state,
			   LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	late_runtime_evidence.system_cap = system;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		late_runtime_evidence.target_policy[target] = policy;
}

/* Profile-owned production input may describe expectations, never observations. */
static bool __init
late_profile_runtime_fields_empty(const struct arm64_late_cpu_evidence *evidence)
{
	unsigned int target;

	if (!late_profile_identity_empty(evidence->evidence_identity))
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (evidence->observed_target_mpidr[target] ||
		    evidence->observed_target_midr[target] ||
		    evidence->observed_target_revidr[target] ||
		    memchr_inv(&evidence->target_cap[target], 0,
			       sizeof(evidence->target_cap[target])) ||
		    memchr_inv(&evidence->target_policy[target], 0,
			       sizeof(evidence->target_policy[target])))
			return false;

	return !memchr_inv(&evidence->system_cap, 0,
			   sizeof(evidence->system_cap));
}

static bool __init
late_runtime_evidence_storage_empty(void)
{
	unsigned int target;

	if (late_runtime_evidence.abi != ARM64_LATE_CPU_PLAN_ABI ||
	    memchr_inv(&late_runtime_evidence.expected_pair, 0,
		       sizeof(late_runtime_evidence.expected_pair)) ||
	    !late_profile_identity_empty(late_runtime_evidence.evidence_identity) ||
	    late_runtime_evidence.blocker_mask ||
	    memchr_inv(&late_runtime_evidence.system_cap, 0,
		       sizeof(late_runtime_evidence.system_cap)))
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (late_runtime_evidence.target_cpu[target] ||
		    late_runtime_evidence.expected_target_mpidr[target] ||
		    late_runtime_evidence.observed_target_mpidr[target] ||
		    late_runtime_evidence.expected_target_midr[target] ||
		    late_runtime_evidence.observed_target_midr[target] ||
		    late_runtime_evidence.observed_target_revidr[target] ||
		    memchr_inv(&late_runtime_evidence.target_cap[target], 0,
			       sizeof(late_runtime_evidence.target_cap[target])) ||
		    memchr_inv(&late_runtime_evidence.target_policy[target], 0,
			       sizeof(late_runtime_evidence.target_policy[target])))
			return false;
	}

	return true;
}

static bool __init
late_runtime_system_policy_complete(void)
{
	const struct arm64_late_cpu_system_cap_evidence *system =
		&late_runtime_evidence.system_cap;
	const struct arm64_late_cpu_target_policy_evidence *first =
		&late_runtime_evidence.target_policy[0];
	unsigned int target;

	if (system->valid != ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK ||
	    !system->ctr_strict_mask || system->ssbs > 1 ||
	    system->spectre_v2_state <
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    system->spectre_v2_state >
		    ARM64_LATE_CPU_MITIGATION_VULNERABLE ||
	    system->spectre_v4_state <
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    system->spectre_v4_state >
		    ARM64_LATE_CPU_MITIGATION_VULNERABLE ||
	    system->bhb_state < ARM64_LATE_CPU_BHB_STATE_UNAFFECTED ||
	    system->bhb_state > ARM64_LATE_CPU_BHB_STATE_VULNERABLE ||
	    system->bhb_system_method & ~GENMASK(3, 0) ||
	    first->valid != ARM64_LATE_CPU_TARGET_POLICY_VALID_MASK ||
	    first->smccc_conduit < ARM64_LATE_CPU_SMCCC_NONE ||
	    first->smccc_conduit > ARM64_LATE_CPU_SMCCC_HVC ||
	    first->mitigations_off > 1 || first->nospectre_v2 > 1 ||
	    first->spectre_v4_policy <
		    ARM64_LATE_CPU_V4_POLICY_DYNAMIC ||
	    first->spectre_v4_policy >
		    ARM64_LATE_CPU_V4_POLICY_FORCE_OFF)
		return false;

	for (target = 1; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (memcmp(first, &late_runtime_evidence.target_policy[target],
			   sizeof(*first)))
			return false;

	return true;
}

static bool __init
late_runtime_evidence_storage_complete(void)
{
	unsigned int target;

	if (late_runtime_evidence.abi != ARM64_LATE_CPU_PLAN_ABI ||
	    memchr_inv(&late_runtime_evidence.expected_pair, 0,
		       sizeof(late_runtime_evidence.expected_pair)) ||
	    !late_profile_identity_empty(late_runtime_evidence.evidence_identity) ||
	    late_runtime_evidence.blocker_mask ||
	    !late_runtime_system_policy_complete())
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (late_runtime_evidence.target_cpu[target] ||
		    late_runtime_evidence.expected_target_mpidr[target] ||
		    late_runtime_evidence.observed_target_mpidr[target] ||
		    late_runtime_evidence.expected_target_midr[target] ||
		    late_runtime_evidence.observed_target_midr[target] ||
		    late_runtime_evidence.observed_target_revidr[target] ||
		    memchr_inv(&late_runtime_evidence.target_cap[target], 0,
			       sizeof(late_runtime_evidence.target_cap[target])))
			return false;

	return true;
}

void __init arm64_seal_late_cpu_runtime_evidence(void)
{
	u32 state = READ_ONCE(late_runtime_evidence_state);

	if (state != LATE_RUNTIME_EVIDENCE_OPEN ||
	    system_capabilities_finalized() ||
	    cpus_have_cap(ARM64_ALWAYS_SYSTEM)) {
		/* Publish the fault before profile preparation examines state. */
		smp_store_release(&late_runtime_evidence_state,
				  LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	if (!late_runtime_evidence_storage_complete()) {
		/* Publish the fault before profile preparation examines state. */
		smp_store_release(&late_runtime_evidence_state,
				  LATE_RUNTIME_EVIDENCE_FAULT);
		return;
	}

	/* Publish every core-owned field before profile planning may consume it. */
	smp_store_release(&late_runtime_evidence_state,
			  LATE_RUNTIME_EVIDENCE_SEALED);
}

static bool __init
late_profile_target_effects_match(
	const struct arm64_late_cpu_target_effect_plan *left,
	const struct arm64_late_cpu_target_effect_plan *right)
{
	return left->valid == right->valid &&
	       left->spectre_v2_state == right->spectre_v2_state &&
	       left->spectre_v2_conduit == right->spectre_v2_conduit &&
	       left->spectre_v2_callback == right->spectre_v2_callback &&
	       left->spectre_v2_hyp_vector == right->spectre_v2_hyp_vector &&
	       left->spectre_v4_state == right->spectre_v4_state &&
	       left->spectre_v4_method == right->spectre_v4_method &&
	       left->spectre_v4_conduit == right->spectre_v4_conduit &&
	       left->spectre_v4_policy == right->spectre_v4_policy &&
	       left->spectre_v4_callback_required ==
		       right->spectre_v4_callback_required &&
	       left->bhb_method == right->bhb_method &&
	       left->bhb_loop_count == right->bhb_loop_count &&
	       left->bhb_matcher_loop_count == right->bhb_matcher_loop_count &&
	       left->bhb_conduit == right->bhb_conduit &&
	       left->bhb_mitigation_state == right->bhb_mitigation_state &&
	       left->bhb_vector_template == right->bhb_vector_template &&
	       left->bhb_hyp_vector == right->bhb_hyp_vector &&
	       left->bhb_v2_non_vulnerable == right->bhb_v2_non_vulnerable;
}

static bool __init
late_profile_effects_match(const struct arm64_late_cpu_effect_plan *left,
			   const struct arm64_late_cpu_effect_plan *right)
{
	unsigned int target;

	if (left->ctr_mismatch.required != right->ctr_mismatch.required ||
	    left->ctr_mismatch.target_mask != right->ctr_mismatch.target_mask ||
	    left->ctr_mismatch.trap_ctr_el0 !=
		    right->ctr_mismatch.trap_ctr_el0 ||
	    left->ctr_mismatch.alternative != right->ctr_mismatch.alternative ||
	    left->spectre_v2.required != right->spectre_v2.required ||
	    left->spectre_v2.target_mask != right->spectre_v2.target_mask ||
	    left->spectre_v2.mitigation_state !=
		    right->spectre_v2.mitigation_state ||
	    left->spectre_v2.conduit != right->spectre_v2.conduit ||
	    left->spectre_v2.callback != right->spectre_v2.callback ||
	    left->spectre_v2.hyp_vector != right->spectre_v2.hyp_vector ||
	    left->spectre_v2.alternative != right->spectre_v2.alternative ||
	    left->spectre_v4.required != right->spectre_v4.required ||
	    left->spectre_v4.target_mask != right->spectre_v4.target_mask ||
	    left->spectre_v4.mitigation_state !=
		    right->spectre_v4.mitigation_state ||
	    left->spectre_v4.method != right->spectre_v4.method ||
	    left->spectre_v4.conduit != right->spectre_v4.conduit ||
	    left->spectre_v4.policy != right->spectre_v4.policy ||
	    left->spectre_v4.callback_required_mask !=
		    right->spectre_v4.callback_required_mask ||
	    left->spectre_v4.firmware_alternative !=
		    right->spectre_v4.firmware_alternative ||
	    left->bhb.required != right->bhb.required ||
	    left->bhb.target_mask != right->bhb.target_mask ||
	    left->bhb.method != right->bhb.method ||
	    left->bhb.loop_count != right->bhb.loop_count ||
	    left->bhb.matcher_loop_count != right->bhb.matcher_loop_count ||
	    left->bhb.conduit != right->bhb.conduit ||
	    left->bhb.system_method != right->bhb.system_method ||
	    left->bhb.mitigation_state != right->bhb.mitigation_state ||
	    left->bhb.vector_template != right->bhb.vector_template ||
	    left->bhb.hyp_vector != right->bhb.hyp_vector ||
	    left->bhb.alternative != right->bhb.alternative ||
	    left->bhb.v2_non_vulnerable != right->bhb.v2_non_vulnerable ||
	    left->compat_aes_clear != right->compat_aes_clear ||
	    left->speculative_at_finalization !=
		    right->speculative_at_finalization)
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (!late_profile_target_effects_match(
			    &left->target[target], &right->target[target]))
			return false;

	return true;
}

static bool __init
late_profile_receipt_identity_matches(const struct arm64_late_cpu_plan *plan,
				      const struct arm64_late_cpu_receipt *receipt)
{
	return receipt->abi == plan->abi &&
	       !memcmp(receipt->profile_id, plan->profile_id,
		       sizeof(receipt->profile_id)) &&
	       !memcmp(receipt->plan_identity, plan->identity,
		       sizeof(receipt->plan_identity));
}

static bool __init
late_profile_descriptor_matches(const struct arm64_late_cpu_profile *profile)
{
	return profile && profile->name && profile->prepare &&
	       profile->derive_effects &&
	       strnlen(profile->name, ARM64_LATE_CPU_PROFILE_ID_LEN) <
		       ARM64_LATE_CPU_PROFILE_ID_LEN &&
	       late_profile_active &&
	       !strcmp(late_profile.name, profile->name) &&
	       late_profile.target_count == profile->target_count &&
	       late_profile.classify_local_cap == profile->classify_local_cap &&
	       late_profile.validate_plan == profile->validate_plan &&
	       late_profile.derive_effects == profile->derive_effects &&
	       late_profile.prepare == profile->prepare &&
	       late_profile.verify_system == profile->verify_system &&
	       late_profile.finalize_user == profile->finalize_user;
}

void __init
arm64_activate_late_cpu_profile(const struct arm64_late_cpu_profile *profile)
{
	if (!profile || !profile->name || !profile->prepare ||
	    !profile->classify_local_cap || !profile->validate_plan ||
	    !profile->derive_effects ||
	    !profile->target_count ||
	    profile->target_count > ARM64_LATE_CPU_MAX_TARGETS) {
		late_profile_registration_fault = true;
		return;
	}

	if (!late_profile_active) {
		if (strscpy(late_receipt.profile_id, profile->name,
			    sizeof(late_receipt.profile_id)) < 0) {
			late_profile_registration_fault = true;
			return;
		}
		late_profile = *profile;
		late_profile.name = late_receipt.profile_id;
		late_profile_active = true;
		WRITE_ONCE(late_receipt.state,
			   ARM64_LATE_CPU_PROFILE_REGISTERED);
	} else if (!late_profile_descriptor_matches(profile)) {
		late_profile_registration_fault = true;
	}
}

void __init
arm64_register_late_cpu_target(const struct arm64_late_cpu_profile *profile,
			       unsigned int cpu)
{
	if (!late_profile_descriptor_matches(profile) || cpu >= nr_cpu_ids) {
		late_profile_registration_fault = true;
		return;
	}

	if (cpumask_test_and_set_cpu(cpu, &late_profile_targets))
		late_profile_registration_fault = true;
}

void __init arm64_prepare_late_cpu_profile(void)
{
	u32 runtime_state;
	unsigned int target;
	int identity_ret;
	int validate_ret;
	int hwcap_ret;
	int effect_ret;
	int plan_ret;
	int ret;

	memset(&profile_evidence, 0, sizeof(profile_evidence));
	profile_evidence.abi = ARM64_LATE_CPU_PLAN_ABI;
	memset(&draft, 0, sizeof(draft));
	draft.abi = ARM64_LATE_CPU_PLAN_ABI;

	if (!late_profile_active && !late_profile_registration_fault)
		return;

	if (late_profile_registration_fault) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "registration identity changed");
		return;
	}
	/* Pairs with the release stores at the architecture seal point. */
	runtime_state = smp_load_acquire(&late_runtime_evidence_state);
	if (runtime_state != LATE_RUNTIME_EVIDENCE_SEALED) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_POLICY,
				   "runtime evidence was not sealed");
		return;
	}
	if (cpumask_weight(&late_profile_targets) != late_profile.target_count) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "target registrations are incomplete");
		return;
	}

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_REGISTERED) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_REGISTRATION,
				   "invalid prepare state");
		return;
	}

	strscpy(draft.profile_id, late_receipt.profile_id,
		sizeof(draft.profile_id));
	cpumask_copy(&draft.target_cpus, &late_profile_targets);
	ret = late_profile.prepare(&profile_evidence, &late_profile_targets);
	if (profile_evidence.abi != ARM64_LATE_CPU_PLAN_ABI) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_CAP_INVENTORY,
				   "prepare altered evidence ABI");
		return;
	}
	if (!late_profile_runtime_fields_empty(&profile_evidence)) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_RUNTIME_POLICY,
				   "profile supplied runtime observations");
		return;
	}

	draft.evidence = profile_evidence;
	draft.evidence.system_cap = late_runtime_evidence.system_cap;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		draft.evidence.target_policy[target] =
			late_runtime_evidence.target_policy[target];
	plan_ret = arm64_plan_late_cpu_capabilities(&draft, &late_profile);
	effect_ret = plan_ret ? -EAGAIN :
		arm64_plan_late_cpu_effects(&draft, &late_profile);
	hwcap_ret = effect_ret ? -EAGAIN :
		arm64_plan_late_cpu_hwcaps(&draft);
	validate_ret = late_profile.validate_plan(&draft);
	identity_ret = (plan_ret || effect_ret || hwcap_ret ||
			validate_ret) ? -EAGAIN :
		late_profile_finalize_plan_identity(&draft);
	if (plan_ret)
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_CAP_INVENTORY;
	if (effect_ret)
		draft.evidence.blocker_mask |= ARM64_LATE_CPU_BLOCK_EFFECT_PLAN;
	if (hwcap_ret)
		draft.evidence.blocker_mask |= ARM64_LATE_CPU_BLOCK_HWCAP;
	if (identity_ret)
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_SOURCE_IDENTITY;
	if (validate_ret)
		draft.evidence.blocker_mask |=
			ARM64_LATE_CPU_BLOCK_PLAN_VALIDATION;

	if (ret || plan_ret || effect_ret || hwcap_ret || identity_ret ||
	    validate_ret ||
	    draft.evidence.blocker_mask) {
		u64 blockers = draft.evidence.blocker_mask ?:
			       ARM64_LATE_CPU_BLOCK_CAP_INVENTORY;

		late_profile_block(blockers,
				   "required evidence is incomplete");
		return;
	}
	if (!draft.local_caps_planned || !draft.effects_planned ||
	    !draft.hwcaps_planned ||
	    !late_profile_plan_has_identity(&draft)) {
		late_profile_block(ARM64_LATE_CPU_BLOCK_SOURCE_IDENTITY,
				   "canonical plan identity is unavailable");
		return;
	}

	late_plan = draft;
	memcpy(late_receipt.plan_identity, late_plan.identity,
	       sizeof(late_receipt.plan_identity));
	/* Publish the immutable plan before exposing PLAN_FROZEN. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_PLAN_FROZEN);
	pr_info("%s froze an immutable capability plan\n",
		late_receipt.profile_id);
}

void __init arm64_commit_late_cpu_profile(void)
{
	u32 state = READ_ONCE(late_receipt.state);
	int ret;

	if (state == ARM64_LATE_CPU_PROFILE_NONE ||
	    state == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;
	if (state != ARM64_LATE_CPU_PROFILE_PLAN_FROZEN ||
	    !late_plan.local_caps_planned || !late_plan.effects_planned ||
	    !late_plan.hwcaps_planned ||
	    !late_profile_plan_has_identity(&late_plan) ||
	    memcmp(late_receipt.plan_identity, late_plan.identity,
		   sizeof(late_receipt.plan_identity)) ||
	    late_receipt.blocker_mask || late_receipt.commit_complete ||
	    late_receipt.strict_caps_verified ||
	    late_receipt.alternatives_finalized ||
	    late_receipt.user_hwcaps_finalized ||
	    memchr_inv(&late_receipt.committed, 0,
		       sizeof(late_receipt.committed)))
		panic("late CPU profile reached capability commit out of order");

	ret = arm64_commit_late_cpu_plan(&late_plan);
	if (ret)
		panic("late CPU architecture commit failed: %d", ret);
	late_receipt.committed = late_plan.effects;
	late_receipt.commit_complete = 1;
	/* Publish the complete receipt after all architecture state is committed. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_COMMITTED);
	pr_info("%s committed its immutable capability plan\n",
		late_receipt.profile_id);
}

void __init arm64_verify_late_cpu_profile_system(void)
{
	int ret;

	if (READ_ONCE(late_receipt.state) == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_COMMITTED || !late_profile.verify_system)
		panic("late CPU profile reached system finalization out of order");
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt))
		panic("late CPU profile receipt identity changed before verification");

	ret = late_profile.verify_system(&late_plan, &late_receipt);
	if (ret || late_receipt.blocker_mask ||
	    !late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_receipt.commit_complete ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects) ||
	    late_receipt.strict_caps_verified ||
	    late_receipt.alternatives_finalized ||
	    late_receipt.user_hwcaps_finalized)
		panic("late CPU profile system verification failed: %d", ret);

	late_receipt.strict_caps_verified = 1;
	late_receipt.alternatives_finalized = 1;
	/* Pairs with the state load before user verification consumes them. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_SYSTEM_VERIFIED);
}

void __init arm64_finalize_late_cpu_profile_user(void)
{
	int ret;

	if (READ_ONCE(late_receipt.state) == ARM64_LATE_CPU_PROFILE_BLOCKED)
		return;
	if (!late_profile_active)
		return;

	if (READ_ONCE(late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_SYSTEM_VERIFIED ||
	    !late_profile.finalize_user)
		panic("late CPU profile reached user finalization out of order");
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects))
		panic("late CPU profile receipt changed before user finalization");

	ret = late_profile.finalize_user(&late_plan, &late_receipt);
	if (ret || late_receipt.blocker_mask ||
	    !late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects) ||
	    !late_receipt.commit_complete ||
	    !late_receipt.strict_caps_verified ||
	    !late_receipt.alternatives_finalized ||
	    late_receipt.user_hwcaps_finalized)
		panic("late CPU profile user verification failed: %d", ret);

	late_ready_token.abi = ARM64_LATE_CPU_PLAN_ABI;
	strscpy(late_ready_token.profile_id, late_plan.profile_id,
		sizeof(late_ready_token.profile_id));
	cpumask_copy(&late_ready_token.target_cpus, &late_plan.target_cpus);
	memcpy(late_ready_token.plan_identity, late_plan.identity,
	       sizeof(late_ready_token.plan_identity));
	memcpy(late_ready_token.evidence_identity,
	       late_plan.evidence.evidence_identity,
	       sizeof(late_ready_token.evidence_identity));
	memcpy(late_ready_token.target_cpu, late_plan.evidence.target_cpu,
	       sizeof(late_ready_token.target_cpu));
	memcpy(late_ready_token.expected_target_mpidr,
	       late_plan.evidence.expected_target_mpidr,
	       sizeof(late_ready_token.expected_target_mpidr));
	memcpy(late_ready_token.observed_target_mpidr,
	       late_plan.evidence.observed_target_mpidr,
	       sizeof(late_ready_token.observed_target_mpidr));
	memcpy(late_ready_token.expected_target_midr,
	       late_plan.evidence.expected_target_midr,
	       sizeof(late_ready_token.expected_target_midr));
	memcpy(late_ready_token.observed_target_midr,
	       late_plan.evidence.observed_target_midr,
	       sizeof(late_ready_token.observed_target_midr));
	memcpy(late_ready_token.observed_target_revidr,
	       late_plan.evidence.observed_target_revidr,
	       sizeof(late_ready_token.observed_target_revidr));
	late_ready_token.committed = late_receipt.committed;
	if (!late_profile_receipt_identity_matches(&late_plan, &late_receipt) ||
	    !late_profile_effects_match(&late_receipt.committed,
					&late_plan.effects))
		panic("late CPU profile receipt changed before READY publication");
	late_receipt.user_hwcaps_finalized = 1;
	/* Pairs with smp_load_acquire() in the READY-token accessor. */
	smp_store_release(&late_receipt.state,
			  ARM64_LATE_CPU_PROFILE_READY);
	pr_info("%s ready\n", late_receipt.profile_id);
}

bool
arm64_late_cpu_expected_pair_complete(const struct arm64_late_cpu_plan *plan)
{
	const struct arm64_late_cpu_expected_pair *expected =
		&plan->evidence.expected_pair;
	unsigned int target;

	if (expected->abi != ARM64_LATE_CPU_EXPECTED_PAIR_ABI ||
	    expected->target_count != ARM64_LATE_CPU_MAX_TARGETS ||
	    expected->valid != ARM64_LATE_CPU_EXPECTED_PAIR_VALID_MASK ||
	    !memchr_inv(expected->source_identity, 0,
			   sizeof(expected->source_identity)) ||
	    cpumask_weight(&plan->target_cpus) != expected->target_count)
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (!expected->capsule_identity[target] ||
		    expected->mpidr[target] !=
			    plan->evidence.expected_target_mpidr[target] ||
		    (expected->midr & MIDR_CPU_MODEL_MASK) !=
			    plan->evidence.expected_target_midr[target])
			return false;

	return true;
}

int arm64_validate_late_cpu_preflight(unsigned int cpu)
{
	/* Pairs with READY publication of late_plan and late_receipt. */
	if (smp_load_acquire(&late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_READY)
		return 0;
	if (!cpumask_test_cpu(cpu, &late_plan.target_cpus))
		return 0;
	if (cpu != smp_processor_id() ||
	    !arm64_late_cpu_expected_pair_complete(&late_plan))
		return -EINVAL;
	if (!arm64_late_cpu_asid_compatible())
		return -ERANGE;

	return arm64_late_cpu_validate_boot_caps();
}

static bool
late_expected_target_matches(const struct arm64_late_cpu_expected_pair *expected,
			     unsigned int target,
			     const struct cpuinfo_arm64 *info)
{
	const struct cpuinfo_32bit *aarch32 = &info->aarch32;
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;

	return expected->mpidr[target] == mpidr &&
	       expected->midr == info->reg_midr &&
	       expected->revidr == info->reg_revidr &&
	       expected->cntfrq == info->reg_cntfrq &&
	       expected->ctr == read_cpuid_cachetype() &&
	       expected->dczid == info->reg_dczid &&
	       expected->clidr_el1 == read_sysreg(clidr_el1) &&
	       expected->id_aa64dfr0 == info->reg_id_aa64dfr0 &&
	       expected->id_aa64isar0 == info->reg_id_aa64isar0 &&
	       expected->id_aa64isar1 == info->reg_id_aa64isar1 &&
	       expected->id_aa64mmfr0 == info->reg_id_aa64mmfr0 &&
	       expected->id_aa64mmfr1 == info->reg_id_aa64mmfr1 &&
	       expected->id_aa64pfr0 == info->reg_id_aa64pfr0 &&
	       expected->id_aa64pfr1 == info->reg_id_aa64pfr1 &&
	       expected->id_isar0 == aarch32->reg_id_isar0 &&
	       expected->id_isar1 == aarch32->reg_id_isar1 &&
	       expected->id_isar2 == aarch32->reg_id_isar2 &&
	       expected->id_isar3 == aarch32->reg_id_isar3 &&
	       expected->id_isar4 == aarch32->reg_id_isar4 &&
	       expected->id_isar5 == aarch32->reg_id_isar5 &&
	       expected->id_mmfr0 == aarch32->reg_id_mmfr0 &&
	       expected->id_mmfr1 == aarch32->reg_id_mmfr1 &&
	       expected->id_mmfr2 == aarch32->reg_id_mmfr2 &&
	       expected->id_mmfr3 == aarch32->reg_id_mmfr3 &&
	       expected->id_pfr0 == aarch32->reg_id_pfr0 &&
	       expected->id_pfr1 == aarch32->reg_id_pfr1;
}

int arm64_validate_late_cpu_expected_target(unsigned int cpu)
{
	const struct arm64_late_cpu_expected_pair *expected;
	const struct cpuinfo_arm64 *info;
	unsigned int target;

	/* Pairs with READY publication of late_plan and late_receipt. */
	if (smp_load_acquire(&late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_READY)
		return 0;
	if (!cpumask_test_cpu(cpu, &late_plan.target_cpus))
		return 0;
	if (cpu != smp_processor_id() || !arm64_late_cpu_expected_pair_complete(&late_plan))
		return -EINVAL;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (late_plan.evidence.target_cpu[target] == cpu)
			break;
	if (target == ARM64_LATE_CPU_MAX_TARGETS)
		return -EINVAL;

	expected = &late_plan.evidence.expected_pair;
	info = this_cpu_ptr(&cpu_data);
	return late_expected_target_matches(expected, target, info) ? 0 : -ERANGE;
}

const struct arm64_late_cpu_ready_token *
arm64_get_late_cpu_ready_token(void)
{
	/* A36 consumers may observe only the immutable READY token. */
	if (smp_load_acquire(&late_receipt.state) !=
	    ARM64_LATE_CPU_PROFILE_READY)
		return NULL;

	return &late_ready_token;
}
