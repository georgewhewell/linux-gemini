// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 LK CCCI hand-off parser. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define MD1_TEST_TABLE_SIZE	1024

static void mt6797_md1_lk_test_make_info(u8 *data, u32 table_size,
					 u32 tag_count)
{
	memset(data, 0, MT6797_MD1_LK_INFO_V2_SIZE);
	put_unaligned_le64(0x88000000, data);
	put_unaligned_le32(table_size, data + 8);
	put_unaligned_le32(2, data + 16);
	put_unaligned_le32(tag_count, data + 20);
}

static void mt6797_md1_lk_test_put_tag(u8 *table, u32 offset,
				       const char *name, u32 data_offset,
					u32 data_size, u32 next_offset)
{
	memcpy(table + offset, name, strlen(name) + 1);
	put_unaligned_le32(data_offset, table + offset + 64);
	put_unaligned_le32(data_size, table + offset + 68);
	put_unaligned_le32(next_offset, table + offset + 72);
}

static void mt6797_md1_lk_test_parses_live_shape(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_INFO_V2_SIZE];
	struct mt6797_md1_lk_info info;

	/* The read-only Gemini mainline probe observed this v2 shape. */
	mt6797_md1_lk_test_make_info(data, 0xa20, 22);
	put_unaligned_le32(5, data + 24);
	KUNIT_ASSERT_EQ(test, mt6797_md1_lk_info_parse_v2(data, sizeof(data),
							  &info), 0);
	KUNIT_EXPECT_EQ(test, info.table_phys, 0x88000000ULL);
	KUNIT_EXPECT_EQ(test, info.table_size, 0xa20U);
	KUNIT_EXPECT_EQ(test, info.tag_count, 22U);
	KUNIT_EXPECT_EQ(test, info.load_flags, 5U);
}

static void mt6797_md1_lk_test_rejects_packed_44_byte_v2(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_INFO_V2_SIZE];
	struct mt6797_md1_lk_info info;

	mt6797_md1_lk_test_make_info(data, 0xa20, 22);
	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_info_parse_v2(data, 44, &info),
			-EMSGSIZE);
}

static void mt6797_md1_lk_test_rejects_incoherent_info(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_INFO_V2_SIZE];
	struct mt6797_md1_lk_info info;

	mt6797_md1_lk_test_make_info(data, 0x100, 22);
	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_info_parse_v2(data, sizeof(data),
							  &info), -EINVAL);
}

static void mt6797_md1_lk_test_does_not_mutate_output_on_failure(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_INFO_V2_SIZE];
	struct mt6797_md1_lk_info info;
	struct mt6797_md1_lk_info original;

	mt6797_md1_lk_test_make_info(data, 0x100, 22);
	memset(&info, 0xa5, sizeof(info));
	original = info;
	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_info_parse_v2(data, sizeof(data),
							   &info), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(&info, &original, sizeof(info)), 0);
}

static void mt6797_md1_lk_test_finds_linked_tag(struct kunit *test)
{
	u8 table[MD1_TEST_TABLE_SIZE] = { };
	struct mt6797_md1_lk_tag tag;

	mt6797_md1_lk_test_put_tag(table, 0, "hdr_count", 228, 4,
				   MT6797_MD1_LK_TAG_V2_SIZE);
	mt6797_md1_lk_test_put_tag(table, MT6797_MD1_LK_TAG_V2_SIZE,
				   "md1_chk", 232, 512,
					2 * MT6797_MD1_LK_TAG_V2_SIZE);
	mt6797_md1_lk_test_put_tag(table, 2 * MT6797_MD1_LK_TAG_V2_SIZE,
				   "md1img", 744, 4, 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_lk_tag_find_v2(table, sizeof(table), 3,
							"md1_chk", &tag), 0);
	KUNIT_EXPECT_EQ(test, tag.data_offset, 232U);
	KUNIT_EXPECT_EQ(test, tag.data_size, 512U);
}

static void mt6797_md1_lk_test_rejects_oob_linked_data(struct kunit *test)
{
	u8 table[MD1_TEST_TABLE_SIZE] = { };
	struct mt6797_md1_lk_tag tag;

	mt6797_md1_lk_test_put_tag(table, 0, "md1_chk", 800, 512, 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_tag_find_v2(table, sizeof(table), 1,
							"md1_chk", &tag), -ERANGE);
}

static void mt6797_md1_lk_test_parses_md1_memory_record(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_MODEM_INFO_SIZE] = { };
	struct mt6797_md1_lk_modem_info info;

	put_unaligned_le64(0xb4000000, data);
	/* LK MD1 image is 128 MiB; the live mblock-6 CCCI lease is larger. */
	put_unaligned_le32(0x08000000, data + 8);
	data[12] = MT6797_MD1_LK_MD1_INDEX;
	data[14] = 9;
	data[15] = 2;
	KUNIT_ASSERT_EQ(test, mt6797_md1_lk_modem_info_find(data, sizeof(data),
								 MT6797_MD1_LK_MD1_INDEX, &info), 0);
	KUNIT_EXPECT_EQ(test, info.base_phys, 0xb4000000ULL);
	KUNIT_EXPECT_EQ(test, info.size, 0x08000000U);
	KUNIT_EXPECT_EQ(test, info.load_error, (s8)0);
}

