/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_PROVIDER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_PROVIDER_H

#include <linux/bitops.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-calibrated-provider.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-source.h>

#define MT6797_DVFSP_VENDOR_PROVIDER_ABI	5
#define MT6797_DVFSP_VENDOR_PROVIDER_PPM	BIT(0)
#define MT6797_DVFSP_VENDOR_PROVIDER_CPUFREQ	BIT(1)
#define MT6797_DVFSP_VENDOR_PROVIDER_CSPM	BIT(2)
#define MT6797_DVFSP_VENDOR_PROVIDER_EEM	BIT(3)
#define MT6797_DVFSP_VENDOR_PROVIDER_IDENTITY	BIT(4)
#define MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_EEM_UV	BIT(5)
#define MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_POLICY_ROWS	BIT(6)
#define MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_CLOCK	BIT(7)
#define MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_RAIL	BIT(8)
#define MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_ALL	GENMASK(8, 0)

/* These fields are intentionally not guessed from a partial vendor record. */
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_POLICY_ROWS	BIT(0)
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_EEM_UV	BIT(1)
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_CLOCK	BIT(2)
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_RAIL	BIT(3)
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_REGISTRATION	BIT(4)
#define MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_ALL	GENMASK(4, 0)

#define MT6797_DVFSP_VENDOR_PROVIDER_EEM_BANK_BIG	0
#define MT6797_DVFSP_VENDOR_PROVIDER_EEM_BANK_L	1
#define MT6797_DVFSP_VENDOR_PROVIDER_EEM_BANK_2L	2
#define MT6797_DVFSP_VENDOR_PROVIDER_EEM_BANK_CCI	3

/*
 * This is a review view, not a replacement for the calibrated-provider
 * callback ABI.  PPM/cpufreq arrays are ordered by ID; CSPM is the fixed
 * LL/L/B/CCI order; EEM keeps every source detector and names the four
 * canonical detector IDs separately.  Voltage values remain raw vendor data.
 */
struct mt6797_dvfsp_vendor_provider_snapshot {
	u32 abi;
	u32 mapped_mask;
	u32 unavailable_mask;
	u32 ppm_table_type;
	u32 reserved;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_vendor_source_identity identity;
	struct mt6797_dvfsp_vendor_source_provenance provenance;
	u32 policy_row_count;
	struct mt6797_dvfsp_vendor_source_policy_row
		policy[MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW];
	u32 policy_source_index[MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW];
	struct mt6797_dvfsp_vendor_source_ppm_cluster
		ppm[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER];
	struct mt6797_dvfsp_vendor_source_cpufreq_domain
		cpufreq[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN];
	struct mt6797_dvfsp_vendor_source_cspm_cluster
		cspm[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER + 1];
	struct mt6797_dvfsp_vendor_source_eem_detector
		eem[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR];
	u32 ppm_source_index[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER];
	u32 cpufreq_source_index[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN];
	u32 eem_source_index[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR];
	u32 eem_provider_detector[4];
	/* Converted from the source PMIC table; unit is microvolts. */
	u32 eem_voltage_uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR]
		[MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ];
	u32 eem_voltage_count[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR];
	/* Converted from source 10uV values; unit is microvolts. */
	u32 cpufreq_current_volt_uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN];
	u32 cpufreq_current_vsram_uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN];
	u32 cpufreq_voltage_uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN]
		[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
	u32 cpufreq_voltage_original_uv[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN]
		[MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP];
};

/* Pure field/identity mapping; no source callback or provider is invoked. */
int mt6797_dvfsp_vendor_provider_map(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	const struct mt6797_dvfsp_vendor_source_identity *identity,
	const struct mt6797_dvfsp_vendor_source_provenance *provenance,
	u64 owner_handle, u64 transition_handle,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped);

/* Require one exact vendor, calibrated-state, and owner identity view. */
int mt6797_dvfsp_vendor_provider_match_state(
	const struct mt6797_dvfsp_vendor_provider_snapshot *mapped,
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	const struct mt6797_dvfsp_state_owner_identity *identity);

/* Attach the read-only review view to an already dormant calibrated provider. */
struct mt6797_dvfsp_vendor_provider_bridge {
	struct mt6797_dvfsp_calibrated_provider *provider;
	struct mt6797_dvfsp_vendor_source *source;
	u64 owner_handle;
	u64 transition_handle;
	bool initialized;
};

int mt6797_dvfsp_vendor_provider_bridge_init(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge,
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_vendor_source *source,
	u64 owner_handle, u64 transition_handle);
int mt6797_dvfsp_vendor_provider_bridge_snapshot(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped);
void mt6797_dvfsp_vendor_provider_bridge_exit(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_PROVIDER_H */
