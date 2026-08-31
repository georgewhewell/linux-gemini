/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_LATE_CPU_PROFILE_H
#define __ASM_LATE_CPU_PROFILE_H

#include <linux/bitmap.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/types.h>

#include <asm/cpucaps.h>

#define ARM64_LATE_CPU_PLAN_ABI		7
#define ARM64_LATE_CPU_PROFILE_ID_LEN	32
#define ARM64_LATE_CPU_ID_WORDS		4
#define ARM64_LATE_CPU_MAX_TARGETS	2

enum arm64_late_cpu_profile_state {
	ARM64_LATE_CPU_PROFILE_NONE,
	ARM64_LATE_CPU_PROFILE_REGISTERED,
	ARM64_LATE_CPU_PROFILE_BLOCKED,
	ARM64_LATE_CPU_PROFILE_PLAN_FROZEN,
	ARM64_LATE_CPU_PROFILE_COMMITTED,
	ARM64_LATE_CPU_PROFILE_SYSTEM_VERIFIED,
	ARM64_LATE_CPU_PROFILE_READY,
};

enum arm64_late_cpu_bhb_method {
	ARM64_LATE_CPU_BHB_UNRESOLVED,
	ARM64_LATE_CPU_BHB_NONE,
	ARM64_LATE_CPU_BHB_LOOP,
	ARM64_LATE_CPU_BHB_FIRMWARE,
	ARM64_LATE_CPU_BHB_INSTRUCTION,
	ARM64_LATE_CPU_BHB_HARDWARE,
};

enum arm64_late_cpu_cap_state {
	ARM64_LATE_CPU_CAP_UNRESOLVED,
	ARM64_LATE_CPU_CAP_ABSENT,
	ARM64_LATE_CPU_CAP_PRESENT,
};

/* Every unresolved proof keeps the production profile blocked. */
#define ARM64_LATE_CPU_BLOCK_REGISTRATION	BIT_ULL(0)
#define ARM64_LATE_CPU_BLOCK_TOPOLOGY		BIT_ULL(1)
#define ARM64_LATE_CPU_BLOCK_CONFIGURATION	BIT_ULL(2)
#define ARM64_LATE_CPU_BLOCK_CAP_INVENTORY	BIT_ULL(3)
#define ARM64_LATE_CPU_BLOCK_FIRMWARE_WA1	BIT_ULL(4)
#define ARM64_LATE_CPU_BLOCK_FIRMWARE_WA2	BIT_ULL(5)
#define ARM64_LATE_CPU_BLOCK_ID_REGISTERS	BIT_ULL(6)
#define ARM64_LATE_CPU_BLOCK_CACHE_TYPE		BIT_ULL(7)
#define ARM64_LATE_CPU_BLOCK_ASID		BIT_ULL(8)
#define ARM64_LATE_CPU_BLOCK_GRANULE		BIT_ULL(9)
#define ARM64_LATE_CPU_BLOCK_VA_MODE		BIT_ULL(10)
#define ARM64_LATE_CPU_BLOCK_GIC			BIT_ULL(11)
#define ARM64_LATE_CPU_BLOCK_HWCAP		BIT_ULL(12)
#define ARM64_LATE_CPU_BLOCK_ATTESTATION_USERS	BIT_ULL(13)
#define ARM64_LATE_CPU_BLOCK_SOURCE_IDENTITY	BIT_ULL(14)
#define ARM64_LATE_CPU_BLOCK_FIRMWARE_WA3	BIT_ULL(15)
#define ARM64_LATE_CPU_BLOCK_EFFECT_PLAN		BIT_ULL(16)
#define ARM64_LATE_CPU_BLOCK_PLAN_VALIDATION	BIT_ULL(17)
#define ARM64_LATE_CPU_BLOCK_RUNTIME_BINDING	BIT_ULL(18)
#define ARM64_LATE_CPU_BLOCK_COMMIT_PATH		BIT_ULL(19)

enum arm64_late_cpu_mitigation_state {
	ARM64_LATE_CPU_MITIGATION_UNRESOLVED,
	ARM64_LATE_CPU_MITIGATION_UNAFFECTED,
	ARM64_LATE_CPU_MITIGATION_MITIGATED,
	ARM64_LATE_CPU_MITIGATION_VULNERABLE,
};

