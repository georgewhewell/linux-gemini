// SPDX-License-Identifier: GPL-2.0-only
/*
 * Observer-stage PSCI gate for the MT6797 Cortex-A72 cluster
 *
 * Copyright (c) 2026 Julien Etienne
 */

#define pr_fmt(fmt) "mt6797-psci: " fmt

#include <linux/arm-smccc.h>
#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/soc/mediatek/mt6797-a72-power.h>
#include <linux/string.h>

#include <asm/cpu_ops.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/late_cpu_profile.h>
#include <asm/mt6797_a72_membership.h>
#include <asm/smp.h>

extern const struct cpu_operations cpu_psci_ops;

#ifdef CONFIG_ARM64_MT6797_A72_CAPABILITY_PROFILE
#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
#define MT6797_A72_EFFECT_BLOCKER	0
#else
#define MT6797_A72_EFFECT_BLOCKER	ARM64_LATE_CPU_BLOCK_EFFECT_PLAN
#endif

#define MT6797_A72_PROFILE_BLOCKERS					\
	(ARM64_LATE_CPU_BLOCK_CONFIGURATION |				\
	 ARM64_LATE_CPU_BLOCK_SOURCE_IDENTITY |			\
	 ARM64_LATE_CPU_BLOCK_FIRMWARE_WA1 |				\
	 ARM64_LATE_CPU_BLOCK_FIRMWARE_WA2 |				\
	 ARM64_LATE_CPU_BLOCK_FIRMWARE_WA3 |				\
	 ARM64_LATE_CPU_BLOCK_ID_REGISTERS |				\
	 ARM64_LATE_CPU_BLOCK_CACHE_TYPE |				\
	 ARM64_LATE_CPU_BLOCK_ASID |					\
	 ARM64_LATE_CPU_BLOCK_GRANULE |				\
	 ARM64_LATE_CPU_BLOCK_VA_MODE |				\
	 ARM64_LATE_CPU_BLOCK_GIC |					\
	 ARM64_LATE_CPU_BLOCK_HWCAP |					\
	 ARM64_LATE_CPU_BLOCK_ATTESTATION_USERS |			\
	 MT6797_A72_EFFECT_BLOCKER |					\
	 ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING |			\
	 ARM64_LATE_CPU_BLOCK_COMMIT_PATH)

static const u64 mt6797_a72_source_parent_identity[ARM64_LATE_CPU_ID_WORDS] __initconst = {
	0x78fcb018e5693cc2, 0x58127ea6e2655319,
	0xf55b80135c1230cb, 0x42fbf70c6d2e6deb,
};

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static const u64 mt6797_a72_config_input_identity[ARM64_LATE_CPU_ID_WORDS] __initconst = {
	0x7b875e34f11c7c6d, 0x007124aacc3e1e01,
	0x3acc41cc1628913a, 0x94cfddf0be8d7a74,
};
#else
static const u64 mt6797_a72_config_input_identity[ARM64_LATE_CPU_ID_WORDS] __initconst = {
	0x699f14786e1d64eb, 0x3811f0b6c481c31d,
	0x9e0e77fc96b64eb4, 0xd12ebbbfde3b23b0,
};
#endif

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static const u64 mt6797_a72_fixture_evidence_identity[ARM64_LATE_CPU_ID_WORDS] __initconst = {
	0x9e93afac8cddf877, 0x1fe76cd27c6818ce,
	0x3e165b1f47a43e8d, 0x1821b7769ce02fad,
};
#endif

static bool __init mt6797_a72_kpti_policy_static(void)
{
#ifdef CONFIG_CMDLINE_FORCE
	return IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0) &&
	       !IS_ENABLED(CONFIG_RANDOMIZE_BASE) &&
	       strstr(CONFIG_CMDLINE, "nokaslr") &&
	       !strstr(CONFIG_CMDLINE, "kpti=") &&
	       !strcmp(saved_command_line, CONFIG_CMDLINE);
#else
	return false;
#endif
}

static bool __init
mt6797_a72_cap_descriptor_shape(const struct arm64_cpu_capabilities *cap,
				const struct arm64_cpu_capabilities *match)
{
	if (!cap || !match || !match->matches || match->match_list)
		return false;
	if (!cap->match_list)
		return cap == match;

	return cap != match && arm64_late_cpu_cap_uses_multi_entry_match(cap);
}

static enum arm64_late_cpu_cap_state __init
mt6797_a72_expected_erratum_state(const struct arm64_cpu_capabilities *match,
				  enum arm64_late_cpu_cap_state expected)
{
	enum arm64_late_cpu_cap_state state;

	state = arm64_late_cpu_erratum_state(match, MIDR_CORTEX_A72);
	return state == expected ? state : ARM64_LATE_CPU_CAP_UNRESOLVED;
}

static enum arm64_late_cpu_cap_state __init
mt6797_a72_classify_local_cap(const struct arm64_cpu_capabilities *cap,
			      const struct arm64_cpu_capabilities *match,
			      const struct arm64_late_cpu_evidence *evidence,
			      unsigned int target)
{
	if (!evidence || target >= ARM64_LATE_CPU_MAX_TARGETS ||
	    evidence->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    arm64_late_cpu_target_impl_override_active() ||
	    evidence->target_cpu[target] != 8 + target ||
	    evidence->expected_target_mpidr[target] != 0x200 + target ||
	    evidence->expected_target_midr[target] != MIDR_CORTEX_A72 ||
	    !mt6797_a72_cap_descriptor_shape(cap, match))
		return ARM64_LATE_CPU_CAP_UNRESOLVED;

	switch (cap->capability) {
	case ARM64_HAS_AMU_EXTN:
	case ARM64_HW_DBM:
		if (cap->type != ARM64_CPUCAP_WEAK_LOCAL_CPU_FEATURE)
			break;
		return arm64_late_cpu_static_feature_state(cap, match,
							   MIDR_CORTEX_A72);
	case ARM64_HAS_BBML2_NOABORT:
		if (cap->type != ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE)
			break;
		return arm64_late_cpu_static_feature_state(cap, match,
							   MIDR_CORTEX_A72);
	case ARM64_HAS_GICV5_LEGACY:
		if (cap->type != ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE)
			break;
		return arm64_late_cpu_gicv5_legacy_state(
			cap, match, &evidence->target_cap[target]);
	case ARM64_HAS_ICH_HCR_EL2_TDIR:
		if (cap->type != ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE)
			break;
		return arm64_late_cpu_ich_hcr_tdir_state(
			cap, match, &evidence->target_cap[target]);
	case ARM64_UNMAP_KERNEL_AT_EL0:
		if (cap->type !=
			    ARM64_CPUCAP_BOOT_RESTRICTED_CPU_LOCAL_FEATURE ||
		    !mt6797_a72_kpti_policy_static())
			break;
		return arm64_late_cpu_static_feature_state(cap, match,
							   MIDR_CORTEX_A72);
	case ARM64_MISMATCHED_CACHE_TYPE:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return arm64_late_cpu_cache_type_state(
			cap, match,
			&evidence->target_cap[target], &evidence->system_cap);
	case ARM64_SPECTRE_V2:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return arm64_late_cpu_a72_spectre_v2_state(
			cap, match, &evidence->target_cap[target]);
	case ARM64_SPECTRE_V4:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return arm64_late_cpu_a72_spectre_v4_state(
			cap, match, &evidence->target_cap[target]);
	case ARM64_SPECTRE_BHB:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return arm64_late_cpu_a72_spectre_bhb_state(
			cap, match, &evidence->target_cap[target]);
	case ARM64_WORKAROUND_1742098:
	case ARM64_WORKAROUND_SPECULATIVE_AT:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return mt6797_a72_expected_erratum_state(match,
						 ARM64_LATE_CPU_CAP_PRESENT);
	case ARM64_WORKAROUND_1418040:
	case ARM64_WORKAROUND_2457168:
		if (cap->type != ARM64_CPUCAP_WEAK_LOCAL_CPU_FEATURE)
			break;
		return mt6797_a72_expected_erratum_state(match,
						 ARM64_LATE_CPU_CAP_ABSENT);
	case ARM64_WORKAROUND_843419:
	case ARM64_WORKAROUND_845719:
	case ARM64_WORKAROUND_858921:
	case ARM64_WORKAROUND_1463225:
	case ARM64_WORKAROUND_1508412:
	case ARM64_WORKAROUND_2077057:
	case ARM64_WORKAROUND_2645198:
	case ARM64_WORKAROUND_2658417:
	case ARM64_WORKAROUND_4193714:
	case ARM64_WORKAROUND_AMPERE_AC03_CPU_38:
	case ARM64_WORKAROUND_AMPERE_AC04_CPU_23:
	case ARM64_WORKAROUND_TSB_FLUSH_FAILURE:
	case ARM64_WORKAROUND_CAVIUM_23154:
	case ARM64_WORKAROUND_CAVIUM_27456:
	case ARM64_WORKAROUND_CAVIUM_30115:
	case ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM:
	case ARM64_WORKAROUND_CAVIUM_TX2_219_TVM:
	case ARM64_WORKAROUND_CLEAN_CACHE:
	case ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE:
	case ARM64_WORKAROUND_NVIDIA_CARMEL_CNP:
	case ARM64_WORKAROUND_PMUV3_IMPDEF_TRAPS:
	case ARM64_WORKAROUND_QCOM_FALKOR_E1003:
	case ARM64_WORKAROUND_QCOM_ORYON_CNTVOFF:
	case ARM64_WORKAROUND_REPEAT_TLBI:
	case ARM64_WORKAROUND_SPECULATIVE_SSBS:
	case ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD:
		if (cap->type != ARM64_CPUCAP_LOCAL_CPU_ERRATUM)
			break;
		return mt6797_a72_expected_erratum_state(match,
						 ARM64_LATE_CPU_CAP_ABSENT);
	default:
		break;
	}

	return ARM64_LATE_CPU_CAP_UNRESOLVED;
}

