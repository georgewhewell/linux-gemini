// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <kunit/test.h>

#include "mtk_mt6797_consys_fw.h"

static const u8 sha256_abc[MT6797_CONSYS_FW_SHA256_SIZE] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
	0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
	0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static void mt6797_consys_fw_verify_test(struct kunit *test)
{
	u8 corrupt[sizeof(sha256_abc)];

	memcpy(corrupt, sha256_abc, sizeof(corrupt));
	corrupt[0] ^= 1;

	KUNIT_EXPECT_EQ(test, mt6797_consys_fw_verify((const u8 *)"abc", 3, 3,
						      sha256_abc), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_fw_verify((const u8 *)"abc", 3, 4,
						      sha256_abc), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, mt6797_consys_fw_verify((const u8 *)"abc", 3, 3,
						      corrupt), -EKEYREJECTED);
}

static void mt6797_consys_wmt_parse_test(struct kunit *test)
{
	u8 patch[MT6797_CONSYS_WMT_HEADER_SIZE + 2] = {
		'2', '0', '1', '8', '0', '3', '0', '7',
		'b', '0', '0', '1', '0', '0', '5', '\n',
		'1', '7', '1', '5', 0x8a, 0x00, 0x8a, 0x00,
		0x21, 0x00, 0x0a, 0xf0, 0xaa, 0x55,
	};
	struct mt6797_consys_wmt_metadata metadata = { };
	static const u8 address[] = { 0x00, 0x00, 0x0a, 0xf0 };
	static const u8 address2[] = { 0x00, 0x00, 0x09, 0x00 };

	KUNIT_ASSERT_EQ(test, mt6797_consys_wmt_parse(patch, sizeof(patch), 1,
						      &metadata), 0);
	KUNIT_EXPECT_MEMEQ(test, metadata.build_time, patch, 16);
	KUNIT_EXPECT_MEMEQ(test, metadata.platform, "1715", 4);
	KUNIT_EXPECT_EQ(test, metadata.hw_version, (u16)0x008a);
	KUNIT_EXPECT_EQ(test, metadata.sw_version, (u16)0x008a);
	KUNIT_EXPECT_EQ(test, metadata.patch_count, (u8)2);
	KUNIT_EXPECT_EQ(test, metadata.sequence, (u8)1);
	KUNIT_EXPECT_MEMEQ(test, metadata.address, address, sizeof(address));
	KUNIT_EXPECT_PTR_EQ(test, metadata.payload,
			    patch + MT6797_CONSYS_WMT_HEADER_SIZE);
	KUNIT_EXPECT_EQ(test, metadata.payload_size, (size_t)2);

	patch[24] = 0x22;
	patch[26] = 0x09;
	patch[27] = 0x00;
	KUNIT_ASSERT_EQ(test, mt6797_consys_wmt_parse(patch, sizeof(patch), 2,
						      &metadata), 0);
	KUNIT_EXPECT_MEMEQ(test, metadata.address, address2, sizeof(address2));
}

static void mt6797_consys_wmt_reject_test(struct kunit *test)
{
	u8 patch[MT6797_CONSYS_WMT_HEADER_SIZE + 1] = { };
	struct mt6797_consys_wmt_metadata metadata;
	int ret;

	patch[24] = 0x21;
	ret = mt6797_consys_wmt_parse(patch, MT6797_CONSYS_WMT_HEADER_SIZE, 1,
				      &metadata);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	ret = mt6797_consys_wmt_parse(patch, sizeof(patch), 2, &metadata);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	patch[24] = 0x31;
	ret = mt6797_consys_wmt_parse(patch, sizeof(patch), 1, &metadata);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
}

static struct kunit_case mt6797_consys_fw_test_cases[] = {
	KUNIT_CASE(mt6797_consys_fw_verify_test),
	KUNIT_CASE(mt6797_consys_wmt_parse_test),
	KUNIT_CASE(mt6797_consys_wmt_reject_test),
	{ }
};

static struct kunit_suite mt6797_consys_fw_test_suite = {
	.name = "mt6797-consys-fw-handoff",
	.test_cases = mt6797_consys_fw_test_cases,
};

kunit_test_suite(mt6797_consys_fw_test_suite);

MODULE_LICENSE("GPL");
