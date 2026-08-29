/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_SOURCE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm-owner.h>

#define MT6797_DVFSP_PPM_SOURCE_ADAPTER_ABI	1

/*
 * The owner holds its external transition lock before calling lock(). The
 * source lock is the vendor ppm_main_info.lock boundary; snapshot() is
 * called while that lock is held and must not take it again.
 */
struct mt6797_dvfsp_ppm_source_ops {
	u32 abi;
	int (*lock)(void *context);
	void (*unlock)(void *context);
	int (*read_ppm_clusters)(void *context,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				struct mt6797_dvfsp_ppm_snapshot *snapshot);
	int (*read_cci_frequency)(void *context,
				 const struct mt6797_dvfsp_ptp_state *ptp_state,
				 u32 frequency_khz[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES]);
	int (*read_policy_limits)(void *context,
				 const struct mt6797_dvfsp_ptp_state *ptp_state,
				 u32 limit_khz[MT6797_DVFSP_PPM_POLICY_BANK_COUNT]
					[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES]);
	int (*read_table_epoch)(void *context,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				u64 *table_epoch);
};

/* Device and vendor-owner references remain borrowed by the caller. */
struct mt6797_dvfsp_ppm_source {
	const struct mt6797_dvfsp_ppm_source_ops *ops;
	void *context;
};

/* Called by the PPM owner while its source lock is held. */
int mt6797_dvfsp_ppm_source_snapshot(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy);

/* Populate the existing dormant PPM-owner callbacks; no owner is registered. */
int mt6797_dvfsp_ppm_source_owner_ops_init(
	const struct mt6797_dvfsp_ppm_source *source,
	struct mt6797_dvfsp_ppm_owner_ops *owner_ops);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_SOURCE_H */