static const u16 mt6797_a72_compiled_caps[] __initconst = {
	ARM64_HAS_AMU_EXTN,
	ARM64_HAS_GICV5_LEGACY,
	ARM64_HAS_ICH_HCR_EL2_TDIR,
	ARM64_HAS_BBML2_NOABORT,
	ARM64_HW_DBM,
	ARM64_MISMATCHED_CACHE_TYPE,
	ARM64_SPECTRE_V2,
	ARM64_SPECTRE_V4,
	ARM64_SPECTRE_BHB,
	ARM64_UNMAP_KERNEL_AT_EL0,
	ARM64_WORKAROUND_843419,
	ARM64_WORKAROUND_845719,
	ARM64_WORKAROUND_858921,
	ARM64_WORKAROUND_1418040,
	ARM64_WORKAROUND_1463225,
	ARM64_WORKAROUND_1508412,
	ARM64_WORKAROUND_1742098,
	ARM64_WORKAROUND_2077057,
	ARM64_WORKAROUND_2457168,
	ARM64_WORKAROUND_2645198,
	ARM64_WORKAROUND_2658417,
	ARM64_WORKAROUND_4193714,
	ARM64_WORKAROUND_AMPERE_AC03_CPU_38,
	ARM64_WORKAROUND_AMPERE_AC04_CPU_23,
	ARM64_WORKAROUND_TSB_FLUSH_FAILURE,
	ARM64_WORKAROUND_CAVIUM_23154,
	ARM64_WORKAROUND_CAVIUM_27456,
	ARM64_WORKAROUND_CAVIUM_30115,
	ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM,
	ARM64_WORKAROUND_CAVIUM_TX2_219_TVM,
	ARM64_WORKAROUND_CLEAN_CACHE,
	ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
	ARM64_WORKAROUND_NVIDIA_CARMEL_CNP,
	ARM64_WORKAROUND_PMUV3_IMPDEF_TRAPS,
	ARM64_WORKAROUND_QCOM_FALKOR_E1003,
	ARM64_WORKAROUND_QCOM_ORYON_CNTVOFF,
	ARM64_WORKAROUND_REPEAT_TLBI,
	ARM64_WORKAROUND_SPECULATIVE_AT,
	ARM64_WORKAROUND_SPECULATIVE_SSBS,
	ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD,
};

static const u16 mt6797_a72_present_caps[] __initconst = {
	ARM64_HAS_AMU_EXTN,
	ARM64_HW_DBM,
#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	ARM64_MISMATCHED_CACHE_TYPE,
	ARM64_SPECTRE_V2,
	ARM64_SPECTRE_V4,
	ARM64_SPECTRE_BHB,
#endif
	ARM64_WORKAROUND_1742098,
	ARM64_WORKAROUND_SPECULATIVE_AT,
};

static const u16 mt6797_a72_early_caps[] __initconst = {
	ARM64_HAS_AMU_EXTN,
	ARM64_HW_DBM,
};

#ifndef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static const u16 mt6797_a72_unresolved_caps[] __initconst = {
	ARM64_HAS_GICV5_LEGACY,
	ARM64_HAS_ICH_HCR_EL2_TDIR,
	ARM64_MISMATCHED_CACHE_TYPE,
	ARM64_SPECTRE_V2,
	ARM64_SPECTRE_V4,
	ARM64_SPECTRE_BHB,
};
#endif

static const u16 mt6797_a72_absent_caps[] __initconst = {
#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	ARM64_HAS_GICV5_LEGACY,
	ARM64_HAS_ICH_HCR_EL2_TDIR,
#endif
	ARM64_HAS_BBML2_NOABORT,
	ARM64_UNMAP_KERNEL_AT_EL0,
	ARM64_WORKAROUND_843419,
	ARM64_WORKAROUND_845719,
	ARM64_WORKAROUND_858921,
	ARM64_WORKAROUND_1418040,
	ARM64_WORKAROUND_1463225,
	ARM64_WORKAROUND_1508412,
	ARM64_WORKAROUND_2077057,
	ARM64_WORKAROUND_2457168,
	ARM64_WORKAROUND_2645198,
	ARM64_WORKAROUND_2658417,
	ARM64_WORKAROUND_4193714,
	ARM64_WORKAROUND_AMPERE_AC03_CPU_38,
	ARM64_WORKAROUND_AMPERE_AC04_CPU_23,
	ARM64_WORKAROUND_TSB_FLUSH_FAILURE,
	ARM64_WORKAROUND_CAVIUM_23154,
	ARM64_WORKAROUND_CAVIUM_27456,
	ARM64_WORKAROUND_CAVIUM_30115,
	ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM,
	ARM64_WORKAROUND_CAVIUM_TX2_219_TVM,
	ARM64_WORKAROUND_CLEAN_CACHE,
	ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
	ARM64_WORKAROUND_NVIDIA_CARMEL_CNP,
	ARM64_WORKAROUND_PMUV3_IMPDEF_TRAPS,
	ARM64_WORKAROUND_QCOM_FALKOR_E1003,
	ARM64_WORKAROUND_QCOM_ORYON_CNTVOFF,
	ARM64_WORKAROUND_REPEAT_TLBI,
	ARM64_WORKAROUND_SPECULATIVE_SSBS,
	ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD,
};

static const u16 mt6797_a72_required_caps[] __initconst = {
#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	ARM64_MISMATCHED_CACHE_TYPE,
	ARM64_SPECTRE_V2,
	ARM64_SPECTRE_V4,
	ARM64_SPECTRE_BHB,
#endif
	ARM64_WORKAROUND_1742098,
	ARM64_WORKAROUND_SPECULATIVE_AT,
};

static bool __init
mt6797_a72_bitmap_exact(const unsigned long *bitmap, const u16 *caps,
			unsigned int count)
{
	unsigned int i;

	if (bitmap_weight(bitmap, ARM64_NCAPS) != count)
		return false;
	for (i = 0; i < count; i++)
		if (!test_bit(caps[i], bitmap))
			return false;

	return true;
}

static bool __init
mt6797_a72_target_effect_empty(
	const struct arm64_late_cpu_target_effect_plan *effect)
{
	return !effect->valid && !effect->spectre_v2_state &&
	       !effect->spectre_v2_conduit && !effect->spectre_v2_callback &&
	       !effect->spectre_v2_hyp_vector && !effect->spectre_v4_state &&
	       !effect->spectre_v4_method && !effect->spectre_v4_conduit &&
	       !effect->spectre_v4_policy &&
	       !effect->spectre_v4_callback_required && !effect->bhb_method &&
	       !effect->bhb_loop_count && !effect->bhb_matcher_loop_count &&
	       !effect->bhb_conduit && !effect->bhb_mitigation_state &&
	       !effect->bhb_vector_template && !effect->bhb_hyp_vector &&
	       !effect->bhb_v2_non_vulnerable;
}

