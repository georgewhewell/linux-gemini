// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free KUnit coverage for the source-backed table identity. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-table-source.h>

static const u32 mt6797_dvfsp_table_test_expected[4][16] = {
	{ 1547000, 1495000, 1443000, 1391000, 1339000, 1274000,
	  1222000, 1118000, 1014000, 897000, 806000, 715000,
	  624000, 481000, 338000, 221000 },
	{ 2002000, 1950000, 1885000, 1820000, 1755000, 1703000,
	  1625000, 1495000, 1352000, 1209000, 1092000, 962000,
	  832000, 650000, 468000, 325000 },
	{ 2522000, 2392000, 2327000, 2262000, 2223000, 2158000,
	  2093000, 1885000, 1677000, 1495000, 1378000, 1131000,
	  1001000, 845000, 676000, 338000 },
	{ 988000, 975000, 936000, 910000, 884000, 845000,
	  819000, 754000, 676000, 611000, 533000, 481000,
	  416000, 325000, 234000, 169000 },
};

static void mt6797_dvfsp_table_source_exact_sb0119(struct kunit *test)
{
	struct mt6797_dvfsp_ptp_identity identity = {
		.abi = MT6797_DVFSP_PTP_IDENTITY_ABI,
		.variant_id = 0x101,
		.date_code = MT6797_DVFSP_PTP_DATE_0119,
	};
	struct mt6797_dvfsp_table_source_snapshot snapshot;
	unsigned int bank;
	unsigned int row;
	u64 epoch;

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_table_source_read(&identity, &snapshot), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_table_source_read_epoch(NULL, &identity, &epoch), 0);
	KUNIT_EXPECT_EQ(test, epoch,
		(u64)MT6797_DVFSP_TABLE_SOURCE_EPOCH_SB0119);
	KUNIT_EXPECT_EQ(test, snapshot.abi,
		MT6797_DVFSP_TABLE_SOURCE_ABI);
	KUNIT_EXPECT_EQ(test, snapshot.table_family,
		MT6797_DVFSP_TABLE_SOURCE_FAMILY_SB);
	KUNIT_EXPECT_EQ(test, snapshot.source_epoch,
		(u64)MT6797_DVFSP_TABLE_SOURCE_EPOCH_SB0119);
	for (bank = 0; bank < ARRAY_SIZE(mt6797_dvfsp_table_test_expected);
	     bank++)
		for (row = 0; row < MT6797_DVFSP_TABLE_SOURCE_ROWS; row++)
			KUNIT_EXPECT_EQ(test,
				bank < MT6797_DVFSP_TABLE_SOURCE_PHYSICAL ?
				snapshot.physical_khz[bank][row] :
				snapshot.cci_khz[row],
				mt6797_dvfsp_table_test_expected[bank][row]);
}

static void mt6797_dvfsp_table_source_rejects_other_date(
	struct kunit *test)
{
	struct mt6797_dvfsp_ptp_identity identity = {
		.abi = MT6797_DVFSP_PTP_IDENTITY_ABI,
		.variant_id = 1,
		.date_code = MT6797_DVFSP_PTP_DATE_1221,
	};
	struct mt6797_dvfsp_table_source_snapshot snapshot;

	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_table_source_read(&identity, &snapshot),
		-EOPNOTSUPP);
}

static struct kunit_case mt6797_dvfsp_table_source_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_table_source_exact_sb0119),
	KUNIT_CASE(mt6797_dvfsp_table_source_rejects_other_date),
	{ }
};

static struct kunit_suite mt6797_dvfsp_table_source_test_suite = {
	.name = "mt6797-dvfsp-table-source",
	.test_cases = mt6797_dvfsp_table_source_test_cases,
};

kunit_test_suite(mt6797_dvfsp_table_source_test_suite);

MODULE_LICENSE("GPL");
