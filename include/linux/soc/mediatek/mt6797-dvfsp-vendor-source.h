/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_SOURCE_H

#include <linux/bitops.h>
#include <linux/types.h>

#define MT6797_DVFSP_VENDOR_SOURCE_ABI	4
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER	3
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN	4
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP	16
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR	6
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ	16
#define MT6797_DVFSP_VENDOR_SOURCE_PPM		BIT(0)
#define MT6797_DVFSP_VENDOR_SOURCE_CPUFREQ	BIT(1)
#define MT6797_DVFSP_VENDOR_SOURCE_CSPM		BIT(2)
#define MT6797_DVFSP_VENDOR_SOURCE_EEM		BIT(3)
#define MT6797_DVFSP_VENDOR_SOURCE_ALL		GENMASK(3, 0)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_OBSERVATION_ABI	1
#define MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI	1
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_FUNCTION_WORD	BIT(0)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_DATE_WORD	BIT(1)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_CPU_BIN	BIT(2)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_EEM_ATE	BIT(3)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_PPM_TABLE	BIT(4)
#define MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_ALL	GENMASK(4, 0)
#define MT6797_DVFSP_VENDOR_SOURCE_EEM_VOLTAGE_UNIT_10UV	1
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_STATE	4
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW	\
	(MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_STATE * \
	 MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER)
#define MT6797_DVFSP_VENDOR_SOURCE_CLOCK_OWNER_CPU_DVFS	1
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_CLOCK_MUX	4
#define MT6797_DVFSP_VENDOR_SOURCE_VPROC_EXTBUCK	1
#define MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC	1
#define MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS	2
#define MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL	3
#define MT6797_DVFSP_VENDOR_SOURCE_VOLTAGE_UNIT_10UV	1
#define MT6797_DVFSP_VENDOR_SOURCE_VPROC_BASE_10UV	30000
#define MT6797_DVFSP_VENDOR_SOURCE_VPROC_STEP_10UV	1000
#define MT6797_DVFSP_VENDOR_SOURCE_VPROC_CODE_MASK	0x7f
#define MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BASE_10UV	90000
#define MT6797_DVFSP_VENDOR_SOURCE_VSRAM_STEP_10UV	2500
#define MT6797_DVFSP_VENDOR_SOURCE_VSRAM_CODE_OFFSET	3

