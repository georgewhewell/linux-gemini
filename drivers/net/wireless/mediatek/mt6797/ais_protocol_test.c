// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_protocol.h"

static const u8 mt6797_wifi_ais_test_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_ais_test_peer_addr[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static void mt6797_wifi_ais_test_bss(
	struct kunit *test, struct mt6797_wifi_ais_resources *resources,
	struct mt6797_wifi_ais_bss_handle *bss)
{
	mt6797_wifi_ais_resources_init(resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(resources), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_reserve(resources, 3, 1,
			mt6797_wifi_ais_test_own_addr, bss), 0);
}

static void mt6797_wifi_ais_test_sta(
	struct kunit *test, struct mt6797_wifi_ais_resources *resources,
	struct mt6797_wifi_ais_bss_handle *bss,
	struct mt6797_wifi_ais_sta_handle *sta)
{
	mt6797_wifi_ais_test_bss(test, resources, bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_reserve(resources, bss,
			mt6797_wifi_ais_test_peer_addr, sta), 0);
}

static void mt6797_wifi_ais_bss_commands_test(struct kunit *test)
{
	static const u8 activate_expected[MT6797_WIFI_AIS_BSS_COMMAND_LENGTH] = {
		0x14, 0x00, 0x00, 0x80, 0x11, 0xa0, 0x01, 0x09,
		0x03, 0x01, 0x00, 0x01, 0x02, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x00, 0x00,
	};
	static const u8 deactivate_expected[MT6797_WIFI_AIS_BSS_COMMAND_LENGTH] = {
		0x14, 0x00, 0x00, 0x80, 0x11, 0xa0, 0x01, 0x0a,
		0x03, 0x00, 0x00, 0x01,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	u8 command[MT6797_WIFI_AIS_BSS_COMMAND_LENGTH];

	mt6797_wifi_ais_test_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_activate_build(&bss, 9, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, activate_expected, sizeof(command));
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_deactivate_build(&bss, 10, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, deactivate_expected, sizeof(command));
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_bss_activate_build(&bss, 9, command,
			sizeof(command) - 1), -EMSGSIZE);
}

static void mt6797_wifi_ais_sta_commands_test(struct kunit *test)
{
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 expected[MT6797_WIFI_AIS_STA_UPDATE_LENGTH] = {
		0x84, 0x00, 0x00, 0x80, 0x13, 0xa0, 0x01, 0x0b,
		[9] = 0x41,
		[10] = 0x02, [11] = 0xaa, [12] = 0xbb, [13] = 0xcc,
		[14] = 0xdd, [15] = 0xee, [20] = 0x03,
		[62] = 0x01, [63] = 0x02,
	};
	static const u8 remove_expected[MT6797_WIFI_AIS_STA_REMOVE_LENGTH] = {
		0x0c, 0x00, 0x00, 0x80, 0x14, 0xa0, 0x01, 0x0c,
		0x00, 0x00, 0x03, 0x00,
	};
	u8 command[MT6797_WIFI_AIS_STA_UPDATE_LENGTH];

	mt6797_wifi_ais_test_sta(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_state1_build(&bss, &sta, 11, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));
	memset(command, 0xa5, sizeof(command));
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_remove_build(&bss, &sta, 12, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, remove_expected,
			   sizeof(remove_expected));
}

static void mt6797_wifi_ais_channel_commands_test(struct kunit *test)
{
	static const u8 request_expected[MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH] = {
		0x18, 0x00, 0x00, 0x80, 0x1c, 0xa0, 0x01, 0x0d,
		0x03, 0x07, 0x00, 0x24, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xa0, 0x0f, 0x00, 0x00,
	};
	static const u8 abort_expected[MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH] = {
		0x18, 0x00, 0x00, 0x80, 0x1c, 0xa0, 0x01, 0x0e,
		0x03, 0x07, 0x01,
	};
	const struct mt6797_wifi_ais_channel channel = {
		.band = MT6797_WIFI_AIS_BAND_5GHZ,
		.primary = 36,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_channel_pending pending;
	struct mt6797_wifi_ais_bss_handle bss;
	u8 command[MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH];

	mt6797_wifi_ais_test_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_request_build(&bss, &channel, 7, 13,
			command, sizeof(command), &pending), 0);
	KUNIT_EXPECT_MEMEQ(test, command, request_expected, sizeof(command));
	KUNIT_EXPECT_EQ(test, pending.generation, bss.generation);
	KUNIT_EXPECT_EQ(test, pending.bss_cookie, bss.cookie);
	KUNIT_EXPECT_EQ(test, pending.max_interval_ms,
			(u32)MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS);
	KUNIT_EXPECT_EQ(test, pending.token, (u8)7);
	KUNIT_EXPECT_EQ(test, pending.primary, (u8)36);
	memset(command, 0xa5, sizeof(command));
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_abort_build(&bss, 7, 14, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, abort_expected, sizeof(command));
}

static void mt6797_wifi_ais_builder_rejects_invalid_test(struct kunit *test)
{
	struct mt6797_wifi_ais_channel channel = {
		.band = MT6797_WIFI_AIS_BAND_2GHZ,
		.primary = 15,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_channel_pending pending = {
		.generation = 99,
	};
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 command[MT6797_WIFI_AIS_STA_UPDATE_LENGTH];

	mt6797_wifi_ais_test_sta(test, &resources, &bss, &sta);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_channel_request_build(&bss, &channel, 1, 1,
			command, sizeof(command), &pending), -EINVAL);
	KUNIT_EXPECT_EQ(test, pending.generation, (u64)0);
	sta.pairwise.role = MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_state1_build(&bss, &sta, 1, command,
			sizeof(command)), -EINVAL);
}

