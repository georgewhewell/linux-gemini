// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w0-phase.h>

#define MT6797_WIFI_W1_FIXTURE_SIZE	411632U

static const u8 mt6797_wifi_w1_sha256_abc[32] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
	0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
	0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static void mt6797_wifi_w1_accepts_exact_size_and_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image) - 1,
						       mt6797_wifi_w1_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void mt6797_wifi_w1_rejects_wrong_size_before_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image),
						       mt6797_wifi_w1_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, -EFBIG);
}

static void mt6797_wifi_w1_rejects_wrong_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	u8 wrong_digest[sizeof(mt6797_wifi_w1_sha256_abc)] = { };
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image) - 1, wrong_digest);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static void mt6797_wifi_w1_fixture_header(u8 *fixture)
{
	static const struct mt6797_wifi_ahb_w1_section section[] = {
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
		MT6797_WIFI_W1_FIXTURE_SIZE - 8), fixture + 4);
}

static void mt6797_wifi_w1_parses_only_exact_divided_header(struct kunit *test)
{
	struct mt6797_wifi_ahb_w1_image image;
	u8 *fixture;

	fixture = kunit_kzalloc(test, MT6797_WIFI_W1_FIXTURE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture);
	mt6797_wifi_w1_fixture_header(fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_w1_firmware_parse(fixture,
		MT6797_WIFI_W1_FIXTURE_SIZE, &image), 0);
	KUNIT_EXPECT_EQ(test, image.section[0].length, 0x16d0U);
	KUNIT_EXPECT_TRUE(test, image.section[0].encrypted);
	KUNIT_EXPECT_EQ(test, image.section[1].length, 0x2320U);
	KUNIT_EXPECT_TRUE(test, image.section[1].encrypted);

	fixture[24 + 16 + 5] = 2;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
		MT6797_WIFI_W1_FIXTURE_SIZE - 8), fixture + 4);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w1_firmware_parse(fixture,
		MT6797_WIFI_W1_FIXTURE_SIZE, &image), -EBADMSG);

	mt6797_wifi_w1_fixture_header(fixture);
	fixture[12] = 1;
	put_unaligned_le32(~crc32_le(~0, fixture + 8,
		MT6797_WIFI_W1_FIXTURE_SIZE - 8), fixture + 4);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w1_firmware_parse(fixture,
		MT6797_WIFI_W1_FIXTURE_SIZE, &image), -EBADMSG);
}

static void mt6797_wifi_w1_accounts_for_every_idlm_frame(struct kunit *test)
{
	struct mt6797_wifi_ahb_w1_image image = {
		.section = {
			{ .length = 0x16d0 }, { .length = 0x2320 },
		},
	};
	u32 frames;
	size_t bytes;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_w1_idlm_transfer_account(&image,
		&frames, &bytes), 0);
	KUNIT_EXPECT_EQ(test, frames, 8U);
	KUNIT_EXPECT_EQ(test, bytes, 18432UL);
}

static void mt6797_wifi_w2_capability_fixture(u8 *event)
{
	static const u8 mac[] = { 0x02, 0x79, 0x67, 0x00, 0x00, 0x01 };

	memset(event, 0, MT6797_WIFI_AHB_W2_CAPABILITY_LENGTH);
	put_unaligned_le16(MT6797_WIFI_AHB_W2_CAPABILITY_LENGTH, event);
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

static void mt6797_wifi_w2_builds_exact_capability_command(struct kunit *test)
{
	u8 command[MT6797_WIFI_AHB_W2_CAPABILITY_LENGTH];

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_w2_capability_command_build(command,
		sizeof(command), 1), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command), 124U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 2), 0x8000U);
	KUNIT_EXPECT_EQ(test, command[4], 0x80U);
	KUNIT_EXPECT_EQ(test, command[5], 0xa0U);
	KUNIT_EXPECT_EQ(test, command[6], 0U);
	KUNIT_EXPECT_EQ(test, command[7], 1U);
	KUNIT_EXPECT_EQ(test, command[123], 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_command_build(command,
		sizeof(command) - 1, 1), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_command_build(command,
		sizeof(command), 2), -EINVAL);
}

static void mt6797_wifi_w2_parses_only_exact_capability_event(struct kunit *test)
{
	struct mt6797_wifi_ahb_w2_capability capability;
	u8 event[MT6797_WIFI_AHB_W2_CAPABILITY_LENGTH];

	mt6797_wifi_w2_capability_fixture(event);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_w2_capability_event_parse(event,
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
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_event_parse(event,
		sizeof(event), 1, &capability), -EMSGSIZE);
	mt6797_wifi_w2_capability_fixture(event);
	put_unaligned_le16(0x0000, event + 2);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
	mt6797_wifi_w2_capability_fixture(event);
	event[4] = 2;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
	mt6797_wifi_w2_capability_fixture(event);
	event[5] = 2;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w2_capability_event_parse(event,
		sizeof(event), 1, &capability), -EPROTO);
}