struct mt6797_dvfsp_vendor_source_ppm_cluster {
	u32 cluster_id;
	u32 core_num;
	u32 cpu_id;
	u32 opp_count;
	u32 frequency_khz[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
	s32 min_cpufreq_idx;
	s32 max_cpufreq_idx;
	u32 min_cpu_core;
	u32 max_cpu_core;
	u32 has_advise_freq;
	s32 advise_cpufreq_idx;
	u32 has_advise_core;
	s32 advise_cpu_core;
};

struct mt6797_dvfsp_vendor_source_policy_row {
	u32 state;
	u32 cluster;
	s32 min_cpufreq_idx;
	s32 max_cpufreq_idx;
	u32 min_cpu_core;
	u32 max_cpu_core;
};

struct mt6797_dvfsp_vendor_source_cpufreq_domain {
	u32 id;
	s32 current_opp;
	u32 opp_count;
	u32 normal_max_opp;
	s32 armpll_available;
	u32 current_freq_khz;
	u32 current_volt;
	u32 current_vsram;
	u32 frequency_khz[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
	u32 voltage[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
	u32 voltage_original[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
	u32 clock_owner_kind;
	u32 clock_owner_id;
	u32 clock_mux;
	u32 clock_valid;
	u32 voltage_unit;
	u32 vproc_mode;
	u32 vproc_base_10uv;
	u32 vproc_step_10uv;
	u32 vproc_code_mask;
	u32 vsram_mode;
	u32 vsram_base_10uv;
	u32 vsram_step_10uv;
	s32 vsram_code_offset;
	u32 vsram_code_mask;
	u32 vsram_special_count;
	u32 vsram_special_code[MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL];
	u32 vsram_special_10uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL];
};

struct mt6797_dvfsp_vendor_source_cspm_cluster {
	u32 firmware_opp;
	u32 opp;
	u32 volt;
	u32 vsram;
	u32 ceiling;
	u32 floor;
	u8 is_on;
	u8 reserved[3];
};

struct mt6797_dvfsp_vendor_source_eem_detector {
	u32 id;
	s32 status;
	u32 features;
	u32 num_freq;
	u32 max_freq_khz;
	u32 disabled;
	u8 frequency_percent[MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ];
	u32 volt_tbl[MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ];
	u32 volt_tbl_init2[MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ];
	u32 volt_tbl_pmic[MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ];
	/* Vendor documents these values and conversion metadata in 10uV. */
	u32 voltage_unit;
	u32 eem_v_base;
	u32 eem_step;
	u32 pmic_base;
	u32 pmic_step;
	s32 volt_offset;
	s32 pi_offset;
};

struct mt6797_dvfsp_vendor_source_snapshot {
	u32 abi;
	u32 source_mask;
	u32 ppm_table_type;
	u32 ppm_cluster_count;
	u32 cpufreq_domain_count;
	u32 cspm_cluster_count;
	u32 eem_detector_count;
	u32 policy_row_count;
	struct mt6797_dvfsp_vendor_source_policy_row
		policy[MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW];
	u32 init_done;
	u32 hw_governor_enabled;
	u32 info_idvfs;
	u32 cpu_bin_level;
	u32 reserved;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_vendor_source_ppm_cluster
		ppm[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER];
	struct mt6797_dvfsp_vendor_source_cpufreq_domain
		cpufreq[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN];
	struct mt6797_dvfsp_vendor_source_cspm_cluster
		cspm[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER + 1];
	struct mt6797_dvfsp_vendor_source_eem_detector
		eem[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR];
};

/* This identity must come from a real PTP/efuse source; table hashes are not identity. */
struct mt6797_dvfsp_vendor_source_identity {
	u32 abi;
	u32 variant_id;
	u64 table_epoch;
	u64 calibration_handle;
};
/* Raw source observation; this is not calibrated-owner identity. */
struct mt6797_dvfsp_vendor_source_identity_observation {
	u32 abi;
	u32 source_mask;
	u32 function_word;
	u32 date_word;
	u32 cpu_bin_level;
	u32 eem_ate_version;
	u32 ppm_table_type;
};
/* The vendor must prove these fields against one source snapshot. */
struct mt6797_dvfsp_vendor_source_provenance {
	u32 abi;
	u32 variant_id;
	u64 table_epoch;
	u64 calibration_handle;
	u64 source_generation;
	u64 owner_handle;
	u64 transition_handle;
};

struct mt6797_dvfsp_vendor_source_ops {
	u32 abi;
	int (*snapshot)(void *context,
			struct mt6797_dvfsp_vendor_source_snapshot *snapshot);
	int (*read_identity)(void *context,
			struct mt6797_dvfsp_vendor_source_identity *identity);
	int (*read_identity_observation)(void *context,
			struct mt6797_dvfsp_vendor_source_identity_observation *observation);
	int (*read_provenance)(void *context,
			const struct mt6797_dvfsp_vendor_source_snapshot *snapshot,
			struct mt6797_dvfsp_vendor_source_provenance *provenance);
};

struct mt6797_dvfsp_vendor_source_registration {
	const struct mt6797_dvfsp_vendor_source_ops *ops;
	void *context;
};

struct mt6797_dvfsp_vendor_source {
	const struct mt6797_dvfsp_vendor_source_ops *ops;
	void *context;
	u64 owner_handle;
	u64 transition_handle;
	bool initialized;
	bool invalidated;
};

int mt6797_dvfsp_vendor_source_init(
	struct mt6797_dvfsp_vendor_source *source,
	const struct mt6797_dvfsp_vendor_source_ops *ops, void *context,
	u64 owner_handle, u64 transition_handle);
int mt6797_dvfsp_vendor_source_snapshot(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_snapshot *snapshot);
int mt6797_dvfsp_vendor_source_identity(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_identity *identity);
int mt6797_dvfsp_vendor_source_identity_observation(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_identity_observation *observation);
int mt6797_dvfsp_vendor_source_provenance(
	struct mt6797_dvfsp_vendor_source *source,
	const struct mt6797_dvfsp_vendor_source_snapshot *snapshot,
	struct mt6797_dvfsp_vendor_source_provenance *provenance);
void mt6797_dvfsp_vendor_source_invalidate(
	struct mt6797_dvfsp_vendor_source *source);
void mt6797_dvfsp_vendor_source_exit(
	struct mt6797_dvfsp_vendor_source *source);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_SOURCE_H */