enum arm64_late_cpu_smccc_conduit {
	ARM64_LATE_CPU_SMCCC_UNRESOLVED,
	ARM64_LATE_CPU_SMCCC_NONE,
	ARM64_LATE_CPU_SMCCC_SMC,
	ARM64_LATE_CPU_SMCCC_HVC,
};

enum arm64_late_cpu_v2_callback {
	ARM64_LATE_CPU_V2_CALLBACK_UNRESOLVED,
	ARM64_LATE_CPU_V2_CALLBACK_NONE,
	ARM64_LATE_CPU_V2_CALLBACK_SMC,
	ARM64_LATE_CPU_V2_CALLBACK_HVC,
	ARM64_LATE_CPU_V2_CALLBACK_QCOM_LINK_STACK,
};

enum arm64_late_cpu_v4_method {
	ARM64_LATE_CPU_V4_UNRESOLVED,
	ARM64_LATE_CPU_V4_NONE,
	ARM64_LATE_CPU_V4_SSBS,
	ARM64_LATE_CPU_V4_FIRMWARE,
};

enum arm64_late_cpu_v4_policy {
	ARM64_LATE_CPU_V4_POLICY_UNRESOLVED,
	ARM64_LATE_CPU_V4_POLICY_DYNAMIC,
	ARM64_LATE_CPU_V4_POLICY_FORCE_ON,
	ARM64_LATE_CPU_V4_POLICY_FORCE_OFF,
};

enum arm64_late_cpu_bhb_mitigation_state {
	ARM64_LATE_CPU_BHB_STATE_UNRESOLVED,
	ARM64_LATE_CPU_BHB_STATE_UNAFFECTED,
	ARM64_LATE_CPU_BHB_STATE_MITIGATED,
	ARM64_LATE_CPU_BHB_STATE_VULNERABLE,
};

enum arm64_late_cpu_bhb_vector_template {
	ARM64_LATE_CPU_BHB_VECTOR_UNRESOLVED,
	ARM64_LATE_CPU_BHB_VECTOR_NONE,
	ARM64_LATE_CPU_BHB_VECTOR_LOOP,
	ARM64_LATE_CPU_BHB_VECTOR_CLEAR_INSN,
	ARM64_LATE_CPU_BHB_VECTOR_FIRMWARE,
};

enum arm64_late_cpu_hyp_vector {
	ARM64_LATE_CPU_HYP_VECTOR_UNRESOLVED,
	ARM64_LATE_CPU_HYP_VECTOR_DIRECT,
	ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_DIRECT,
	ARM64_LATE_CPU_HYP_VECTOR_INDIRECT,
	ARM64_LATE_CPU_HYP_VECTOR_SPECTRE_INDIRECT,
};

enum arm64_late_cpu_ich_vtr_source {
	ARM64_LATE_CPU_ICH_VTR_UNRESOLVED,
	ARM64_LATE_CPU_ICH_VTR_NONE,
	ARM64_LATE_CPU_ICH_VTR_DIRECT,
	ARM64_LATE_CPU_ICH_VTR_HVC,
};

#define ARM64_LATE_CPU_TARGET_EFFECT_V2_VALID	BIT(0)
#define ARM64_LATE_CPU_TARGET_EFFECT_V4_VALID	BIT(1)
#define ARM64_LATE_CPU_TARGET_EFFECT_BHB_VALID	BIT(2)
#define ARM64_LATE_CPU_TARGET_EFFECT_VALID_MASK			\
	(ARM64_LATE_CPU_TARGET_EFFECT_V2_VALID |			\
	 ARM64_LATE_CPU_TARGET_EFFECT_V4_VALID |			\
	 ARM64_LATE_CPU_TARGET_EFFECT_BHB_VALID)

/* Pure evaluator output retained separately for every target. */
struct arm64_late_cpu_target_effect_plan {
	u32 valid;
	u8 spectre_v2_state;
	u8 spectre_v2_conduit;
	u8 spectre_v2_callback;
	u8 spectre_v2_hyp_vector;
	u8 spectre_v4_state;
	u8 spectre_v4_method;
	u8 spectre_v4_conduit;
	u8 spectre_v4_policy;
	u8 spectre_v4_callback_required;
	u8 bhb_method;
	u8 bhb_loop_count;
	u8 bhb_matcher_loop_count;
	u8 bhb_conduit;
	u8 bhb_mitigation_state;
	u8 bhb_vector_template;
	u8 bhb_hyp_vector;
	u8 bhb_v2_non_vulnerable;
};

