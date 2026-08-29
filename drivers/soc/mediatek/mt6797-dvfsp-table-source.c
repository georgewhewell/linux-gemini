// SPDX-License-Identifier: GPL-2.0-only
/*
 * Source-backed MT6797 SB/0119 frequency-table identity.
 *
 * The arrays are transcribed from the public MT6797 cpufreq source and are
 * selected only after the caller has decoded the PTP date identity. This
 * file is a pure data reader: it performs no I/O and does not claim a live
 * PPM policy or transition-owner lock.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-table-source.h>

static const u32 mt6797_dvfsp_table_ll_sb[MT6797_DVFSP_TABLE_SOURCE_ROWS] = {
	1547000, 1495000, 1443000, 1391000,
	1339000, 1274000, 1222000, 1118000,
	1014000, 897000, 806000, 715000,
	624000, 481000, 338000, 221000,
};

static const u32 mt6797_dvfsp_table_l_sb[MT6797_DVFSP_TABLE_SOURCE_ROWS] = {
	2002000, 1950000, 1885000, 1820000,
	1755000, 1703000, 1625000, 1495000,
	1352000, 1209000, 1092000, 962000,
	832000, 650000, 468000, 325000,
};

static const u32 mt6797_dvfsp_table_cci_sb[MT6797_DVFSP_TABLE_SOURCE_ROWS] = {
	988000, 975000, 936000, 910000,
	884000, 845000, 819000, 754000,
	676000, 611000, 533000, 481000,
	416000, 325000, 234000, 169000,
};

static const u32 mt6797_dvfsp_table_b_sb0119[MT6797_DVFSP_TABLE_SOURCE_ROWS] = {
	2522000, 2392000, 2327000, 2262000,
	2223000, 2158000, 2093000, 1885000,
	1677000, 1495000, 1378000, 1131000,
	1001000, 845000, 676000, 338000,
};

static int mt6797_dvfsp_table_source_identity_check(
	const struct mt6797_dvfsp_ptp_identity *identity)
{
	if (!identity || identity->abi != MT6797_DVFSP_PTP_IDENTITY_ABI)
		return -EINVAL;
	if (identity->date_code != MT6797_DVFSP_PTP_DATE_0119 ||
	    !identity->variant_id)
		return -EOPNOTSUPP;
	return 0;
}

int mt6797_dvfsp_table_source_read(
	const struct mt6797_dvfsp_ptp_identity *identity,
	struct mt6797_dvfsp_table_source_snapshot *snapshot)
{
	int ret;

	if (!snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_table_source_identity_check(identity);
	if (ret)
		return ret;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->abi = MT6797_DVFSP_TABLE_SOURCE_ABI;
	snapshot->table_family = MT6797_DVFSP_TABLE_SOURCE_FAMILY_SB;
	snapshot->date_code = identity->date_code;
	snapshot->source_epoch = MT6797_DVFSP_TABLE_SOURCE_EPOCH_SB0119;
	memcpy(snapshot->physical_khz[0], mt6797_dvfsp_table_ll_sb,
	       sizeof(mt6797_dvfsp_table_ll_sb));
	memcpy(snapshot->physical_khz[1], mt6797_dvfsp_table_l_sb,
	       sizeof(mt6797_dvfsp_table_l_sb));
	memcpy(snapshot->physical_khz[2], mt6797_dvfsp_table_b_sb0119,
	       sizeof(mt6797_dvfsp_table_b_sb0119));
	memcpy(snapshot->cci_khz, mt6797_dvfsp_table_cci_sb,
	       sizeof(mt6797_dvfsp_table_cci_sb));

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_table_source_read);

int mt6797_dvfsp_table_source_read_epoch(
	void *context,
	const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *table_epoch)
{
	struct mt6797_dvfsp_table_source_snapshot snapshot;
	int ret;

	(void)context;
	if (!table_epoch)
		return -EINVAL;
	ret = mt6797_dvfsp_table_source_read(identity, &snapshot);
	if (ret)
		return ret;
	*table_epoch = snapshot.source_epoch;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_table_source_read_epoch);

MODULE_DESCRIPTION("MT6797 source-backed SB/0119 frequency-table identity");
MODULE_LICENSE("GPL");
