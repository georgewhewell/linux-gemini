// SPDX-License-Identifier: GPL-2.0-only
/*
 * Validate a copied MT6797 vendor PPM snapshot.
 *
 * The real owner must copy this object while holding the vendor
 * ppm_main_info.lock and the external clock/rail transition lock. This file
 * intentionally has no access to either private owner and performs no I/O.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm.h>

static int mt6797_dvfsp_ppm_cluster_check(
	const struct mt6797_dvfsp_ppm_cluster_snapshot *cluster,
	u32 expected_id, u32 expected_cpu, u32 expected_cores)
{
	u32 previous_frequency = ~0U;
	u32 i;

	if (!cluster || cluster->cluster_id != expected_id ||
	    cluster->cpu_id != expected_cpu ||
	    cluster->core_num != expected_cores ||
	    cluster->dvfs_opp_num != MT6797_DVFSP_PPM_TABLE_ENTRIES ||
	    cluster->min_cpufreq_idx < 0 ||
	    cluster->max_cpufreq_idx < 0 ||
	    cluster->max_cpufreq_idx > cluster->min_cpufreq_idx ||
    cluster->min_cpufreq_idx >= MT6797_DVFSP_PPM_TABLE_ENTRIES ||
    cluster->max_cpufreq_idx >= MT6797_DVFSP_PPM_TABLE_ENTRIES ||
    cluster->min_cpu_core > cluster->max_cpu_core ||
    cluster->max_cpu_core > expected_cores ||
    cluster->has_advise_freq > 1 || cluster->has_advise_core > 1)
		return -EPROTO;

	if (!cluster->has_advise_freq && cluster->advise_cpufreq_idx != -1)
		return -EPROTO;
	if (cluster->has_advise_freq &&
	    (cluster->advise_cpufreq_idx < 0 ||
    cluster->advise_cpufreq_idx >= MT6797_DVFSP_PPM_TABLE_ENTRIES))
		return -EPROTO;
	if (!cluster->has_advise_core && cluster->advise_cpu_core != -1)
		return -EPROTO;
	if (cluster->has_advise_core &&
	    (cluster->advise_cpu_core < 0 ||
    cluster->advise_cpu_core > expected_cores))
		return -EPROTO;

	for (i = 0; i < MT6797_DVFSP_PPM_TABLE_ENTRIES; i++) {
		if (!cluster->frequency_khz[i] ||
		    cluster->frequency_khz[i] >= previous_frequency)
			return -EPROTO;
		previous_frequency = cluster->frequency_khz[i];
	}

	return 0;
}

int mt6797_dvfsp_ppm_snapshot_validate(
	const struct mt6797_dvfsp_ppm_snapshot *snapshot)
{
	int ret;

	if (!snapshot || snapshot->abi != MT6797_DVFSP_PPM_SOURCE_ABI ||
	    snapshot->reserved ||
	    snapshot->cluster_num != MT6797_DVFSP_PPM_CLUSTER_COUNT ||
	    snapshot->dvfs_tbl_type > MT6797_DVFSP_PPM_TABLE_TYPE_MB ||
	    snapshot->root_cluster >= MT6797_DVFSP_PPM_CLUSTER_COUNT ||
	    snapshot->is_ptp_policy_activate > 1 || !snapshot->table_epoch)
		return -EPROTO;

	ret = mt6797_dvfsp_ppm_cluster_check(
		&snapshot->cluster[MT6797_DVFSP_PPM_CLUSTER_LL],
		MT6797_DVFSP_PPM_CLUSTER_LL, MT6797_DVFSP_PPM_LL_CPU_ID,
		MT6797_DVFSP_PPM_LL_CORE_COUNT);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_ppm_cluster_check(
		&snapshot->cluster[MT6797_DVFSP_PPM_CLUSTER_L],
		MT6797_DVFSP_PPM_CLUSTER_L, MT6797_DVFSP_PPM_L_CPU_ID,
		MT6797_DVFSP_PPM_L_CORE_COUNT);
	if (ret)
		return ret;

	return mt6797_dvfsp_ppm_cluster_check(
		&snapshot->cluster[MT6797_DVFSP_PPM_CLUSTER_B],
		MT6797_DVFSP_PPM_CLUSTER_B, MT6797_DVFSP_PPM_B_CPU_ID,
		MT6797_DVFSP_PPM_B_CORE_COUNT);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ppm_snapshot_validate);

int mt6797_dvfsp_ppm_snapshot_current_limit(
	const struct mt6797_dvfsp_ppm_snapshot *snapshot,
	u32 cluster_id, u32 *floor_khz, u32 *ceiling_khz)
{
	const struct mt6797_dvfsp_ppm_cluster_snapshot *cluster;
	int ret;

	if (!floor_khz || !ceiling_khz ||
	    cluster_id >= MT6797_DVFSP_PPM_CLUSTER_COUNT)
		return -EINVAL;
	ret = mt6797_dvfsp_ppm_snapshot_validate(snapshot);
	if (ret)
		return ret;

	cluster = &snapshot->cluster[cluster_id];
	*floor_khz = cluster->frequency_khz[cluster->min_cpufreq_idx];
	*ceiling_khz = cluster->frequency_khz[cluster->max_cpufreq_idx];
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ppm_snapshot_current_limit);

MODULE_DESCRIPTION("MT6797 PPM snapshot contract");
MODULE_LICENSE("GPL");