/* Typed decisions; no field authorizes a live mutation by itself. */
struct arm64_late_cpu_effect_plan {
	struct {
		u8 required;
		u8 target_mask;
		u8 trap_ctr_el0;
		u8 alternative;
	} ctr_mismatch;
	struct {
		u8 required;
		u8 target_mask;
		u8 mitigation_state;
		u8 conduit;
		u8 callback;
		u8 hyp_vector;
		u8 alternative;
	} spectre_v2;
	struct {
		u8 required;
		u8 target_mask;
		u8 mitigation_state;
		u8 method;
		u8 conduit;
		u8 policy;
		u8 callback_required_mask;
		u8 firmware_alternative;
	} spectre_v4;
	struct {
		u8 required;
		u8 target_mask;
		u8 method;
		u8 loop_count;
		u8 matcher_loop_count;
		u8 conduit;
		u8 system_method;
		u8 mitigation_state;
		u8 vector_template;
		u8 hyp_vector;
		u8 alternative;
		u8 v2_non_vulnerable;
	} bhb;
	struct arm64_late_cpu_target_effect_plan
		target[ARM64_LATE_CPU_MAX_TARGETS];
	u8 compat_aes_clear;
	u8 speculative_at_finalization;
};

/* Per-target capability-state evidence validity. */
#define ARM64_LATE_CPU_TARGET_CAP_MIDR_VALID	BIT(0)
#define ARM64_LATE_CPU_TARGET_CAP_ID_REGS_VALID	BIT(1)
#define ARM64_LATE_CPU_TARGET_CAP_CTR_VALID	BIT(2)
#define ARM64_LATE_CPU_TARGET_CAP_GIC_VALID	BIT(3)
#define ARM64_LATE_CPU_TARGET_CAP_HYP_VALID	BIT(4)
#define ARM64_LATE_CPU_TARGET_CAP_WA1_VALID	BIT(5)
#define ARM64_LATE_CPU_TARGET_CAP_WA2_VALID	BIT(6)
#define ARM64_LATE_CPU_TARGET_CAP_WA3_VALID	BIT(7)
#define ARM64_LATE_CPU_TARGET_CAP_ASID_VALID	BIT(8)
#define ARM64_LATE_CPU_TARGET_CAP_GRANULE_VALID	BIT(9)
#define ARM64_LATE_CPU_TARGET_CAP_VA_VALID	BIT(10)
#define ARM64_LATE_CPU_TARGET_CAP_VALID_MASK				\
	(ARM64_LATE_CPU_TARGET_CAP_MIDR_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_ID_REGS_VALID |			\
	 ARM64_LATE_CPU_TARGET_CAP_CTR_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_GIC_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_HYP_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_WA1_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_WA2_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_WA3_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_ASID_VALID |				\
	 ARM64_LATE_CPU_TARGET_CAP_GRANULE_VALID |			\
	 ARM64_LATE_CPU_TARGET_CAP_VA_VALID)

/* Plain register fields; canonical identity must serialize fields, not padding. */
struct arm64_late_cpu_aarch32_register_image {
	u32 id_dfr0;
	u32 id_dfr1;
	u32 id_isar0;
	u32 id_isar1;
	u32 id_isar2;
	u32 id_isar3;
	u32 id_isar4;
	u32 id_isar5;
	u32 id_isar6;
	u32 id_mmfr0;
	u32 id_mmfr1;
	u32 id_mmfr2;
	u32 id_mmfr3;
	u32 id_mmfr4;
	u32 id_mmfr5;
	u32 id_pfr0;
	u32 id_pfr1;
	u32 id_pfr2;
	u32 mvfr0;
	u32 mvfr1;
	u32 mvfr2;
};

