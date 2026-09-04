// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"

static void mt6797_wifi_command_test(struct kunit *test)
{
	u8 command[16];
	int ret;

	ret = mt6797_wifi_command_init(command, sizeof(command), 8, 2, true,
				       0x5a);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command), (u16)sizeof(command));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 2), (u16)0x8000);
	KUNIT_EXPECT_EQ(test, command[4], (u8)2);
	KUNIT_EXPECT_EQ(test, command[5], (u8)0xa0);
	KUNIT_EXPECT_EQ(test, command[6], (u8)1);
	KUNIT_EXPECT_EQ(test, command[7], (u8)0x5a);
}

static void mt6797_wifi_basic_config_test(struct kunit *test)
{
	u8 expected[MT6797_WIFI_BASIC_CONFIG_LENGTH] = {
		0x10, 0x00, 0x00, 0x80, 0x02, 0xa0, 0x01, 0x02,
	};
	u8 command[MT6797_WIFI_BASIC_CONFIG_LENGTH];
	int ret;

	ret = mt6797_wifi_basic_config_build(command, sizeof(command), 2);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));

	ret = mt6797_wifi_basic_config_build(command, sizeof(command) - 1, 2);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_wifi_command_bounds_test(struct kunit *test)
{
	u8 command[MT6797_WIFI_COMMAND_HEADER_LENGTH];
	int ret;

	ret = mt6797_wifi_command_init(NULL, sizeof(command), 0, 1, false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_wifi_command_init(command, sizeof(command) - 1, 0, 1,
				       false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	ret = mt6797_wifi_command_init(command, SIZE_MAX, SIZE_MAX, 1, false,
				       0);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
	ret = mt6797_wifi_command_init(command, SIZE_MAX,
				       MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH,
				       1, false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_wifi_event_test(struct kunit *test)
{
	u8 packet[12] = {
		0x0c, 0x00, 0x00, 0xe0, 0x27, 0x5a, 0x00, 0x00,
		0x11, 0x22, 0x33, 0x44,
	};
	struct mt6797_wifi_event_view event;
	int ret;

	ret = mt6797_wifi_event_parse(packet, sizeof(packet), &event);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, event.id, (u8)0x27);
	KUNIT_EXPECT_EQ(test, event.sequence, (u8)0x5a);
	KUNIT_EXPECT_EQ(test, event.packet_length, (size_t)12);
	KUNIT_EXPECT_EQ(test, event.payload_length, (size_t)4);
	KUNIT_EXPECT_PTR_EQ(test, event.payload, packet + 8);
	KUNIT_EXPECT_MEMEQ(test, event.payload, packet + 8, 4);
}

static void mt6797_wifi_event_bounds_test(struct kunit *test)
{
	u8 packet[12] = {
		0x0c, 0x00, 0x00, 0xe0, 0x27, 0x5a, 0x00, 0x00,
	};
	struct mt6797_wifi_event_view event;

	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(NULL, sizeof(packet),
						      &event), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, 7, &event),
			-EMSGSIZE);
	put_unaligned_le16(7, packet);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EMSGSIZE);
	put_unaligned_le16(13, packet);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EMSGSIZE);
	put_unaligned_le16(sizeof(packet), packet);
	packet[2] = 1;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EPROTO);
	packet[2] = 0;
	packet[6] = 1;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EPROTO);
}

static struct kunit_case mt6797_wifi_protocol_cases[] = {
	KUNIT_CASE(mt6797_wifi_command_test),
	KUNIT_CASE(mt6797_wifi_basic_config_test),
	KUNIT_CASE(mt6797_wifi_command_bounds_test),
	KUNIT_CASE(mt6797_wifi_event_test),
	KUNIT_CASE(mt6797_wifi_event_bounds_test),
	{}
};

static struct kunit_suite mt6797_wifi_protocol_suite = {
	.name = "mt6797-wifi-protocol",
	.test_cases = mt6797_wifi_protocol_cases,
};

kunit_test_suite(mt6797_wifi_protocol_suite);

MODULE_LICENSE("GPL");
