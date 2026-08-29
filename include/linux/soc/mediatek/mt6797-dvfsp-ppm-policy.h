/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_POLICY_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_POLICY_H

#include <linux/types.h>

#define MT6797_DVFSP_PPM_POLICY_ABI		1
#define MT6797_DVFSP_PPM_POLICY_BANK_COUNT	4
#define MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES	16
#define MT6797_DVFSP_PPM_POLICY_BANK_BIG	0
#define MT6797_DVFSP_PPM_POLICY_BANK_L		1
#define MT6797_DVFSP_PPM_POLICY_BANK_2L	2
#define MT6797_DVFSP_PPM_POLICY_BANK_CCI	3

/*
 * Provider-owned PPM policy rows in EEM's packed bank order. The CCI bank
 * is intentional: the vendor three-cluster snapshot has no CCI limit table,
 * so a provider must supply this row set rather than infer one.
 */
struct mt6797_dvfsp_ppm_policy_snapshot {
	u32 abi;
	u32 reserved;
	u64 table_epoch;
	u32 frequency_khz[MT6797_DVFSP_PPM_POLICY_BANK_COUNT]
		[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES];
	u32 limit_khz[MT6797_DVFSP_PPM_POLICY_BANK_COUNT]
		[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES];
};

int mt6797_dvfsp_ppm_policy_validate(
	const struct mt6797_dvfsp_ppm_policy_snapshot *policy);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_POLICY_H */