struct arm64_late_cpu_register_image {
	u64 ctr;
	u64 cntfrq;
	u64 dczid;
	u64 midr;
	u64 revidr;
	u64 aidr;
	u64 gmid;
	u64 smidr;
	u64 mpamidr;
	u64 id_aa64dfr0;
	u64 id_aa64dfr1;
	u64 id_aa64isar0;
	u64 id_aa64isar1;
	u64 id_aa64isar2;
	u64 id_aa64isar3;
	u64 id_aa64mmfr0;
	u64 id_aa64mmfr1;
	u64 id_aa64mmfr2;
	u64 id_aa64mmfr3;
	u64 id_aa64mmfr4;
	u64 id_aa64pfr0;
	u64 id_aa64pfr1;
	u64 id_aa64pfr2;
	u64 id_aa64zfr0;
	u64 id_aa64smfr0;
	u64 id_aa64fpfr0;
	struct arm64_late_cpu_aarch32_register_image aarch32;
};

#define ARM64_LATE_CPU_EXPECTED_PAIR_ABI	1

enum arm64_late_cpu_expected_pair_field {
	ARM64_LATE_CPU_EXPECT_SOURCE_IDENTITY,
	ARM64_LATE_CPU_EXPECT_CAPSULE_IDENTITY,
	ARM64_LATE_CPU_EXPECT_MPIDR,
	ARM64_LATE_CPU_EXPECT_MIDR,
	ARM64_LATE_CPU_EXPECT_REVIDR,
	ARM64_LATE_CPU_EXPECT_CNTFRQ,
	ARM64_LATE_CPU_EXPECT_CTR,
	ARM64_LATE_CPU_EXPECT_DCZID,
	ARM64_LATE_CPU_EXPECT_CLIDR,
	ARM64_LATE_CPU_EXPECT_AA64DFR0,
	ARM64_LATE_CPU_EXPECT_AA64ISAR0,
	ARM64_LATE_CPU_EXPECT_AA64ISAR1,
	ARM64_LATE_CPU_EXPECT_AA64MMFR0,
	ARM64_LATE_CPU_EXPECT_AA64MMFR1,
	ARM64_LATE_CPU_EXPECT_AA64PFR0,
	ARM64_LATE_CPU_EXPECT_AA64PFR1,
	ARM64_LATE_CPU_EXPECT_A32ISAR0,
	ARM64_LATE_CPU_EXPECT_A32ISAR1,
	ARM64_LATE_CPU_EXPECT_A32ISAR2,
	ARM64_LATE_CPU_EXPECT_A32ISAR3,
	ARM64_LATE_CPU_EXPECT_A32ISAR4,
	ARM64_LATE_CPU_EXPECT_A32ISAR5,
	ARM64_LATE_CPU_EXPECT_A32MMFR0,
	ARM64_LATE_CPU_EXPECT_A32MMFR1,
	ARM64_LATE_CPU_EXPECT_A32MMFR2,
	ARM64_LATE_CPU_EXPECT_A32MMFR3,
	ARM64_LATE_CPU_EXPECT_A32PFR0,
	ARM64_LATE_CPU_EXPECT_A32PFR1,
	ARM64_LATE_CPU_EXPECT_FIELD_COUNT,
};

#define ARM64_LATE_CPU_EXPECTED_PAIR_VALID_MASK				\
	GENMASK_ULL(ARM64_LATE_CPU_EXPECT_FIELD_COUNT - 1, 0)

/* Prior-cycle expectation; no field is a current-boot observation. */
struct arm64_late_cpu_expected_pair {
	u32 abi;
	u32 target_count;
	u64 valid;
	u64 source_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 capsule_identity[ARM64_LATE_CPU_MAX_TARGETS];
	u64 mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u64 midr;
	u64 revidr;
	u64 cntfrq;
	u64 ctr;
	u64 dczid;
	u64 clidr_el1;
	u64 id_aa64dfr0;
	u64 id_aa64isar0;
	u64 id_aa64isar1;
	u64 id_aa64mmfr0;
	u64 id_aa64mmfr1;
	u64 id_aa64pfr0;
	u64 id_aa64pfr1;
	u32 id_isar0;
	u32 id_isar1;
	u32 id_isar2;
	u32 id_isar3;
	u32 id_isar4;
	u32 id_isar5;
	u32 id_mmfr0;
	u32 id_mmfr1;
	u32 id_mmfr2;
	u32 id_mmfr3;
	u32 id_pfr0;
	u32 id_pfr1;
};