static bool __init
mt6797_a72_effects_empty(const struct arm64_late_cpu_effect_plan *effects)
{
	unsigned int target;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (!mt6797_a72_target_effect_empty(&effects->target[target]))
			return false;

	return !effects->ctr_mismatch.required &&
	       !effects->ctr_mismatch.target_mask &&
	       !effects->ctr_mismatch.trap_ctr_el0 &&
	       !effects->ctr_mismatch.alternative &&
	       !effects->spectre_v2.required &&
	       !effects->spectre_v2.target_mask &&
	       !effects->spectre_v2.mitigation_state &&
	       !effects->spectre_v2.conduit &&
	       !effects->spectre_v2.callback &&
	       !effects->spectre_v2.hyp_vector &&
	       !effects->spectre_v2.alternative &&
	       !effects->spectre_v4.required &&
	       !effects->spectre_v4.target_mask &&
	       !effects->spectre_v4.mitigation_state &&
	       !effects->spectre_v4.method &&
	       !effects->spectre_v4.conduit &&
	       !effects->spectre_v4.policy &&
	       !effects->spectre_v4.callback_required_mask &&
	       !effects->spectre_v4.firmware_alternative &&
	       !effects->bhb.required && !effects->bhb.target_mask &&
	       !effects->bhb.method && !effects->bhb.loop_count &&
	       !effects->bhb.matcher_loop_count && !effects->bhb.conduit &&
	       !effects->bhb.system_method &&
	       !effects->bhb.mitigation_state &&
	       !effects->bhb.vector_template && !effects->bhb.hyp_vector &&
	       !effects->bhb.alternative &&
	       !effects->bhb.v2_non_vulnerable &&
	       !effects->compat_aes_clear &&
	       !effects->speculative_at_finalization;
}

#ifndef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static bool __init
mt6797_a72_target_policy_empty(
	const struct arm64_late_cpu_target_policy_evidence *policy)
{
	return !policy->valid && !policy->smccc_conduit &&
	       !policy->mitigations_off &&
	       !policy->nospectre_v2 &&
	       !policy->spectre_v4_policy;
}
#endif

static u8 __init
mt6797_a72_target_mask(const struct arm64_late_cpu_plan *plan, int cap)
{
	unsigned int target;
	u8 mask = 0;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (test_bit(cap, plan->target[target].local_caps))
			mask |= BIT(target);

	return mask;
}

static bool __init
mt6797_a72_policy_equal(
	const struct arm64_late_cpu_target_policy_evidence *left,
	const struct arm64_late_cpu_target_policy_evidence *right)
{
	return left->valid == right->valid &&
	       left->smccc_conduit == right->smccc_conduit &&
	       left->mitigations_off == right->mitigations_off &&
	       left->nospectre_v2 == right->nospectre_v2 &&
	       left->spectre_v4_policy == right->spectre_v4_policy;
}

static bool __init
mt6797_a72_effect_state_matches_cap(
	const struct arm64_late_cpu_plan *plan, unsigned int target, int cap,
	u8 state)
{
	bool present;

	if (!test_bit(cap, plan->target[target].classified_local_caps))
		return false;
	present = test_bit(cap, plan->target[target].local_caps);

	switch (cap) {
	case ARM64_SPECTRE_V2:
	case ARM64_SPECTRE_V4:
		if (state == ARM64_LATE_CPU_MITIGATION_UNAFFECTED)
			return !present;
		return present &&
		       (state == ARM64_LATE_CPU_MITIGATION_MITIGATED ||
			state == ARM64_LATE_CPU_MITIGATION_VULNERABLE);
	case ARM64_SPECTRE_BHB:
		if (state == ARM64_LATE_CPU_BHB_STATE_UNAFFECTED)
			return !present;
		return present &&
		       (state == ARM64_LATE_CPU_BHB_STATE_MITIGATED ||
			state == ARM64_LATE_CPU_BHB_STATE_VULNERABLE);
	default:
		return false;
	}
}

static bool __init
mt6797_a72_v2_effect_equal(
	const struct arm64_late_cpu_target_effect_plan *left,
	const struct arm64_late_cpu_target_effect_plan *right)
{
	return left->spectre_v2_state == right->spectre_v2_state &&
	       left->spectre_v2_conduit == right->spectre_v2_conduit &&
	       left->spectre_v2_callback == right->spectre_v2_callback &&
	       left->spectre_v2_hyp_vector == right->spectre_v2_hyp_vector;
}

static bool __init
mt6797_a72_v4_effect_equal(
	const struct arm64_late_cpu_target_effect_plan *left,
	const struct arm64_late_cpu_target_effect_plan *right)
{
	return left->spectre_v4_state == right->spectre_v4_state &&
	       left->spectre_v4_method == right->spectre_v4_method &&
	       left->spectre_v4_conduit == right->spectre_v4_conduit &&
	       left->spectre_v4_policy == right->spectre_v4_policy;
}

static bool __init
mt6797_a72_bhb_effect_equal(
	const struct arm64_late_cpu_target_effect_plan *left,
	const struct arm64_late_cpu_target_effect_plan *right)
{
	return left->bhb_method == right->bhb_method &&
	       left->bhb_loop_count == right->bhb_loop_count &&
	       left->bhb_conduit == right->bhb_conduit &&
	       left->bhb_mitigation_state == right->bhb_mitigation_state &&
	       left->bhb_vector_template == right->bhb_vector_template &&
	       left->bhb_hyp_vector == right->bhb_hyp_vector &&
	       left->bhb_v2_non_vulnerable == right->bhb_v2_non_vulnerable;
}

static int __init mt6797_a72_bhb_system_method(u8 method, u8 *system_method)
{
	switch (method) {
	case ARM64_LATE_CPU_BHB_NONE:
		*system_method = 0;
		return 0;
	case ARM64_LATE_CPU_BHB_LOOP:
		*system_method = BIT(0);
		return 0;
	case ARM64_LATE_CPU_BHB_FIRMWARE:
		*system_method = BIT(1);
		return 0;
	case ARM64_LATE_CPU_BHB_HARDWARE:
		*system_method = BIT(2);
		return 0;
	case ARM64_LATE_CPU_BHB_INSTRUCTION:
		*system_method = BIT(3);
		return 0;
	default:
		return -EINVAL;
	}
}

static int __init
mt6797_a72_derive_effects(const struct arm64_late_cpu_plan *plan,
			  struct arm64_late_cpu_effect_plan *effects)
{
	const struct arm64_late_cpu_target_policy_evidence *policy;
	const struct arm64_late_cpu_target_effect_plan *first;
	u8 system_v2_state;
	u8 ctr_mask;
	u8 v2_mask;
	u8 v4_mask;
	u8 bhb_mask;
	u8 late_bhb_system_method;
	unsigned int target;
	int first_target;
	int ret;

	if (!plan || !effects || plan->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    !plan->local_caps_planned || plan->effects_planned ||
	    !mt6797_a72_effects_empty(effects) ||
	    arm64_late_cpu_target_impl_override_active())
		return -EINVAL;
	if ((plan->evidence.system_cap.valid &
	     (ARM64_LATE_CPU_SYSTEM_CAP_SSBS_VALID |
	      ARM64_LATE_CPU_SYSTEM_CAP_EFFECTS_VALID)) !=
		    (ARM64_LATE_CPU_SYSTEM_CAP_SSBS_VALID |
		     ARM64_LATE_CPU_SYSTEM_CAP_EFFECTS_VALID) ||
	    plan->evidence.system_cap.valid &
		    ~ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK ||
	    plan->evidence.system_cap.ssbs > 1)
		return -EAGAIN;
	if (plan->evidence.system_cap.spectre_v2_state !=
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    plan->evidence.system_cap.spectre_v4_state !=
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    plan->evidence.system_cap.bhb_state !=
		    ARM64_LATE_CPU_BHB_STATE_UNAFFECTED ||
	    plan->evidence.system_cap.bhb_matcher_loop_count ||
	    plan->evidence.system_cap.bhb_system_method)
		return -EOPNOTSUPP;
	if (!mt6797_a72_policy_equal(&plan->evidence.target_policy[0],
				     &plan->evidence.target_policy[1]))
		return -EOPNOTSUPP;

