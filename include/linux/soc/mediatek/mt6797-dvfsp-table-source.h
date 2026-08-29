/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_TABLE_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_TABLE_SOURCE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>

#define MT6797_DVFSP_TABLE_SOURCE_ABI		1
#define MT6797_DVFSP_TABLE_SOURCE_FAMILY_SB	1
#define MT6797_DVFSP_TABLE_SOURCE_EPOCH_SB0119	0x53420119ULL
#define MT6797_DVFSP_TABLE_SOURCE_ROWS		16
#define MT6797_DVFSP_TABLE_SOURCE_PHYSICAL	3

/*
 * This object contains frequency identity only. It intentionally has no
 * policy-limit, voltage, lock, generation, calibration, or owner fields.
 */
struct mt6797_dvfsp_table_source_snapshot {
	u32 abi;
	u32 table_family;
	u32 date_code;
	u64 source_epoch;
	u32 physical_khz[MT6797_DVFSP_TABLE_SOURCE_PHYSICAL]
		[MT6797_DVFSP_TABLE_SOURCE_ROWS];
	u32 cci_khz[MT6797_DVFSP_TABLE_SOURCE_ROWS];
};

int mt6797_dvfsp_table_source_read(
	const struct mt6797_dvfsp_ptp_identity *identity,
	struct mt6797_dvfsp_table_source_snapshot *snapshot);

int mt6797_dvfsp_table_source_read_epoch(
	void *context,
	const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *table_epoch);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_TABLE_SOURCE_H */