static void mt6797_md1_lk_test_rejects_oob_shared_memory(struct kunit *test)
{
	u8 data[MT6797_MD1_LK_SMEM_LAYOUT_SIZE] = { };
	struct mt6797_md1_lk_smem_layout layout;

	put_unaligned_le64(0x88000000, data);
	put_unaligned_le32(0x400000, data + 12);
	put_unaligned_le32(0x600000, data + 32);
	put_unaligned_le32(0x400000, data + 24);
	put_unaligned_le32(0x300000, data + 28);
	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_smem_layout_parse(data, sizeof(data),
								   &layout), -ERANGE);
}

static void mt6797_md1_lk_test_load_evidence_is_not_execution(struct kunit *test)
{
	struct mt6797_md1_lk_info lk_info = {
		.load_flags = MT6797_MD1_LK_MD1_LOAD_FLAG,
	};
	struct mt6797_md1_lk_modem_info md1 = {
		.md_id = MT6797_MD1_LK_MD1_INDEX,
	};
	enum mt6797_md1_lk_md1_load_evidence evidence;

	KUNIT_ASSERT_EQ(test, mt6797_md1_lk_md1_load_evidence(&lk_info, &md1,
							  &evidence), 0);
	KUNIT_EXPECT_EQ(test, evidence,
				MT6797_MD1_LK_MD1_LOAD_EXECUTION_UNKNOWN);
}

static void mt6797_md1_lk_test_load_evidence_rejects_missing_flag(struct kunit *test)
{
	struct mt6797_md1_lk_info lk_info = { };
	struct mt6797_md1_lk_modem_info md1 = {
		.md_id = MT6797_MD1_LK_MD1_INDEX,
	};
	enum mt6797_md1_lk_md1_load_evidence evidence =
		MT6797_MD1_LK_MD1_LOAD_EXECUTION_UNKNOWN;

	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_md1_load_evidence(&lk_info, &md1,
							   &evidence), -ENODATA);
	KUNIT_EXPECT_EQ(test, evidence, MT6797_MD1_LK_MD1_LOAD_NONE);
}

static void mt6797_md1_lk_test_load_evidence_error_wins_over_flag(struct kunit *test)
{
	struct mt6797_md1_lk_info lk_info = {
		.load_flags = MT6797_MD1_LK_MD1_LOAD_FLAG,
		.md_error = { [MT6797_MD1_LK_MD1_INDEX] = -7 },
	};
	struct mt6797_md1_lk_modem_info md1 = {
		.md_id = MT6797_MD1_LK_MD1_INDEX,
	};
	enum mt6797_md1_lk_md1_load_evidence evidence;

	KUNIT_EXPECT_EQ(test, mt6797_md1_lk_md1_load_evidence(&lk_info, &md1,
							   &evidence), -EIO);
	KUNIT_EXPECT_EQ(test, evidence, MT6797_MD1_LK_MD1_LOAD_FAILED);
}

static struct kunit_case mt6797_md1_lk_handoff_test_cases[] = {
	KUNIT_CASE(mt6797_md1_lk_test_parses_live_shape),
	KUNIT_CASE(mt6797_md1_lk_test_rejects_packed_44_byte_v2),
	KUNIT_CASE(mt6797_md1_lk_test_rejects_incoherent_info),
	KUNIT_CASE(mt6797_md1_lk_test_does_not_mutate_output_on_failure),
	KUNIT_CASE(mt6797_md1_lk_test_finds_linked_tag),
	KUNIT_CASE(mt6797_md1_lk_test_rejects_oob_linked_data),
	KUNIT_CASE(mt6797_md1_lk_test_parses_md1_memory_record),
	KUNIT_CASE(mt6797_md1_lk_test_rejects_oob_shared_memory),
	KUNIT_CASE(mt6797_md1_lk_test_load_evidence_is_not_execution),
	KUNIT_CASE(mt6797_md1_lk_test_load_evidence_rejects_missing_flag),
	KUNIT_CASE(mt6797_md1_lk_test_load_evidence_error_wins_over_flag),
	{}
};

static struct kunit_suite mt6797_md1_lk_handoff_test_suite = {
	.name = "mt6797-md1-lk-handoff",
	.test_cases = mt6797_md1_lk_handoff_test_cases,
};

kunit_test_suite(mt6797_md1_lk_handoff_test_suite);

MODULE_LICENSE("GPL");