static void mt6797_wifi_ais_sta_event_test(struct kunit *test)
{
	u8 payload[8] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x00, 0x03,
	};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_STA_EVENT_LENGTH,
		.id = 0x0c,
		.sequence = 15,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_sta_pending pending;
	struct mt6797_wifi_ais_sta_pending original;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;

	mt6797_wifi_ais_test_sta(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_pending_init(&bss, &sta, 15, &pending), 0);
	original = pending;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_event_match(&pending, &event, &disposition), 0);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_MEMEQ(test, &pending, &original, sizeof(pending));

	event.sequence++;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_event_match(&pending, &event, &disposition), 0);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_IGNORED);
	event.sequence = pending.command_sequence;
	event.payload_length--;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_event_match(&pending, &event, &disposition),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_TERMINAL);
}

static void mt6797_wifi_ais_sta_event_mismatch_test(struct kunit *test)
{
	u8 payload[8] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xef, 0x00, 0x03,
	};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_STA_EVENT_LENGTH,
		.id = 0x0c,
		.sequence = 15,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_sta_pending pending;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;

	mt6797_wifi_ais_test_sta(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_pending_init(&bss, &sta, 15, &pending), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_event_match(&pending, &event, &disposition),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_TERMINAL);
	event.id = 0x10;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_event_match(&pending, &event, &disposition),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_TERMINAL);
}

static void mt6797_wifi_ais_channel_event_test(struct kunit *test)
{
	u8 payload[16] = {
		0x03, 0x07, 0x00, 0x24, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
	};
	const struct mt6797_wifi_ais_channel channel = {
		.band = MT6797_WIFI_AIS_BAND_5GHZ,
		.primary = 36,
	};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH,
		.id = 0x10,
		.sequence = 99,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_channel_pending pending;
	struct mt6797_wifi_ais_channel_pending original;
	struct mt6797_wifi_ais_bss_handle bss;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 command[MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH];

	mt6797_wifi_ais_test_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_request_build(&bss, &channel, 7, 13,
			command, sizeof(command), &pending), 0);
	original = pending;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_event_match(&pending, &event,
			&disposition), 0);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_MEMEQ(test, &pending, &original, sizeof(pending));

	payload[1]++;
	event.payload_length = 2;
	event.packet_length = 10;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_event_match(&pending, &event,
			&disposition), 0);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_IGNORED);
}

static void mt6797_wifi_ais_channel_event_terminal_test(struct kunit *test)
{
	u8 payload[16] = {
		0x03, 0x07, 0x00, 0x24, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
	};
	const struct mt6797_wifi_ais_channel channel = {
		.band = MT6797_WIFI_AIS_BAND_5GHZ,
		.primary = 36,
	};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH,
		.id = 0x10,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_channel_pending pending;
	struct mt6797_wifi_ais_bss_handle bss;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 command[MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH];

	mt6797_wifi_ais_test_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_channel_request_build(&bss, &channel, 7, 13,
			command, sizeof(command), &pending), 0);
	event.payload_length = 1;
	event.packet_length = 9;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_channel_event_match(&pending, &event,
			&disposition), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition,
			(enum mt6797_wifi_ais_event_disposition)
			MT6797_WIFI_AIS_EVENT_TERMINAL);

	event.payload_length = sizeof(payload);
	event.packet_length = MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH;
	payload[2] = 1;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_channel_event_match(&pending, &event,
			&disposition), -EPROTO);
	payload[2] = 0;
	put_unaligned_le32(MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS + 1,
			   payload + 12);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_channel_event_match(&pending, &event,
			&disposition), -EPROTO);
}

static struct kunit_case mt6797_wifi_ais_protocol_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_bss_commands_test),
	KUNIT_CASE(mt6797_wifi_ais_sta_commands_test),
	KUNIT_CASE(mt6797_wifi_ais_channel_commands_test),
	KUNIT_CASE(mt6797_wifi_ais_builder_rejects_invalid_test),
	KUNIT_CASE(mt6797_wifi_ais_sta_event_test),
	KUNIT_CASE(mt6797_wifi_ais_sta_event_mismatch_test),
	KUNIT_CASE(mt6797_wifi_ais_channel_event_test),
	KUNIT_CASE(mt6797_wifi_ais_channel_event_terminal_test),
	{}
};

static struct kunit_suite mt6797_wifi_ais_protocol_suite = {
	.name = "mt6797-wifi-ais-protocol",
	.test_cases = mt6797_wifi_ais_protocol_cases,
};

kunit_test_suite(mt6797_wifi_ais_protocol_suite);

MODULE_LICENSE("GPL");