	policy = &plan->evidence.target_policy[0];
	system_v2_state = plan->evidence.system_cap.spectre_v2_state;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		ret = arm64_late_cpu_a72_spectre_v2_v4_effects(
			&plan->evidence.target_cap[target],
			&plan->evidence.target_policy[target],
			&effects->target[target]);
		if (ret)
			return ret;
		if (!mt6797_a72_effect_state_matches_cap(
			    plan, target, ARM64_SPECTRE_V2,
			    effects->target[target].spectre_v2_state) ||
		    !mt6797_a72_effect_state_matches_cap(
			    plan, target, ARM64_SPECTRE_V4,
			    effects->target[target].spectre_v4_state))
			return -EINVAL;
		if (effects->target[target].spectre_v2_state > system_v2_state)
			system_v2_state =
				effects->target[target].spectre_v2_state;
	}

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		ret = arm64_late_cpu_a72_bhb_effect(
			&plan->evidence.target_cap[target],
			&plan->evidence.target_policy[target], system_v2_state,
			&effects->target[target]);
		if (ret)
			return ret;
		if (!mt6797_a72_effect_state_matches_cap(
			    plan, target, ARM64_SPECTRE_BHB,
			    effects->target[target].bhb_mitigation_state))
			return -EINVAL;
	}

	ctr_mask = mt6797_a72_target_mask(plan, ARM64_MISMATCHED_CACHE_TYPE);
	v2_mask = mt6797_a72_target_mask(plan, ARM64_SPECTRE_V2);
	v4_mask = mt6797_a72_target_mask(plan, ARM64_SPECTRE_V4);
	bhb_mask = mt6797_a72_target_mask(plan, ARM64_SPECTRE_BHB);
	effects->ctr_mismatch.required = !!ctr_mask;
	effects->ctr_mismatch.target_mask = ctr_mask;
	if (ctr_mask) {
		effects->ctr_mismatch.trap_ctr_el0 = 1;
		effects->ctr_mismatch.alternative = 1;
	}

	effects->spectre_v2.required = !!v2_mask;
	effects->spectre_v2.target_mask = v2_mask;
	first_target = -1;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (!(v2_mask & BIT(target)))
			continue;
		if (first_target < 0) {
			first_target = target;
			continue;
		}
		if (!mt6797_a72_v2_effect_equal(
			    &effects->target[first_target],
			    &effects->target[target]))
			return -EOPNOTSUPP;
	}
	if (first_target >= 0) {
		first = &effects->target[first_target];
		effects->spectre_v2.mitigation_state = first->spectre_v2_state;
		effects->spectre_v2.conduit = first->spectre_v2_conduit;
		effects->spectre_v2.callback = first->spectre_v2_callback;
		effects->spectre_v2.hyp_vector = first->spectre_v2_hyp_vector;
		effects->spectre_v2.alternative = 1;
	}

	effects->spectre_v4.required = !!v4_mask;
	effects->spectre_v4.target_mask = v4_mask;
	first_target = -1;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (effects->target[target].spectre_v4_callback_required)
			effects->spectre_v4.callback_required_mask |= BIT(target);
		if (!(v4_mask & BIT(target)))
			continue;
		if (first_target < 0) {
			first_target = target;
			continue;
		}
		if (!mt6797_a72_v4_effect_equal(
			    &effects->target[first_target],
			    &effects->target[target]))
			return -EOPNOTSUPP;
	}
	if (first_target >= 0) {
		first = &effects->target[first_target];
		effects->spectre_v4.mitigation_state = first->spectre_v4_state;
		effects->spectre_v4.method = first->spectre_v4_method;
		effects->spectre_v4.conduit = first->spectre_v4_conduit;
		effects->spectre_v4.policy = first->spectre_v4_policy;
		effects->spectre_v4.firmware_alternative =
			!plan->evidence.system_cap.ssbs &&
			!policy->mitigations_off &&
			policy->spectre_v4_policy ==
				ARM64_LATE_CPU_V4_POLICY_DYNAMIC;
	}

	effects->bhb.required = !!bhb_mask;
	effects->bhb.target_mask = bhb_mask;
	effects->bhb.matcher_loop_count =
		plan->evidence.system_cap.bhb_matcher_loop_count;
	effects->bhb.system_method =
		plan->evidence.system_cap.bhb_system_method;
	first_target = -1;
	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (!(bhb_mask & BIT(target)))
			continue;
		if (effects->target[target].bhb_matcher_loop_count >
		    effects->bhb.matcher_loop_count)
			effects->bhb.matcher_loop_count =
				effects->target[target].bhb_matcher_loop_count;
		if (first_target < 0) {
			first_target = target;
			continue;
		}
		if (!mt6797_a72_bhb_effect_equal(
			    &effects->target[first_target],
			    &effects->target[target]))
			return -EOPNOTSUPP;
	}
	if (first_target >= 0) {
		first = &effects->target[first_target];
		effects->bhb.method = first->bhb_method;
		effects->bhb.loop_count = first->bhb_loop_count;
		effects->bhb.conduit = first->bhb_conduit;
		effects->bhb.mitigation_state = first->bhb_mitigation_state;
		effects->bhb.vector_template = first->bhb_vector_template;
		effects->bhb.hyp_vector = first->bhb_hyp_vector;
		effects->bhb.v2_non_vulnerable = first->bhb_v2_non_vulnerable;
		effects->bhb.alternative = 1;
		ret = mt6797_a72_bhb_system_method(
			first->bhb_method, &late_bhb_system_method);
		if (ret)
			return ret;
		effects->bhb.system_method |= late_bhb_system_method;
	}

	effects->compat_aes_clear =
		test_bit(ARM64_WORKAROUND_1742098, plan->target_local_caps);
	effects->speculative_at_finalization =
		test_bit(ARM64_WORKAROUND_SPECULATIVE_AT,
			 plan->target_local_caps);

	return 0;
}

static bool __init
mt6797_a72_identity_empty(const u64 identity[ARM64_LATE_CPU_ID_WORDS])
{
	unsigned int i;

	for (i = 0; i < ARM64_LATE_CPU_ID_WORDS; i++)
		if (identity[i])
			return false;

	return true;
}

#ifndef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static bool __init
mt6797_a72_binding_empty(const struct arm64_late_cpu_runtime_binding *binding)
{
	return !binding->valid && !binding->origin &&
	       mt6797_a72_identity_empty(binding->expected_config_identity) &&
	       mt6797_a72_identity_empty(binding->running_config_identity) &&
	       mt6797_a72_identity_empty(binding->expected_build_id_identity) &&
	       mt6797_a72_identity_empty(binding->running_build_id_identity) &&
	       mt6797_a72_identity_empty(binding->expected_cmdline_identity) &&
	       mt6797_a72_identity_empty(binding->running_cmdline_identity);
}

static bool __init
mt6797_a72_binding_is_runtime(const struct arm64_late_cpu_runtime_binding *binding)
{
	return binding->valid == ARM64_LATE_CPU_BIND_VALID_MASK &&
	       binding->origin == ARM64_LATE_CPU_BINDING_RUNTIME &&
	       !mt6797_a72_identity_empty(binding->expected_config_identity) &&
	       !mt6797_a72_identity_empty(binding->running_config_identity) &&
	       !mt6797_a72_identity_empty(binding->expected_build_id_identity) &&
	       !mt6797_a72_identity_empty(binding->running_build_id_identity) &&
	       !mt6797_a72_identity_empty(binding->expected_cmdline_identity) &&
	       !mt6797_a72_identity_empty(binding->running_cmdline_identity) &&
	       !memcmp(binding->expected_config_identity,
		       binding->running_config_identity,
		       sizeof(binding->expected_config_identity)) &&
	       !memcmp(binding->expected_build_id_identity,
		       binding->running_build_id_identity,
		       sizeof(binding->expected_build_id_identity)) &&
	       !memcmp(binding->expected_cmdline_identity,
		       binding->running_cmdline_identity,
		       sizeof(binding->expected_cmdline_identity));
}