struct mt6797_wifi_w1_cleanup_test {
	int voltage;
	int bt_enabled;
	int wifi_enabled;
	unsigned int disables;
	int disable_fail;
};

static int mt6797_wifi_w1_cleanup_voltage(void *context)
{
	return ((struct mt6797_wifi_w1_cleanup_test *)context)->voltage;
}

static int mt6797_wifi_w1_cleanup_enabled(void *context, bool wifi)
{
	struct mt6797_wifi_w1_cleanup_test *fake = context;

	return wifi ? fake->wifi_enabled : fake->bt_enabled;
}

static int mt6797_wifi_w1_cleanup_set(void *context, int voltage)
{
	((struct mt6797_wifi_w1_cleanup_test *)context)->voltage = voltage;
	return 0;
}

static int mt6797_wifi_w1_cleanup_enable(void *context)
{
	((struct mt6797_wifi_w1_cleanup_test *)context)->wifi_enabled = 1;
	return 0;
}

static int mt6797_wifi_w1_cleanup_disable(void *context)
{
	struct mt6797_wifi_w1_cleanup_test *fake = context;

	fake->disables++;
	if (fake->disable_fail)
		return fake->disable_fail;
	fake->wifi_enabled = 0;
	return 0;
}

static int mt6797_wifi_w1_cleanup_stream_fails(void *context)
{
	return -EPROTO;
}

static void mt6797_wifi_w1_stream_failure_unwinds_paldo(struct kunit *test)
{
	struct mt6797_wifi_w1_cleanup_test fake = { .voltage = 1800000 };
	struct mt6797_wifi_ahb_w0_result result;
	struct mt6797_wifi_ahb_w0_ops ops = {
		.get_voltage = mt6797_wifi_w1_cleanup_voltage,
		.is_enabled = mt6797_wifi_w1_cleanup_enabled,
		.set_voltage = mt6797_wifi_w1_cleanup_set,
		.enable = mt6797_wifi_w1_cleanup_enable,
		.disable = mt6797_wifi_w1_cleanup_disable,
		.observe = mt6797_wifi_w1_cleanup_stream_fails,
		.context = &fake,
	};

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, fake.disables, 1U);
	KUNIT_EXPECT_EQ(test, fake.voltage, 1800000);
	KUNIT_EXPECT_EQ(test, result.cleanup, 0);
	KUNIT_EXPECT_FALSE(test, result.fatal);
}

static void mt6797_wifi_w1_cleanup_failure_is_fatal(struct kunit *test)
{
	struct mt6797_wifi_w1_cleanup_test fake = {
		.voltage = 1800000,
		.disable_fail = -EIO,
	};
	struct mt6797_wifi_ahb_w0_result result;
	struct mt6797_wifi_ahb_w0_ops ops = {
		.get_voltage = mt6797_wifi_w1_cleanup_voltage,
		.is_enabled = mt6797_wifi_w1_cleanup_enabled,
		.set_voltage = mt6797_wifi_w1_cleanup_set,
		.enable = mt6797_wifi_w1_cleanup_enable,
		.disable = mt6797_wifi_w1_cleanup_disable,
		.observe = mt6797_wifi_w1_cleanup_stream_fails,
		.context = &fake,
	};

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w0_run(&ops, &result, 3300000),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, result.causal, -EPROTO);
	KUNIT_EXPECT_EQ(test, result.cleanup, -EIO);
	KUNIT_EXPECT_TRUE(test, result.fatal);
	KUNIT_EXPECT_TRUE(test, result.selector_dirty);
}

static struct kunit_case mt6797_wifi_w1_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_w1_accepts_exact_size_and_digest),
	KUNIT_CASE(mt6797_wifi_w1_rejects_wrong_size_before_digest),
	KUNIT_CASE(mt6797_wifi_w1_rejects_wrong_digest),
	KUNIT_CASE(mt6797_wifi_w1_parses_only_exact_divided_header),
	KUNIT_CASE(mt6797_wifi_w1_accounts_for_every_idlm_frame),
	KUNIT_CASE(mt6797_wifi_w2_builds_exact_capability_command),
	KUNIT_CASE(mt6797_wifi_w2_parses_only_exact_capability_event),
	KUNIT_CASE(mt6797_wifi_w1_stream_failure_unwinds_paldo),
	KUNIT_CASE(mt6797_wifi_w1_cleanup_failure_is_fatal),
	{}
};

static struct kunit_suite mt6797_wifi_w1_test_suite = {
	.name = "mt6797-wifi-ahb-w1-firmware",
	.test_cases = mt6797_wifi_w1_test_cases,
};

kunit_test_suite(mt6797_wifi_w1_test_suite);

MODULE_LICENSE("GPL");
