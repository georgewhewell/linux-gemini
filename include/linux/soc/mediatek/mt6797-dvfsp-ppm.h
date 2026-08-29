/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_H

#include <linux/errno.h>
#include <linux/types.h>

#define MT6797_DVFSP_PPM_SOURCE_ABI		1
#define MT6797_DVFSP_PPM_CLUSTER_COUNT		3
#define MT6797_DVFSP_PPM_TABLE_ENTRIES		16
#define MT6797_DVFSP_PPM_TABLE_TYPE_FY		0
#define MT6797_DVFSP_PPM_TABLE_TYPE_SB		1
#define MT6797_DVFSP_PPM_TABLE_TYPE_MB		2
#define MT6797_DVFSP_PPM_CLUSTER_LL		0
#define MT6797_DVFSP_PPM_CLUSTER_L		1
#define MT6797_DVFSP_PPM_CLUSTER_B		2
#define MT6797_DVFSP_PPM_LL_CPU_ID		0
#define MT6797_DVFSP_PPM_L_CPU_ID		4
#define MT6797_DVFSP_PPM_B_CPU_ID		8
#define MT6797_DVFSP_PPM_LL_CORE_COUNT		4
#define MT6797_DVFSP_PPM_L_CORE_COUNT		4
#define MT6797_DVFSP_PPM_B_CORE_COUNT		2

/*
 * This is the copied shape of the vendor ppm_cluster_info plus its current
 * ppm_client_req limit. frequency_khz[] retains the vendor's OPP order:
 * index zero is the highest frequency.
 */
struct mt6797_dvfsp_ppm_cluster_snapshot {
	u32 cluster_id;
	u32 cpu_id;
	u32 core_num;
	u32 dvfs_opp_num;
	u32 frequency_khz[MT6797_DVFSP_PPM_TABLE_ENTRIES];
	s32 min_cpufreq_idx;
	s32 max_cpufreq_idx;
	u32 min_cpu_core;
	u32 max_cpu_core;
	u32 has_advise_freq;
	s32 advise_cpufreq_idx;
	u32 has_advise_core;
	s32 advise_cpu_core;
};

struct mt6797_dvfsp_ppm_snapshot {
	u32 abi;
	u32 reserved;
	u32 cluster_num;
	u32 dvfs_tbl_type;
	u32 root_cluster;
	u32 is_ptp_policy_activate;
	u64 table_epoch;
	struct mt6797_dvfsp_ppm_cluster_snapshot
		cluster[MT6797_DVFSP_PPM_CLUSTER_COUNT];
};

int mt6797_dvfsp_ppm_snapshot_validate(
	const struct mt6797_dvfsp_ppm_snapshot *snapshot);

/* Map the vendor index interval to the current frequency interval. */
int mt6797_dvfsp_ppm_snapshot_current_limit(
	const struct mt6797_dvfsp_ppm_snapshot *snapshot,
	u32 cluster_id, u32 *floor_khz, u32 *ceiling_khz);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_H */