static bool __init
mt6797_a72_evidence_is_expected_only(const struct arm64_late_cpu_evidence *evidence)
{
	u64 required_blockers = MT6797_A72_PROFILE_BLOCKERS;
	bool runtime_bound;
	unsigned int i;

	runtime_bound = mt6797_a72_binding_is_runtime(&evidence->binding);
	if (runtime_bound)
		required_blockers &= ~ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING;

	if (evidence->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    memcmp(evidence->source_parent_identity,
		   mt6797_a72_source_parent_identity,
		   sizeof(evidence->source_parent_identity)) ||
	    memcmp(evidence->config_input_identity,
		   mt6797_a72_config_input_identity,
		   sizeof(evidence->config_input_identity)) ||
	    (evidence->blocker_mask & required_blockers) !=
		    required_blockers ||
	    evidence->blocker_mask &
		    ~(required_blockers |
		      ARM64_LATE_CPU_BLOCK_TOPOLOGY) ||
	    evidence->expected_target_mpidr[0] != 0x200 ||
	    evidence->expected_target_mpidr[1] != 0x201 ||
	    evidence->expected_target_midr[0] != MIDR_CORTEX_A72 ||
	    evidence->expected_target_midr[1] != MIDR_CORTEX_A72 ||
	    evidence->target_cpu[0] != 8 || evidence->target_cpu[1] != 9 ||
	    (!runtime_bound &&
	     !mt6797_a72_binding_empty(&evidence->binding)))
		return false;

	for (i = 0; i < ARM64_LATE_CPU_MAX_TARGETS; i++)
		if (evidence->observed_target_mpidr[i] ||
		    evidence->observed_target_midr[i] ||
		    evidence->observed_target_revidr[i] ||
		    evidence->target_cap[i].valid ||
		    !mt6797_a72_target_policy_empty(&evidence->target_policy[i]))
			return false;
	if (!mt6797_a72_identity_empty(evidence->evidence_identity))
		return false;

	return !evidence->system_cap.valid;
}
#endif

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static bool __init
mt6797_a72_binding_is_fixture(
	const struct arm64_late_cpu_runtime_binding *binding)
{
	return !binding->valid &&
	       binding->origin == ARM64_LATE_CPU_BINDING_FIXTURE &&
	       mt6797_a72_identity_empty(binding->expected_config_identity) &&
	       mt6797_a72_identity_empty(binding->running_config_identity) &&
	       mt6797_a72_identity_empty(binding->expected_build_id_identity) &&
	       mt6797_a72_identity_empty(binding->running_build_id_identity) &&
	       mt6797_a72_identity_empty(binding->expected_cmdline_identity) &&
	       mt6797_a72_identity_empty(binding->running_cmdline_identity);
}

static bool __init
mt6797_a72_fixture_registers_exact(
	const struct arm64_late_cpu_register_image *registers)
{
	const struct arm64_late_cpu_aarch32_register_image *aarch32 =
		&registers->aarch32;

	return registers->ctr == 0x83338003 && !registers->cntfrq &&
	       !registers->dczid && registers->midr == MIDR_CORTEX_A72 &&
	       !registers->revidr && !registers->aidr && !registers->gmid &&
	       !registers->smidr && !registers->mpamidr &&
	       !registers->id_aa64dfr0 && !registers->id_aa64dfr1 &&
	       !registers->id_aa64isar0 && !registers->id_aa64isar1 &&
	       !registers->id_aa64isar2 && !registers->id_aa64isar3 &&
	       !registers->id_aa64mmfr0 && !registers->id_aa64mmfr1 &&
	       !registers->id_aa64mmfr2 && !registers->id_aa64mmfr3 &&
	       !registers->id_aa64mmfr4 && !registers->id_aa64pfr0 &&
	       !registers->id_aa64pfr1 && !registers->id_aa64pfr2 &&
	       !registers->id_aa64zfr0 && !registers->id_aa64smfr0 &&
	       !registers->id_aa64fpfr0 && !aarch32->id_dfr0 &&
	       !aarch32->id_dfr1 && !aarch32->id_isar0 &&
	       !aarch32->id_isar1 && !aarch32->id_isar2 &&
	       !aarch32->id_isar3 && !aarch32->id_isar4 &&
	       !aarch32->id_isar5 && !aarch32->id_isar6 &&
	       !aarch32->id_mmfr0 && !aarch32->id_mmfr1 &&
	       !aarch32->id_mmfr2 && !aarch32->id_mmfr3 &&
	       !aarch32->id_mmfr4 && !aarch32->id_mmfr5 &&
	       !aarch32->id_pfr0 && !aarch32->id_pfr1 &&
	       !aarch32->id_pfr2 && !aarch32->mvfr0 && !aarch32->mvfr1 &&
	       !aarch32->mvfr2;
}

static bool __init
mt6797_a72_fixture_target_exact(
	const struct arm64_late_cpu_target_cap_evidence *target)
{
	u32 valid = ARM64_LATE_CPU_TARGET_CAP_MIDR_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_ID_REGS_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_CTR_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_GIC_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_HYP_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_WA1_VALID |
		    ARM64_LATE_CPU_TARGET_CAP_WA2_VALID;

	return target->valid == valid &&
	       mt6797_a72_fixture_registers_exact(&target->registers) &&
	       !target->clidr_el1 && target->ctr_effective == 0x93338003 &&
	       !target->icc_sre_el1 && !target->icc_idr0_el1 &&
	       !target->ich_vtr_el2 && !target->ich_vtr_status &&
	       target->smccc_wa1 == SMCCC_RET_SUCCESS &&
	       target->smccc_wa2 == SMCCC_RET_SUCCESS && !target->smccc_wa3 &&
	       !target->asid_bits && !target->page_shift && !target->va_bits &&
	       target->hyp_available == 1 && !target->kernel_in_hyp_mode &&
	       !target->gic_sre_usable &&
	       target->ich_vtr_source == ARM64_LATE_CPU_ICH_VTR_NONE;
}

static bool __init
mt6797_a72_fixture_policy_exact(
	const struct arm64_late_cpu_target_policy_evidence *policy)
{
	return policy->valid == ARM64_LATE_CPU_TARGET_POLICY_VALID_MASK &&
	       policy->smccc_conduit == ARM64_LATE_CPU_SMCCC_SMC &&
	       !policy->mitigations_off && !policy->nospectre_v2 &&
	       policy->spectre_v4_policy ==
		       ARM64_LATE_CPU_V4_POLICY_DYNAMIC;
}

static bool __init
mt6797_a72_evidence_is_fixture(
	const struct arm64_late_cpu_evidence *evidence)
{
	unsigned int target;

	if (evidence->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    memcmp(evidence->source_parent_identity,
		   mt6797_a72_source_parent_identity,
		   sizeof(evidence->source_parent_identity)) ||
	    memcmp(evidence->config_input_identity,
		   mt6797_a72_config_input_identity,
		   sizeof(evidence->config_input_identity)) ||
	    memcmp(evidence->evidence_identity,
		   mt6797_a72_fixture_evidence_identity,
		   sizeof(evidence->evidence_identity)) ||
	    !mt6797_a72_binding_is_fixture(&evidence->binding) ||
	    (evidence->blocker_mask & MT6797_A72_PROFILE_BLOCKERS) !=
		    MT6797_A72_PROFILE_BLOCKERS ||
	    evidence->blocker_mask &
		    ~(MT6797_A72_PROFILE_BLOCKERS |
		      ARM64_LATE_CPU_BLOCK_TOPOLOGY) ||
	    evidence->system_cap.valid !=
		    ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK ||
	    evidence->system_cap.ctr_sys_val != 0xb4448004 ||
	    evidence->system_cap.ctr_strict_mask != ~GENMASK_ULL(15, 14) ||
	    evidence->system_cap.ssbs ||
	    evidence->system_cap.spectre_v2_state !=
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    evidence->system_cap.spectre_v4_state !=
		    ARM64_LATE_CPU_MITIGATION_UNAFFECTED ||
	    evidence->system_cap.bhb_state !=
		    ARM64_LATE_CPU_BHB_STATE_UNAFFECTED ||
	    evidence->system_cap.bhb_matcher_loop_count ||
	    evidence->system_cap.bhb_system_method)
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (evidence->target_cpu[target] != 8 + target ||
		    evidence->expected_target_mpidr[target] != 0x200 + target ||
		    evidence->observed_target_mpidr[target] != 0x200 + target ||
		    evidence->expected_target_midr[target] != MIDR_CORTEX_A72 ||
		    evidence->observed_target_midr[target] != MIDR_CORTEX_A72 ||
		    evidence->observed_target_revidr[target] ||
		    !mt6797_a72_fixture_target_exact(
			    &evidence->target_cap[target]) ||
		    !mt6797_a72_fixture_policy_exact(
			    &evidence->target_policy[target]))
			return false;

	return true;
}