struct arm64_late_cpu_target_cap_evidence {
	u32 valid;
	struct arm64_late_cpu_register_image registers;
	u64 clidr_el1;
	u64 ctr_effective;
	u64 icc_sre_el1;
	u64 icc_idr0_el1;
	u64 ich_vtr_el2;
	s32 ich_vtr_status;
	s32 smccc_wa1;
	s32 smccc_wa2;
	s32 smccc_wa3;
	u16 asid_bits;
	u8 page_shift;
	u8 va_bits;
	u8 hyp_available;
	u8 kernel_in_hyp_mode;
	u8 gic_sre_usable;
	u8 ich_vtr_source;
};

#define ARM64_LATE_CPU_SYSTEM_CAP_CTR_VALID	BIT(0)
#define ARM64_LATE_CPU_SYSTEM_CAP_SSBS_VALID	BIT(1)
#define ARM64_LATE_CPU_SYSTEM_CAP_EFFECTS_VALID	BIT(2)
#define ARM64_LATE_CPU_SYSTEM_CAP_EARLY_LOCAL_VALID	BIT(3)
#define ARM64_LATE_CPU_SYSTEM_CAP_VALID_MASK				\
	(ARM64_LATE_CPU_SYSTEM_CAP_CTR_VALID |				\
	 ARM64_LATE_CPU_SYSTEM_CAP_SSBS_VALID |				\
	 ARM64_LATE_CPU_SYSTEM_CAP_EFFECTS_VALID |			\
	 ARM64_LATE_CPU_SYSTEM_CAP_EARLY_LOCAL_VALID)

struct arm64_late_cpu_system_cap_evidence {
	u32 valid;
	u64 ctr_sys_val;
	u64 ctr_strict_mask;
	u8 ssbs;
	u8 spectre_v2_state;
	u8 spectre_v4_state;
	u8 bhb_state;
	u8 bhb_matcher_loop_count;
	u8 bhb_system_method;
	u8 gicv5_legacy;
	u8 ich_hcr_tdir;
};

enum arm64_late_cpu_binding_origin {
	ARM64_LATE_CPU_BINDING_NONE,
	ARM64_LATE_CPU_BINDING_FIXTURE,
	ARM64_LATE_CPU_BINDING_RUNTIME,
};

#define ARM64_LATE_CPU_BIND_EXPECTED_CONFIG_VALID	BIT(0)
#define ARM64_LATE_CPU_BIND_RUNNING_CONFIG_VALID	BIT(1)
#define ARM64_LATE_CPU_BIND_EXPECTED_BUILD_ID_VALID	BIT(2)
#define ARM64_LATE_CPU_BIND_RUNNING_BUILD_ID_VALID	BIT(3)
#define ARM64_LATE_CPU_BIND_EXPECTED_CMDLINE_VALID	BIT(4)
#define ARM64_LATE_CPU_BIND_RUNNING_CMDLINE_VALID	BIT(5)
#define ARM64_LATE_CPU_BIND_VALID_MASK				\
	(ARM64_LATE_CPU_BIND_EXPECTED_CONFIG_VALID |		\
	 ARM64_LATE_CPU_BIND_RUNNING_CONFIG_VALID |		\
	 ARM64_LATE_CPU_BIND_EXPECTED_BUILD_ID_VALID |		\
	 ARM64_LATE_CPU_BIND_RUNNING_BUILD_ID_VALID |		\
	 ARM64_LATE_CPU_BIND_EXPECTED_CMDLINE_VALID |		\
	 ARM64_LATE_CPU_BIND_RUNNING_CMDLINE_VALID)

struct arm64_late_cpu_runtime_binding {
	u32 valid;
	u32 origin;
	u64 expected_config_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 running_config_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 expected_build_id_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 running_build_id_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 expected_cmdline_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 running_cmdline_identity[ARM64_LATE_CPU_ID_WORDS];
};

