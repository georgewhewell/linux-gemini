// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "firmware.h"

static const u8 mt6797_wifi_sha256_abc[MT6797_WIFI_SHA256_SIZE] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
	0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
	0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static const struct mt6797_wifi_section mt6797_wifi_test_sections[] = {
	{ .offset = 0x58, .length = 0x16d0, .destination = 0x020a0000,
	  .encrypted = true },
	{ .offset = 0x1728, .length = 0x2320, .destination = 0x000a2800,
	  .encrypted = true },
	{ .offset = 0x3a48, .length = 0x50e20, .destination = 0xf0000000 },
	{ .offset = 0x54868, .length = 0xff70, .destination = 0xf005c000 },
};

static void mt6797_wifi_fixture_init(u8 *fixture)
{
	unsigned int i;

	put_unaligned_le32(0x454b544d, fixture);
	put_unaligned_le32(ARRAY_SIZE(mt6797_wifi_test_sections), fixture + 8);
	for (i = 0; i < ARRAY_SIZE(mt6797_wifi_test_sections); i++) {
		const struct mt6797_wifi_section *section =
			&mt6797_wifi_test_sections[i];
		u8 *entry = fixture + 24 + i * 16;

		put_unaligned_le32(section->offset, entry);
		entry[4] = section->key_index;
		entry[5] = section->encrypted;
		put_unaligned_le32(section->length, entry + 8);
		put_unaligned_le32(section->destination, entry + 12);
	}
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
				     MT6797_WIFI_FIRMWARE_SIZE - 8),
			   fixture + 4);
}

static void mt6797_wifi_verify_test(struct kunit *test)
{
	static const u8 data[] = "abc";
	u8 wrong_digest[MT6797_WIFI_SHA256_SIZE] = { };
	int ret;

	ret = mt6797_wifi_firmware_verify(data, 3, 3, mt6797_wifi_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_wifi_firmware_verify(data, 3, 4, mt6797_wifi_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	ret = mt6797_wifi_firmware_verify(data, 3, 3, wrong_digest);
	KUNIT_EXPECT_EQ(test, ret, -EKEYREJECTED);
}

static void mt6797_wifi_parse_test(struct kunit *test)
{
	struct mt6797_wifi_section sections[MT6797_WIFI_SECTION_COUNT];
	u8 *fixture;
	int ret;

	fixture = kunit_kzalloc(test, MT6797_WIFI_FIRMWARE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture);
	mt6797_wifi_fixture_init(fixture);
	ret = mt6797_wifi_firmware_parse(fixture, MT6797_WIFI_FIRMWARE_SIZE,
					 sections, ARRAY_SIZE(sections));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sections[0].length, 0x16d0U);
	KUNIT_EXPECT_TRUE(test, sections[0].encrypted);
	KUNIT_EXPECT_EQ(test, sections[3].destination, 0xf005c000U);
}

static void mt6797_wifi_parse_reject_test(struct kunit *test)
{
	struct mt6797_wifi_section sections[MT6797_WIFI_SECTION_COUNT];
	u8 *fixture;
	int ret;

	fixture = kunit_kzalloc(test, MT6797_WIFI_FIRMWARE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture);
	mt6797_wifi_fixture_init(fixture);
	fixture[24 + 5] = 2;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
				     MT6797_WIFI_FIRMWARE_SIZE - 8),
			   fixture + 4);
	ret = mt6797_wifi_firmware_parse(fixture, MT6797_WIFI_FIRMWARE_SIZE,
					 sections, ARRAY_SIZE(sections));
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);

	mt6797_wifi_fixture_init(fixture);
	fixture[12] = 1;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
				     MT6797_WIFI_FIRMWARE_SIZE - 8),
			   fixture + 4);
	ret = mt6797_wifi_firmware_parse(fixture, MT6797_WIFI_FIRMWARE_SIZE,
					 sections, ARRAY_SIZE(sections));
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static struct kunit_case mt6797_wifi_firmware_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_verify_test),
	KUNIT_CASE(mt6797_wifi_parse_test),
	KUNIT_CASE(mt6797_wifi_parse_reject_test),
	{ }
};

static struct kunit_suite mt6797_wifi_firmware_test_suite = {
	.name = "mt6797-wifi-firmware",
	.test_cases = mt6797_wifi_firmware_test_cases,
};

kunit_test_suite(mt6797_wifi_firmware_test_suite);

MODULE_LICENSE("GPL");