static bool __init
mt6797_a72_fixture_target_effect_exact(
	const struct arm64_late_cpu_target_effect_plan *effect)
{
	return effect->valid == ARM64_LATE_CPU_TARGET_EFFECT_VALID_MASK &&
	       effect->spectre_v2_state ==
		       ARM64_LATE_CPU_MITIGATION_MITIGATED &&
	       effect->spectre_v2_conduit == ARM64_LATE_CPU_SMCCC_SMC &&
	       effect->spectre_v2_callback ==
		       ARM64_LATE_CPU_V2_CALLBACK_SMC &&
	       effect->spectre_v2_hyp_vector ==
		       ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_DIRECT &&
	       effect->spectre_v4_state ==
		       ARM64_LATE_CPU_MITIGATION_MITIGATED &&
	       effect->spectre_v4_method == ARM64_LATE_CPU_V4_FIRMWARE &&
	       effect->spectre_v4_conduit == ARM64_LATE_CPU_SMCCC_SMC &&
	       effect->spectre_v4_policy ==
		       ARM64_LATE_CPU_V4_POLICY_DYNAMIC &&
	       effect->spectre_v4_callback_required == 1 &&
	       effect->bhb_method == ARM64_LATE_CPU_BHB_LOOP &&
	       effect->bhb_loop_count == 8 &&
	       effect->bhb_matcher_loop_count == 8 &&
	       effect->bhb_conduit == ARM64_LATE_CPU_SMCCC_NONE &&
	       effect->bhb_mitigation_state ==
		       ARM64_LATE_CPU_BHB_STATE_MITIGATED &&
	       effect->bhb_vector_template == ARM64_LATE_CPU_BHB_VECTOR_LOOP &&
	       effect->bhb_hyp_vector ==
		       ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_DIRECT &&
	       effect->bhb_v2_non_vulnerable == 1;
}

static bool __init
mt6797_a72_fixture_effects_exact(
	const struct arm64_late_cpu_effect_plan *effects)
{
	unsigned int target;

	if (effects->ctr_mismatch.required != 1 ||
	    effects->ctr_mismatch.target_mask != 0x3 ||
	    effects->ctr_mismatch.trap_ctr_el0 != 1 ||
	    effects->ctr_mismatch.alternative != 1 ||
	    effects->spectre_v2.required != 1 ||
	    effects->spectre_v2.target_mask != 0x3 ||
	    effects->spectre_v2.mitigation_state !=
		    ARM64_LATE_CPU_MITIGATION_MITIGATED ||
	    effects->spectre_v2.conduit != ARM64_LATE_CPU_SMCCC_SMC ||
	    effects->spectre_v2.callback != ARM64_LATE_CPU_V2_CALLBACK_SMC ||
	    effects->spectre_v2.hyp_vector !=
		    ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_DIRECT ||
	    effects->spectre_v2.alternative != 1 ||
	    effects->spectre_v4.required != 1 ||
	    effects->spectre_v4.target_mask != 0x3 ||
	    effects->spectre_v4.mitigation_state !=
		    ARM64_LATE_CPU_MITIGATION_MITIGATED ||
	    effects->spectre_v4.method != ARM64_LATE_CPU_V4_FIRMWARE ||
	    effects->spectre_v4.conduit != ARM64_LATE_CPU_SMCCC_SMC ||
	    effects->spectre_v4.policy != ARM64_LATE_CPU_V4_POLICY_DYNAMIC ||
	    effects->spectre_v4.callback_required_mask != 0x3 ||
	    effects->spectre_v4.firmware_alternative != 1 ||
	    effects->bhb.required != 1 || effects->bhb.target_mask != 0x3 ||
	    effects->bhb.method != ARM64_LATE_CPU_BHB_LOOP ||
	    effects->bhb.loop_count != 8 ||
	    effects->bhb.matcher_loop_count != 8 ||
	    effects->bhb.conduit != ARM64_LATE_CPU_SMCCC_NONE ||
	    effects->bhb.system_method != BIT(0) ||
	    effects->bhb.mitigation_state !=
		    ARM64_LATE_CPU_BHB_STATE_MITIGATED ||
	    effects->bhb.vector_template != ARM64_LATE_CPU_BHB_VECTOR_LOOP ||
	    effects->bhb.hyp_vector !=
		    ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_DIRECT ||
	    effects->bhb.alternative != 1 ||
	    effects->bhb.v2_non_vulnerable != 1 ||
	    effects->compat_aes_clear != 1 ||
	    effects->speculative_at_finalization != 1)
		return false;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++)
		if (!mt6797_a72_fixture_target_effect_exact(
			    &effects->target[target]))
			return false;

	return true;
}
#endif

static int __init
mt6797_a72_validate_cap_plan(const struct arm64_late_cpu_plan *plan)
{
	unsigned int target;
	unsigned int i;

	if (!plan || plan->abi != ARM64_LATE_CPU_PLAN_ABI ||
	    strcmp(plan->profile_id, "mt6797-a53-a72-a41-v7") ||
	    cpumask_weight(&plan->target_cpus) != 2 ||
	    !cpumask_test_cpu(8, &plan->target_cpus) ||
	    !cpumask_test_cpu(9, &plan->target_cpus) ||
	    !mt6797_a72_bitmap_exact(plan->compiled_local_caps,
				     mt6797_a72_compiled_caps,
				     ARRAY_SIZE(mt6797_a72_compiled_caps)) ||
	    !mt6797_a72_bitmap_exact(plan->early_local_caps,
				     mt6797_a72_early_caps,
				     ARRAY_SIZE(mt6797_a72_early_caps)) ||
	    !mt6797_a72_bitmap_exact(plan->target_local_caps,
				     mt6797_a72_present_caps,
				     ARRAY_SIZE(mt6797_a72_present_caps)) ||
	    !mt6797_a72_bitmap_exact(plan->required_local_caps,
				     mt6797_a72_required_caps,
				     ARRAY_SIZE(mt6797_a72_required_caps)) ||
	    !bitmap_empty(plan->conflicting_local_caps, ARM64_NCAPS) ||
	    bitmap_weight(plan->classified_local_caps, ARM64_NCAPS) !=
		ARRAY_SIZE(mt6797_a72_present_caps) +
		ARRAY_SIZE(mt6797_a72_absent_caps) ||
	    plan->expected_compat_hwcap || plan->expected_compat_hwcap2)
		return -EINVAL;

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	if (!plan->local_caps_planned || !plan->effects_planned ||
	    !mt6797_a72_evidence_is_fixture(&plan->evidence) ||
	    !mt6797_a72_bitmap_exact(plan->classified_local_caps,
				     mt6797_a72_compiled_caps,
				     ARRAY_SIZE(mt6797_a72_compiled_caps)) ||
	    !mt6797_a72_fixture_effects_exact(&plan->effects))
		return -EINVAL;
#else
	if (plan->local_caps_planned || plan->effects_planned ||
	    !mt6797_a72_evidence_is_expected_only(&plan->evidence) ||
	    !mt6797_a72_effects_empty(&plan->effects))
		return -EINVAL;
#endif

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		if (bitmap_weight(plan->target[target].classified_local_caps,
				  ARM64_NCAPS) !=
			    ARRAY_SIZE(mt6797_a72_present_caps) +
			    ARRAY_SIZE(mt6797_a72_absent_caps) ||
		    !mt6797_a72_bitmap_exact(plan->target[target].local_caps,
			    mt6797_a72_present_caps,
			    ARRAY_SIZE(mt6797_a72_present_caps)) ||
		    !bitmap_subset(plan->target[target].local_caps,
				   plan->target[target].classified_local_caps,
				   ARM64_NCAPS))
			return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(mt6797_a72_present_caps); i++)
			if (!test_bit(mt6797_a72_present_caps[i],
				      plan->target[target].local_caps))
				return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(mt6797_a72_absent_caps); i++)
			if (!test_bit(mt6797_a72_absent_caps[i],
				      plan->target[target].classified_local_caps) ||
			    test_bit(mt6797_a72_absent_caps[i],
				     plan->target[target].local_caps))
				return -EINVAL;