/* Immutable policy inputs; method choices are derived, never supplied. */
#define ARM64_LATE_CPU_TARGET_POLICY_CONDUIT_VALID	BIT(0)
#define ARM64_LATE_CPU_TARGET_POLICY_V2_VALID		BIT(1)
#define ARM64_LATE_CPU_TARGET_POLICY_V4_VALID		BIT(2)
#define ARM64_LATE_CPU_TARGET_POLICY_VALID_MASK			\
	(ARM64_LATE_CPU_TARGET_POLICY_CONDUIT_VALID |		\
	 ARM64_LATE_CPU_TARGET_POLICY_V2_VALID |			\
	 ARM64_LATE_CPU_TARGET_POLICY_V4_VALID)

struct arm64_late_cpu_target_policy_evidence {
	u32 valid;
	u8 smccc_conduit;
	u8 mitigations_off;
	u8 nospectre_v2;
	u8 spectre_v4_policy;
};

/*
 * Core-sealed runtime observations or an explicit profile fixture. A partial
 * record is never published as a plan.
 */
struct arm64_late_cpu_evidence {
	u32 abi;
	/* Non-circular inputs; neither field proves the running image/config. */
	u64 source_parent_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 config_input_identity[ARM64_LATE_CPU_ID_WORDS];
	struct arm64_late_cpu_expected_pair expected_pair;
	struct arm64_late_cpu_runtime_binding binding;
	u64 evidence_identity[ARM64_LATE_CPU_ID_WORDS];
	u32 target_cpu[ARM64_LATE_CPU_MAX_TARGETS];
	u64 expected_target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u64 observed_target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 expected_target_midr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 observed_target_midr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 observed_target_revidr[ARM64_LATE_CPU_MAX_TARGETS];
	struct arm64_late_cpu_target_cap_evidence
		target_cap[ARM64_LATE_CPU_MAX_TARGETS];
	struct arm64_late_cpu_target_policy_evidence
		target_policy[ARM64_LATE_CPU_MAX_TARGETS];
	struct arm64_late_cpu_system_cap_evidence system_cap;
	u64 blocker_mask;
};

/* Per-target results preserve disagreement and future typed-effect masks. */
struct arm64_late_cpu_target_plan {
	DECLARE_BITMAP(classified_local_caps, ARM64_NCAPS);
	DECLARE_BITMAP(local_caps, ARM64_NCAPS);
};

/* Core-normalized and single-copy published only after complete validation. */
struct arm64_late_cpu_plan {
	u32 abi;
	char profile_id[ARM64_LATE_CPU_PROFILE_ID_LEN];
	cpumask_t target_cpus;
	struct arm64_late_cpu_evidence evidence;
	/* Canonical, core-owned read-only planning result. */
	DECLARE_BITMAP(canonical_caps, ARM64_NCAPS);
	DECLARE_BITMAP(compiled_local_caps, ARM64_NCAPS);
	DECLARE_BITMAP(classified_local_caps, ARM64_NCAPS);
	DECLARE_BITMAP(early_local_caps, ARM64_NCAPS);
	DECLARE_BITMAP(target_local_caps, ARM64_NCAPS);
	struct arm64_late_cpu_target_plan
		target[ARM64_LATE_CPU_MAX_TARGETS];
	DECLARE_BITMAP(required_local_caps, ARM64_NCAPS);
	DECLARE_BITMAP(conflicting_local_caps, ARM64_NCAPS);
	struct arm64_late_cpu_effect_plan effects;
	u64 expected_elf_hwcap[3];
	u32 expected_compat_hwcap;
	u32 expected_compat_hwcap2;
	u8 local_caps_planned;
	u8 effects_planned;
	u8 hwcaps_planned;
	/* Core-owned SHA-256 over named fields, never structure padding. */
	u64 identity[ARM64_LATE_CPU_ID_WORDS];
};

/* Monotonic architecture-owned result; profile callbacks receive it const. */
struct arm64_late_cpu_receipt {
	u32 abi;
	u32 state;
	char profile_id[ARM64_LATE_CPU_PROFILE_ID_LEN];
	u64 blocker_mask;
	u64 plan_identity[ARM64_LATE_CPU_ID_WORDS];
	struct arm64_late_cpu_effect_plan committed;
	u8 commit_complete;
	u8 strict_caps_verified;
	u8 alternatives_finalized;
	u8 user_hwcaps_finalized;
};

