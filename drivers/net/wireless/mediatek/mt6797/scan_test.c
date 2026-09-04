// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/unaligned.h>

#include "scan.h"

static void mt6797_wifi_scan_start_test(struct kunit *test)
{
	static const u8 expected[MT6797_WIFI_SCAN_COMMAND_LENGTH] = {
		[0] = 0xec, [3] = 0x80, [4] = 0x03, [5] = 0xa0,
		[6] = 1, [7] = 3, [8] = 1, [11] = BIT(0),
		[166] = 4, [167] = 2, [168] = 1, [169] = 1,
		[170] = 2, [171] = 36,
	};
	static const struct mt6797_wifi_scan_channel channels[] = {
		{ MT6797_WIFI_SCAN_BAND_2GHZ, 1 },
		{ MT6797_WIFI_SCAN_BAND_5GHZ, 36 },
	};
	struct mt6797_wifi_scan_transaction transaction = {};
	u8 command[MT6797_WIFI_SCAN_COMMAND_LENGTH];

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels,
			ARRAY_SIZE(channels), 3, 1, command, sizeof(command)), 0);
	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_WAIT_DONE);
}

static void mt6797_wifi_scan_start_rejects_bounds_test(struct kunit *test)
{
	struct mt6797_wifi_scan_channel channels[MT6797_WIFI_SCAN_MAX_CHANNELS + 1] = {
		{ MT6797_WIFI_SCAN_BAND_2GHZ, 1 },
	};
	struct mt6797_wifi_scan_transaction transaction = {};
	u8 command[MT6797_WIFI_SCAN_COMMAND_LENGTH];

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels, 0, 3, 1,
			command, sizeof(command)), -EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels,
			ARRAY_SIZE(channels), 3, 1, command, sizeof(command)),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels, 1, 3, 1,
			command, sizeof(command) - 1), -EMSGSIZE);
	channels[0].number = 15;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels, 1, 3, 1,
			command, sizeof(command)), -EINVAL);
	channels[0] = (struct mt6797_wifi_scan_channel) {
		MT6797_WIFI_SCAN_BAND_5GHZ, 0,
	};
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels, 1, 3, 1,
			command, sizeof(command)), -EINVAL);
	channels[0] = (struct mt6797_wifi_scan_channel) {
		MT6797_WIFI_SCAN_BAND_2GHZ, 1,
	};
	channels[1] = channels[0];
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_start(&transaction, channels, 2, 3, 1,
			command, sizeof(command)), -EINVAL);
}

static void mt6797_wifi_scan_event_test(struct kunit *test)
{
	u8 payload[4] = { 7, 0, 0, 0 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.id = 0x0d,
		.sequence = 99,
	};
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};
	enum mt6797_wifi_scan_event_result result;

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_event(&transaction, &event, &result), 0);
	KUNIT_EXPECT_EQ(test, result,
			(enum mt6797_wifi_scan_event_result)
			MT6797_WIFI_SCAN_EVENT_COMPLETE);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_DONE);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_reset(&transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_IDLE);
}

static void mt6797_wifi_scan_foreign_event_test(struct kunit *test)
{
	u8 payload[4] = { 6, 0, 0, 0 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.id = 0x0d,
	};
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};
	enum mt6797_wifi_scan_event_result result;

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_event(&transaction, &event, &result), 0);
	KUNIT_EXPECT_EQ(test, result,
			(enum mt6797_wifi_scan_event_result)
			MT6797_WIFI_SCAN_EVENT_IGNORED);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_WAIT_DONE);
	event.payload_length = 1;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_event(&transaction, &event, &result), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_WAIT_DONE);
	event.id = 0x27;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_event(&transaction, &event, &result), 0);
}

static void mt6797_wifi_scan_malformed_event_test(struct kunit *test)
{
	u8 payload[3] = { 7 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.id = 0x0d,
	};
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};
	enum mt6797_wifi_scan_event_result result;

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_event(&transaction, &event, &result), -EPROTO);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_TERMINAL);
}

static void mt6797_wifi_scan_timeout_test(struct kunit *test)
{
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};
	u8 command[MT6797_WIFI_SCAN_CANCEL_LENGTH];

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_timeout(&transaction, 4, command,
			sizeof(command)), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command),
			(u16)MT6797_WIFI_SCAN_CANCEL_LENGTH);
	KUNIT_EXPECT_EQ(test, command[4], (u8)0x1b);
	KUNIT_EXPECT_EQ(test, command[5], (u8)0xa0);
	KUNIT_EXPECT_EQ(test, command[6], (u8)1);
	KUNIT_EXPECT_EQ(test, command[7], (u8)4);
	KUNIT_EXPECT_EQ(test, command[8], (u8)7);
	KUNIT_EXPECT_EQ(test, command[9], (u8)0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 10), (u16)0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_TERMINAL);
}

static void mt6797_wifi_scan_timeout_failure_test(struct kunit *test)
{
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};
	u8 command[MT6797_WIFI_SCAN_CANCEL_LENGTH];

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_timeout(&transaction, 4, command,
			sizeof(command) - 1), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_TERMINAL);
}

static struct kunit_case mt6797_wifi_scan_cases[] = {
	KUNIT_CASE(mt6797_wifi_scan_start_test),
	KUNIT_CASE(mt6797_wifi_scan_start_rejects_bounds_test),
	KUNIT_CASE(mt6797_wifi_scan_event_test),
	KUNIT_CASE(mt6797_wifi_scan_foreign_event_test),
	KUNIT_CASE(mt6797_wifi_scan_malformed_event_test),
	KUNIT_CASE(mt6797_wifi_scan_timeout_test),
	KUNIT_CASE(mt6797_wifi_scan_timeout_failure_test),
	{}
};

static struct kunit_suite mt6797_wifi_scan_suite = {
	.name = "mt6797-wifi-scan",
	.test_cases = mt6797_wifi_scan_cases,
};

kunit_test_suite(mt6797_wifi_scan_suite);

MODULE_LICENSE("GPL");