#ifndef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
		for (i = 0; i < ARRAY_SIZE(mt6797_a72_unresolved_caps); i++)
			if (test_bit(mt6797_a72_unresolved_caps[i],
				     plan->target[target].classified_local_caps) ||
			    test_bit(mt6797_a72_unresolved_caps[i],
				     plan->target[target].local_caps))
				return -EINVAL;
#endif
	}

	for (i = 0; i < ARRAY_SIZE(mt6797_a72_present_caps); i++)
		if (!test_bit(mt6797_a72_present_caps[i],
			      plan->classified_local_caps))
			return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(mt6797_a72_absent_caps); i++)
		if (!test_bit(mt6797_a72_absent_caps[i],
			      plan->classified_local_caps))
			return -EINVAL;
#ifndef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	for (i = 0; i < ARRAY_SIZE(mt6797_a72_unresolved_caps); i++)
		if (test_bit(mt6797_a72_unresolved_caps[i],
			     plan->classified_local_caps) ||
		    test_bit(mt6797_a72_unresolved_caps[i],
			     plan->target_local_caps))
			return -EINVAL;
#endif
	for (i = 0; i < ARRAY_SIZE(plan->expected_elf_hwcap); i++)
		if (plan->expected_elf_hwcap[i])
			return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(plan->identity); i++)
		if (plan->identity[i])
			return -EINVAL;

	/* Source-only fixture/expected evidence never publishes an identity. */
	return -EAGAIN;
}

static bool __init mt6797_a72_profile_config_gates_match(void)
{
	return IS_ENABLED(CONFIG_SMP) &&
	       IS_ENABLED(CONFIG_HOTPLUG_CPU) &&
	       IS_ENABLED(CONFIG_ARM64_AMU_EXTN) &&
	       IS_ENABLED(CONFIG_ARM64_HW_AFDBM) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_843419) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_845719) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_858921) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_1418040) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_1463225) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_1508412) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_1742098) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_2077057) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_2457168) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_2645198) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_2658417) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_4193714) &&
	       IS_ENABLED(CONFIG_AMPERE_ERRATUM_AC03_CPU_38) &&
	       IS_ENABLED(CONFIG_AMPERE_ERRATUM_AC04_CPU_23) &&
	       IS_ENABLED(CONFIG_ARM64_WORKAROUND_TSB_FLUSH_FAILURE) &&
	       IS_ENABLED(CONFIG_CAVIUM_ERRATUM_23154) &&
	       IS_ENABLED(CONFIG_CAVIUM_ERRATUM_27456) &&
	       IS_ENABLED(CONFIG_CAVIUM_ERRATUM_30115) &&
	       IS_ENABLED(CONFIG_CAVIUM_TX2_ERRATUM_219) &&
	       IS_ENABLED(CONFIG_ARM64_WORKAROUND_CLEAN_CACHE) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_832075) &&
	       IS_ENABLED(CONFIG_NVIDIA_CARMEL_CNP_ERRATUM) &&
	       IS_ENABLED(CONFIG_QCOM_FALKOR_ERRATUM_1003) &&
	       IS_ENABLED(CONFIG_ARM64_WORKAROUND_REPEAT_TLBI) &&
	       IS_ENABLED(CONFIG_ARM64_WORKAROUND_SPECULATIVE_AT) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_3194386) &&
	       IS_ENABLED(CONFIG_ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD) &&
	       IS_ENABLED(CONFIG_ARM64_ERRATUM_1319367) &&
	       IS_ENABLED(CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY) &&
	       IS_ENABLED(CONFIG_COMPAT) &&
	       IS_ENABLED(CONFIG_ARM64_4K_PAGES) &&
	       IS_ENABLED(CONFIG_ARM64_VA_BITS_52) &&
	       IS_ENABLED(CONFIG_ARM64_LPA2) &&
	       IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0) &&
	       IS_ENABLED(CONFIG_ARM_GIC_V3) &&
	       mt6797_a72_kpti_policy_static() &&
	       !IS_ENABLED(CONFIG_RANDOMIZE_BASE) &&
	       !IS_ENABLED(CONFIG_VIRTUALIZATION) &&
	       !IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) &&
	       !strstr(saved_command_line, "nospectre_v2") &&
	       !strstr(saved_command_line, "nospectre_bhb") &&
	       !strstr(saved_command_line, "ssbd=") &&
	       !strstr(saved_command_line, "mitigations=off") &&
	       !strstr(saved_command_line, "allow_mismatched_32bit_el0") &&
	       !arm64_late_cpu_target_impl_override_active();
}

static bool __init
mt6797_a72_profile_topology_exact(const struct cpumask *registered_targets)
{
	static const u64 expected_mpidr[] = {
		0x000, 0x001, 0x002, 0x003, 0x100,
		0x101, 0x102, 0x103, 0x200, 0x201,
	};
	struct device_node *dn;
	const char *actual_enable_method;
	const char *expected_compatible;
	const char *expected_enable_method;
	unsigned int cpu;
	bool valid = true;

	if (!of_machine_is_compatible("planet,gemini-pda") ||
	    !of_machine_is_compatible("mediatek,mt6797") ||
	    num_possible_cpus() != ARRAY_SIZE(expected_mpidr) ||
	    num_online_cpus() != 8 ||
	    cpumask_weight(registered_targets) != 2 ||
	    !cpumask_test_cpu(8, registered_targets) ||
	    !cpumask_test_cpu(9, registered_targets))
		return false;

	for_each_possible_cpu(cpu) {
		if (cpu >= ARRAY_SIZE(expected_mpidr) ||
		    cpu_logical_map(cpu) != expected_mpidr[cpu] ||
		    (cpu < 8 && !cpu_online(cpu)) ||
		    (cpu >= 8 && cpu_online(cpu)))
			return false;

		dn = of_get_cpu_node(cpu, NULL);
		if (!dn)
			return false;

		expected_compatible = cpu < 8 ?
			"arm,cortex-a53" : "arm,cortex-a72";
		expected_enable_method = cpu < 8 ?
			"psci" : "mediatek,mt6797-psci";
		if (!of_device_is_compatible(dn, expected_compatible) ||
		    of_get_cpu_hwid(dn, 0) != expected_mpidr[cpu] ||
		    of_property_read_string(dn, "enable-method",
					    &actual_enable_method) ||
		    strcmp(actual_enable_method, expected_enable_method))
			valid = false;
		of_node_put(dn);

		if (!valid)
			return false;
	}

	return true;
}

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
static void __init
mt6797_a72_populate_fixture(struct arm64_late_cpu_evidence *evidence)
{
	struct arm64_late_cpu_target_cap_evidence *target_cap;
	struct arm64_late_cpu_target_policy_evidence *policy;
	unsigned int target;

	evidence->binding.origin = ARM64_LATE_CPU_BINDING_FIXTURE;
	memcpy(evidence->evidence_identity,
	       mt6797_a72_fixture_evidence_identity,
	       sizeof(evidence->evidence_identity));
	evidence->system_cap.valid = ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK;
	evidence->system_cap.ctr_sys_val = 0xb4448004;
	evidence->system_cap.ctr_strict_mask = ~GENMASK_ULL(15, 14);
	evidence->system_cap.ssbs = 0;
	evidence->system_cap.spectre_v2_state =
		ARM64_LATE_CPU_MITIGATION_UNAFFECTED;
	evidence->system_cap.spectre_v4_state =
		ARM64_LATE_CPU_MITIGATION_UNAFFECTED;
	evidence->system_cap.bhb_state = ARM64_LATE_CPU_BHB_STATE_UNAFFECTED;
	evidence->system_cap.bhb_matcher_loop_count = 0;
	evidence->system_cap.bhb_system_method = 0;

	for (target = 0; target < ARM64_LATE_CPU_MAX_TARGETS; target++) {
		evidence->observed_target_mpidr[target] = 0x200 + target;
		evidence->observed_target_midr[target] = MIDR_CORTEX_A72;
		evidence->observed_target_revidr[target] = 0;

		target_cap = &evidence->target_cap[target];
		target_cap->valid = ARM64_LATE_CPU_TARGET_CAP_MIDR_VALID |
			ARM64_LATE_CPU_TARGET_CAP_ID_REGS_VALID |
			ARM64_LATE_CPU_TARGET_CAP_CTR_VALID |
			ARM64_LATE_CPU_TARGET_CAP_GIC_VALID |
			ARM64_LATE_CPU_TARGET_CAP_HYP_VALID |
			ARM64_LATE_CPU_TARGET_CAP_WA1_VALID |
			ARM64_LATE_CPU_TARGET_CAP_WA2_VALID;
		target_cap->registers.midr = MIDR_CORTEX_A72;
		target_cap->registers.ctr = 0x83338003;
		target_cap->registers.id_aa64pfr0 = 0;
		target_cap->registers.id_aa64pfr1 = 0;
		target_cap->registers.id_aa64pfr2 = 0;
		target_cap->registers.id_aa64isar2 = 0;
		target_cap->registers.id_aa64mmfr1 = 0;
		target_cap->clidr_el1 = 0;
		target_cap->ctr_effective = 0x93338003;
		target_cap->icc_sre_el1 = 0;
		target_cap->icc_idr0_el1 = 0;
		target_cap->ich_vtr_el2 = 0;
		target_cap->ich_vtr_status = 0;
		target_cap->smccc_wa1 = SMCCC_RET_SUCCESS;
		target_cap->smccc_wa2 = SMCCC_RET_SUCCESS;
		target_cap->smccc_wa3 = 0;
		target_cap->hyp_available = 1;
		target_cap->kernel_in_hyp_mode = 0;
		target_cap->gic_sre_usable = 0;
		target_cap->ich_vtr_source = ARM64_LATE_CPU_ICH_VTR_NONE;

		policy = &evidence->target_policy[target];
		policy->valid = ARM64_LATE_CPU_TARGET_POLICY_VALID_MASK;
		policy->smccc_conduit = ARM64_LATE_CPU_SMCCC_SMC;
		policy->mitigations_off = 0;
		policy->nospectre_v2 = 0;
		policy->spectre_v4_policy =
			ARM64_LATE_CPU_V4_POLICY_DYNAMIC;
	}
}
#endif