/* The only profile identity available to later admission consumers. */
struct arm64_late_cpu_ready_token {
	u32 abi;
	char profile_id[ARM64_LATE_CPU_PROFILE_ID_LEN];
	cpumask_t target_cpus;
	u64 plan_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 source_parent_identity[ARM64_LATE_CPU_ID_WORDS];
	u64 config_input_identity[ARM64_LATE_CPU_ID_WORDS];
	struct arm64_late_cpu_runtime_binding binding;
	u64 evidence_identity[ARM64_LATE_CPU_ID_WORDS];
	u32 target_cpu[ARM64_LATE_CPU_MAX_TARGETS];
	u64 expected_target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u64 observed_target_mpidr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 expected_target_midr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 observed_target_midr[ARM64_LATE_CPU_MAX_TARGETS];
	u32 observed_target_revidr[ARM64_LATE_CPU_MAX_TARGETS];
	struct arm64_late_cpu_effect_plan committed;
};

struct arm64_cpu_capabilities;

struct arm64_late_cpu_profile {
	const char *name;
	u8 target_count;
	enum arm64_late_cpu_cap_state
	(*classify_local_cap)(const struct arm64_cpu_capabilities *cap,
			      const struct arm64_cpu_capabilities *match,
			      const struct arm64_late_cpu_evidence *evidence,
			      unsigned int target);
	int (*validate_plan)(const struct arm64_late_cpu_plan *plan);
	int (*derive_effects)(const struct arm64_late_cpu_plan *plan,
			      struct arm64_late_cpu_effect_plan *effects);
	/* Expected-only production input or an explicit FIXTURE; never RUNTIME. */
	int (*prepare)(struct arm64_late_cpu_evidence *evidence,
		       const struct cpumask *registered_targets);
	int (*verify_system)(const struct arm64_late_cpu_plan *plan,
			     const struct arm64_late_cpu_receipt *receipt);
	int (*finalize_user)(const struct arm64_late_cpu_plan *plan,
			     const struct arm64_late_cpu_receipt *receipt);
};

int arm64_late_cpu_validate_boot_caps(void);

#ifdef CONFIG_ARM64_LATE_CPU_PROFILE
void __init arm64_activate_late_cpu_profile(const struct arm64_late_cpu_profile *profile);
void __init arm64_register_late_cpu_target(const struct arm64_late_cpu_profile *profile,
					   unsigned int cpu);
void __init arm64_collect_late_cpu_runtime_identity(void);
void __init arm64_collect_late_cpu_runtime_system_policy(void);
int __init arm64_late_cpu_collect_system(struct arm64_late_cpu_system_cap_evidence *system);
int __init
arm64_late_cpu_collect_policy(struct arm64_late_cpu_target_policy_evidence *policy,
			      struct arm64_late_cpu_system_cap_evidence *system);
bool arm64_late_cpu_expected_pair_complete(const struct arm64_late_cpu_plan *plan);
int arm64_validate_late_cpu_preflight(unsigned int cpu);
int arm64_validate_late_cpu_expected_target(unsigned int cpu);
void __init arm64_seal_late_cpu_runtime_evidence(void);
void __init arm64_prepare_late_cpu_profile(void);
void __init arm64_commit_late_cpu_profile(void);
int __init arm64_commit_late_cpu_plan(const struct arm64_late_cpu_plan *plan);
int __init arm64_commit_late_cpu_mitigations(const struct arm64_late_cpu_effect_plan *effects);
int __init
arm64_verify_late_cpu_mitigations(const struct arm64_late_cpu_effect_plan *effects);
int __init
arm64_verify_late_cpu_system(const struct arm64_late_cpu_plan *plan,
			     const struct arm64_late_cpu_receipt *receipt);
int __init
arm64_finalize_late_cpu_hwcaps(const struct arm64_late_cpu_plan *plan,
			       const struct arm64_late_cpu_receipt *receipt);
int __init arm64_plan_late_cpu_capabilities(struct arm64_late_cpu_plan *draft,
					    const struct arm64_late_cpu_profile *profile);
int __init arm64_plan_late_cpu_effects(struct arm64_late_cpu_plan *draft,
				       const struct arm64_late_cpu_profile *profile);
