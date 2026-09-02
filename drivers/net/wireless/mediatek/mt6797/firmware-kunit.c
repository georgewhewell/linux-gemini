// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "mt6797.h"

#define MT6797_WIFI_FW_FIXTURE_SIZE	411632U

static void mt6797_wifi_fw_fixture_header(u8 *fixture)
{
	static const struct mt6797_wifi_fw_section section[] = {
		{ .offset = 0x58, .length = 0x16d0, .destination = 0x020a0000,
		  .key_index = 0, .encrypted = true },
		{ .offset = 0x1728, .length = 0x2320, .destination = 0x000a2800,
		  .key_index = 0, .encrypted = true },
		{ .offset = 0x3a48, .length = 0x50e20, .destination = 0xf0000000,
		  .key_index = 0, .encrypted = false },
		{ .offset = 0x54868, .length = 0xff70, .destination = 0xf005c000,
		  .key_index = 0, .encrypted = false },
	};
	unsigned int i;

	put_unaligned_le32(0x454b544d, fixture);
	put_unaligned_le32(ARRAY_SIZE(section), fixture + 8);
	for (i = 0; i < ARRAY_SIZE(section); i++) {
		u8 *entry = fixture + 24 + i * 16;

		put_unaligned_le32(section[i].offset, entry);
		entry[4] = section[i].key_index;
		entry[5] = section[i].encrypted;
		put_unaligned_le32(section[i].length, entry + 8);
		put_unaligned_le32(section[i].destination, entry + 12);
	}
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
		MT6797_WIFI_FW_FIXTURE_SIZE - 8), fixture + 4);
}

static void mt6797_wifi_fw_parses_only_exact_divided_header(struct kunit *test)
{
	struct mt6797_wifi_fw_image image;
	u8 *fixture;

	fixture = kunit_kzalloc(test, MT6797_WIFI_FW_FIXTURE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture);
	mt6797_wifi_fw_fixture_header(fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_firmware_parse(fixture,
		MT6797_WIFI_FW_FIXTURE_SIZE, &image), 0);
	KUNIT_EXPECT_EQ(test, image.section[0].length, 0x16d0U);
	KUNIT_EXPECT_TRUE(test, image.section[0].encrypted);
	KUNIT_EXPECT_EQ(test, image.section[1].length, 0x2320U);
	KUNIT_EXPECT_TRUE(test, image.section[1].encrypted);

	fixture[24 + 16 + 5] = 2;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
		MT6797_WIFI_FW_FIXTURE_SIZE - 8), fixture + 4);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_firmware_parse(fixture,
		MT6797_WIFI_FW_FIXTURE_SIZE, &image), -EBADMSG);

	mt6797_wifi_fw_fixture_header(fixture);
	fixture[12] = 1;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
		MT6797_WIFI_FW_FIXTURE_SIZE - 8), fixture + 4);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_firmware_parse(fixture,
		MT6797_WIFI_FW_FIXTURE_SIZE, &image), -EBADMSG);
}

static void mt6797_wifi_fw_accounts_for_every_idlm_frame(struct kunit *test)
{
	struct mt6797_wifi_fw_image image = {
		.section = {
			{ .length = 0x16d0 }, { .length = 0x2320 },
		},
	};
	u32 frames;
	size_t bytes;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_fw_idlm_transfer_account(&image,
		&frames, &bytes), 0);
	KUNIT_EXPECT_EQ(test, frames, 8U);
	KUNIT_EXPECT_EQ(test, bytes, 18432UL);
}

static void mt6797_wifi_capability_fixture(u8 *event)
{
	static const u8 mac[] = { 0x02, 0x79, 0x67, 0x00, 0x00, 0x01 };

	memset(event, 0, MT6797_WIFI_CAPABILITY_LENGTH);
	put_unaligned_le16(MT6797_WIFI_CAPABILITY_LENGTH, event);
	put_unaligned_le16(0xe000, event + 2);
	event[4] = 0x01;
	event[5] = 1;
	put_unaligned_le16(0x0279, event + 8);
	put_unaligned_le16(0x1234, event + 10);
	put_unaligned_le16(0x5678, event + 12);
	memcpy(event + 16, mac, sizeof(mac));
	put_unaligned_le32(0x11223344, event + 44);
	put_unaligned_le32(0x55667788, event + 48);
	put_unaligned_le32(0x99aabbcc, event + 52);
	put_unaligned_le32(0xddeeff00, event + 56);
}

static void mt6797_wifi_cap_builds_exact_capability_command(struct kunit *test)
{
	u8 command[MT6797_WIFI_CAPABILITY_LENGTH];

	KUNIT_ASSERT_EQ(test, mt6797_wifi_capability_command_build(command,
		sizeof(command), 1), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command), 124U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 2), 0x8000U);
	KUNIT_EXPECT_EQ(test, command[4], 0x80U);
	KUNIT_EXPECT_EQ(test, command[5], 0xa0U);
	KUNIT_EXPECT_EQ(test, command[6], 0U);
	KUNIT_EXPECT_EQ(test, command[7], 1U);
	KUNIT_EXPECT_EQ(test, command[123], 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_command_build(command,
		sizeof(command) - 1, 1), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_command_build(command,
		sizeof(command), 2), -EINVAL);
}

static void mt6797_wifi_cap_parses_only_exact_capability_event(struct kunit *test)
{
	struct mt6797_wifi_capability capability;
	u8 event[MT6797_WIFI_CAPABILITY_LENGTH];

	mt6797_wifi_capability_fixture(event);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_capability_event_parse(event,
		sizeof(event), 1, &capability), 0);
	KUNIT_EXPECT_EQ(test, capability.product_id, 0x0279U);
	KUNIT_EXPECT_EQ(test, capability.firmware_version, 0x1234U);
	KUNIT_EXPECT_EQ(test, capability.driver_version, 0x5678U);
	KUNIT_EXPECT_EQ(test, capability.mac_address[0], 0x02U);
	KUNIT_EXPECT_EQ(test, capability.mac_address[5], 0x01U);
	KUNIT_EXPECT_EQ(test, capability.feature_flag0, 0x11223344U);
	KUNIT_EXPECT_EQ(test, capability.feature_flag1, 0x55667788U);
	KUNIT_EXPECT_EQ(test, capability.compile_flag0, 0x99aabbccU);
	KUNIT_EXPECT_EQ(test, capability.compile_flag1, 0xddeeff00U);

	put_unaligned_le16(123, event);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_event_parse(event,
		sizeof(event), 1, &capability), -EMSGSIZE);
	mt6797_wifi_capability_fixture(event);
	put_unaligned_le16(0x0000, event + 2);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
	mt6797_wifi_capability_fixture(event);
	event[4] = 2;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
	mt6797_wifi_capability_fixture(event);
	event[5] = 2;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
}

static struct kunit_case mt6797_wifi_fw_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_fw_parses_only_exact_divided_header),
	KUNIT_CASE(mt6797_wifi_fw_accounts_for_every_idlm_frame),
	KUNIT_CASE(mt6797_wifi_cap_builds_exact_capability_command),
	KUNIT_CASE(mt6797_wifi_cap_parses_only_exact_capability_event),
	{}
};

static struct kunit_suite mt6797_wifi_fw_test_suite = {
	.name = "mt6797-wifi-firmware",
	.test_cases = mt6797_wifi_fw_test_cases,
};

kunit_test_suite(mt6797_wifi_fw_test_suite);

MODULE_LICENSE("GPL");