static int __init
mt6797_a72_profile_prepare(struct arm64_late_cpu_evidence *evidence,
			   const struct cpumask *registered_targets)
{
	memcpy(evidence->source_parent_identity,
	       mt6797_a72_source_parent_identity,
	       sizeof(evidence->source_parent_identity));
	memcpy(evidence->config_input_identity,
	       mt6797_a72_config_input_identity,
	       sizeof(evidence->config_input_identity));
	evidence->target_cpu[0] = 8;
	evidence->target_cpu[1] = 9;
	evidence->expected_target_mpidr[0] = 0x200;
	evidence->expected_target_mpidr[1] = 0x201;
	evidence->expected_target_midr[0] = MIDR_CORTEX_A72;
	evidence->expected_target_midr[1] = MIDR_CORTEX_A72;
	evidence->blocker_mask = MT6797_A72_PROFILE_BLOCKERS;

#ifdef CONFIG_ARM64_MT6797_A72_FIXTURE_EVIDENCE
	mt6797_a72_populate_fixture(evidence);
#endif

	if (!mt6797_a72_profile_config_gates_match())
		evidence->blocker_mask |= ARM64_LATE_CPU_BLOCK_CONFIGURATION;
	if (!mt6797_a72_profile_topology_exact(registered_targets))
		evidence->blocker_mask |= ARM64_LATE_CPU_BLOCK_TOPOLOGY;

	/* No live system capability, alternative, vector, or HWCAP is changed. */
	return -EAGAIN;
}

static const struct arm64_late_cpu_profile mt6797_a72_profile __initconst = {
	.name = "mt6797-a53-a72-a41-v7",
	.target_count = 2,
	.classify_local_cap = mt6797_a72_classify_local_cap,
	.validate_plan = mt6797_a72_validate_cap_plan,
	.derive_effects = mt6797_a72_derive_effects,
	.prepare = mt6797_a72_profile_prepare,
};

void __init mt6797_activate_a72_capability_profile(void)
{
	arm64_activate_late_cpu_profile(&mt6797_a72_profile);
}
#endif

static int __init mt6797_psci_cpu_init(unsigned int cpu)
{
	int ret;

	ret = cpu_psci_ops.cpu_init(cpu);
#ifdef CONFIG_ARM64_MT6797_A72_CAPABILITY_PROFILE
	if (!ret)
		arm64_register_late_cpu_target(&mt6797_a72_profile, cpu);
#endif
	return ret;
}

static int __init mt6797_psci_cpu_prepare(unsigned int cpu)
{
	return cpu_psci_ops.cpu_prepare(cpu);
}

#ifdef CONFIG_ARM64_MT6797_A72_P24_ADMISSION_HOOKS
static bool mt6797_psci_is_a72(unsigned int cpu)
{
	return cpu == 8 || cpu == 9;
}

static int mt6797_psci_cpu_up_preflight(unsigned int cpu,
					enum cpuhp_state target)
{
	if (!mt6797_psci_is_a72(cpu))
		return 0;

	return mt6797_a72_membership_preflight_up(cpu, target);
}

static int mt6797_psci_cpu_up_validate(unsigned int cpu, int tasks_frozen,
				       enum cpuhp_state target)
{
	if (!mt6797_psci_is_a72(cpu))
		return 0;

	return mt6797_a72_membership_validate_up(cpu, tasks_frozen, target);
}
#endif

#ifdef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK
static int mt6797_psci_cpu_up_rollback(unsigned int cpu,
				       enum cpuhp_state state, int error,
				       const struct cpu_up_rollback_trace *trace)
{
	if (cpu != 8 && cpu != 9)
		return 0;

	return mt6797_a72_membership_publish_p32(cpu, state, error, trace);
}

#ifdef CONFIG_HOTPLUG_CPU
static int mt6797_psci_cpu_disable(unsigned int cpu)
{
	int ret;

	ret = mt6797_a72_membership_p32_cpu_disable(cpu);
	if (ret)
		return ret;

	return cpu_psci_ops.cpu_disable ? cpu_psci_ops.cpu_disable(cpu) :
		-EOPNOTSUPP;
}

static void mt6797_psci_cpu_die(unsigned int cpu)
{
	if (mt6797_a72_membership_p32_cpu_die(cpu))
		cpu_park_loop();

	cpu_psci_ops.cpu_die(cpu);
}

static int mt6797_psci_cpu_kill(unsigned int cpu)
{
	int ret;

	ret = mt6797_a72_membership_p32_cpu_kill(cpu);
	if (ret)
		return ret;

	return cpu_psci_ops.cpu_kill ? cpu_psci_ops.cpu_kill(cpu) : 0;
}
#endif
#endif

static int mt6797_psci_cpu_boot(unsigned int cpu)
{
	int ret;

	ret = mt6797_a72_power_cpu_boot_admit(cpu);
	if (ret) {
		pr_warn_ratelimited("CPU%u boot rejected by A72 power owner: %d\n",
				    cpu, ret);
		return ret;
	}

	return cpu_psci_ops.cpu_boot(cpu);
}

#ifdef CONFIG_HOTPLUG_CPU
static bool mt6797_psci_cpu_can_disable(unsigned int cpu)
{
	return false;
}
#endif

const struct cpu_operations mt6797_psci_ops = {
	.name		= "mediatek,mt6797-psci",
	.cpu_init	= mt6797_psci_cpu_init,
	.cpu_prepare	= mt6797_psci_cpu_prepare,
#ifdef CONFIG_ARM64_MT6797_A72_P24_ADMISSION_HOOKS
	.cpu_up_preflight = mt6797_psci_cpu_up_preflight,
	.cpu_up_validate = mt6797_psci_cpu_up_validate,
#endif
#ifdef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK
	.cpu_up_rollback = mt6797_psci_cpu_up_rollback,
#endif
	.cpu_boot	= mt6797_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = mt6797_psci_cpu_can_disable,
#ifdef CONFIG_ARM64_MT6797_A72_P32_ROLLBACK
	.cpu_disable = mt6797_psci_cpu_disable,
	.cpu_die = mt6797_psci_cpu_die,
	.cpu_kill = mt6797_psci_cpu_kill,
#endif
#endif
};