int __init arm64_plan_late_cpu_hwcaps(struct arm64_late_cpu_plan *draft);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_midr_all_versions(const struct arm64_cpu_capabilities *cap,
				 u32 model);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_erratum_state(const struct arm64_cpu_capabilities *cap,
			     u32 model);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_static_feature_state(const struct arm64_cpu_capabilities *cap,
				    const struct arm64_cpu_capabilities *match,
				    u32 model);
bool __init
arm64_late_cpu_cap_uses_multi_entry_match(const struct arm64_cpu_capabilities *cap);
bool __init arm64_late_cpu_target_impl_override_active(void);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_early_system_cap_state(const struct arm64_cpu_capabilities *cap,
				      const struct arm64_cpu_capabilities *match,
				      const struct arm64_late_cpu_system_cap_evidence *system);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_gicv5_legacy_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_ich_hcr_tdir_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_cache_type_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target,
	const struct arm64_late_cpu_system_cap_evidence *system);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_expected_cache_type_state(const struct arm64_cpu_capabilities *cap,
					 const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_expected_pair *expected,
	const struct arm64_late_cpu_system_cap_evidence *system);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_a72_spectre_v2_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_a72_spectre_v4_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_a72_spectre_bhb_state(
	const struct arm64_cpu_capabilities *cap,
	const struct arm64_cpu_capabilities *match,
	const struct arm64_late_cpu_target_cap_evidence *target);
int __init arm64_late_cpu_a72_spectre_v2_v4_effects(
	const struct arm64_late_cpu_target_cap_evidence *target,
	const struct arm64_late_cpu_target_policy_evidence *policy,
	struct arm64_late_cpu_target_effect_plan *effects);
int __init arm64_late_cpu_a72_bhb_effect(
	const struct arm64_late_cpu_target_cap_evidence *target,
	const struct arm64_late_cpu_target_policy_evidence *policy,
	u8 system_v2_state,
	struct arm64_late_cpu_target_effect_plan *effects);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_expected_v2_state(const struct arm64_cpu_capabilities *cap,
				 const struct arm64_cpu_capabilities *match,
				 const struct arm64_late_cpu_expected_pair *expected);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_expected_v4_state(const struct arm64_cpu_capabilities *cap,
				 const struct arm64_cpu_capabilities *match,
				 const struct arm64_late_cpu_expected_pair *expected);
enum arm64_late_cpu_cap_state __init
arm64_late_cpu_expected_bhb_state(const struct arm64_cpu_capabilities *cap,
				  const struct arm64_cpu_capabilities *match,
				  const struct arm64_late_cpu_expected_pair *expected);
int __init
arm64_late_cpu_expected_effects(const struct arm64_late_cpu_expected_pair *expected,
				const struct arm64_late_cpu_target_policy_evidence *policy,
				struct arm64_late_cpu_target_effect_plan *effects);
void __init arm64_verify_late_cpu_profile_system(void);
void __init arm64_finalize_late_cpu_profile_user(void);
const struct arm64_late_cpu_ready_token *
arm64_get_late_cpu_ready_token(void);
#else
static inline void __init
arm64_activate_late_cpu_profile(const struct arm64_late_cpu_profile *profile)
{
}

static inline void __init
arm64_register_late_cpu_target(const struct arm64_late_cpu_profile *profile,
			       unsigned int cpu)
{
}

static inline void __init arm64_collect_late_cpu_runtime_identity(void)
{
}

static inline void __init
arm64_collect_late_cpu_runtime_system_policy(void)
{
}

static inline int
arm64_validate_late_cpu_preflight(unsigned int cpu)
{
	return 0;
}

static inline int
arm64_validate_late_cpu_expected_target(unsigned int cpu)
{
	return 0;
}

static inline void __init arm64_seal_late_cpu_runtime_evidence(void)
{
}

static inline void __init arm64_prepare_late_cpu_profile(void)
{
}

static inline void __init arm64_commit_late_cpu_profile(void)
{
}

static inline void __init arm64_verify_late_cpu_profile_system(void)
{
}

static inline void __init arm64_finalize_late_cpu_profile_user(void)
{
}

static inline const struct arm64_late_cpu_ready_token *
arm64_get_late_cpu_ready_token(void)
{
	return NULL;
}
#endif

#endif /* __ASM_LATE_CPU_PROFILE_H */
