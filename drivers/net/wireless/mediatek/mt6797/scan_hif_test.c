// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hif_regs.h"
#include "hif_runtime.h"
#include "protocol.h"
#include "scan_hif.h"

#define MT6797_WIFI_SCAN_HIF_TEST_GROUP3	BIT(2)
#define MT6797_WIFI_SCAN_HIF_TEST_FRAME		40U
#define MT6797_WIFI_SCAN_HIF_TEST_IES		36U
#define MT6797_WIFI_SCAN_HIF_TEST_TIMEOUT_MS	1000U
#define MT6797_WIFI_SCAN_HIF_TEST_MAX_AGE_MS	2000U
#define MT6797_WIFI_SCAN_HIF_TEST_MAX_SENDS	2U

enum mt6797_wifi_scan_hif_test_wait_mode {
	MT6797_WIFI_SCAN_HIF_TEST_TARGET_THEN_DONE,
	MT6797_WIFI_SCAN_HIF_TEST_DONE_THEN_TARGET,
	MT6797_WIFI_SCAN_HIF_TEST_FOREIGN_HIDDEN,
	MT6797_WIFI_SCAN_HIF_TEST_MALFORMED,
	MT6797_WIFI_SCAN_HIF_TEST_TIMEOUT,
	MT6797_WIFI_SCAN_HIF_TEST_DONE_AT_TIMEOUT,
	MT6797_WIFI_SCAN_HIF_TEST_ABORT,
	MT6797_WIFI_SCAN_HIF_TEST_SPURIOUS,
	MT6797_WIFI_SCAN_HIF_TEST_WRONG_CHANNEL,
};

struct mt6797_wifi_scan_hif_test_context {
	struct kunit *test;
	struct mt6797_wifi_scan_hif *scan;
	ktime_t now;
	u8 packet[MT6797_WIFI_SCAN_HIF_TEST_MAX_SENDS]
		 [MT6797_WIFI_SCAN_COMMAND_LENGTH];
	size_t length[MT6797_WIFI_SCAN_HIF_TEST_MAX_SENDS];
	u32 address[MT6797_WIFI_SCAN_HIF_TEST_MAX_SENDS];
	unsigned int sends;
	unsigned int waits;
	unsigned int terminals;
	unsigned int fail_send;
	int delivery_error;
	int terminal_reason;
	u8 wait_mode;
};

struct mt6797_wifi_scan_hif_test_fixture {
	struct mt6797_wifi_scan_hif scan;
	struct mt6797_wifi_hif_irq irq;
	struct mt6797_wifi_scan_hif_backend backend;
	struct mt6797_wifi_scan_hif_request request;
	struct mt6797_wifi_scan_hif_test_context context;
};

static const u8 mt6797_wifi_scan_hif_test_bssid[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_scan_hif_test_foreign[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static size_t mt6797_wifi_scan_hif_test_packet(
	u8 *packet, size_t capacity, const u8 bssid[ETH_ALEN],
	const char *ssid, size_t ssid_length, u8 channel, u8 rcpi)
{
	u8 *frame = packet + MT6797_WIFI_SCAN_HIF_TEST_FRAME;
	u8 *ies = frame + MT6797_WIFI_SCAN_HIF_TEST_IES;
	size_t length = MT6797_WIFI_SCAN_HIF_TEST_FRAME +
		MT6797_WIFI_SCAN_HIF_TEST_IES + 2 + ssid_length + 3;

	if (capacity < length || ssid_length > IEEE80211_MAX_SSID_LEN)
		return 0;
	memset(packet, 0, capacity);
	put_unaligned_le16(length, packet);
	put_unaligned_le16(0xe001 |
		(MT6797_WIFI_SCAN_HIF_TEST_GROUP3 << 9), packet + 2);
	packet[5] = channel;
	packet[6] = 24;
	put_unaligned_le32((u32)rcpi << 8, packet + 24);
	put_unaligned_le16(IEEE80211_STYPE_BEACON, frame);
	memcpy(frame + 10, bssid, ETH_ALEN);
	memcpy(frame + 16, bssid, ETH_ALEN);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + 34);
	ies[0] = WLAN_EID_SSID;
	ies[1] = ssid_length;
	if (ssid_length)
		memcpy(ies + 2, ssid, ssid_length);
	ies += 2 + ssid_length;
	ies[0] = WLAN_EID_DS_PARAMS;
	ies[1] = 1;
	ies[2] = channel;
	return length;
}

static int mt6797_wifi_scan_hif_test_send(
	void *data, u32 address, const u8 *packet, size_t length)
{
	struct mt6797_wifi_scan_hif_test_context *context = data;
	unsigned int send = context->sends;

	if (!packet || !length || length > MT6797_WIFI_SCAN_COMMAND_LENGTH ||
	    send >= MT6797_WIFI_SCAN_HIF_TEST_MAX_SENDS)
		return -EINVAL;
	context->address[send] = address;
	context->length[send] = length;
	memcpy(context->packet[send], packet, length);
	context->sends++;
	return send == context->fail_send ? -EIO : 0;
}

static ktime_t mt6797_wifi_scan_hif_test_now(void *data)
{
	struct mt6797_wifi_scan_hif_test_context *context = data;

	return context->now;
}

static int mt6797_wifi_scan_hif_test_frame(
	struct mt6797_wifi_scan_hif_test_context *context,
	const u8 bssid[ETH_ALEN], const char *ssid, size_t ssid_length,
	u8 channel, u8 rcpi)
{
	u8 packet[160];
	struct mt6797_wifi_rx_view frame;
	size_t length;
	int ret;

	length = mt6797_wifi_scan_hif_test_packet(packet, sizeof(packet),
		bssid, ssid, ssid_length, channel, rcpi);
	if (!length)
		return -EINVAL;
	ret = mt6797_wifi_rx_parse(packet, length, &frame);
	if (ret)
		return ret;
	ret = mt6797_wifi_hif_runtime_sink_frame(context->scan->irq, &frame);
	memset(packet, 0xa5, sizeof(packet));
	return ret;
}

static int mt6797_wifi_scan_hif_test_done(
	struct mt6797_wifi_scan_hif_test_context *context)
{
	u8 payload[4] = { context->scan->transaction.scan_sequence };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_EVENT_HEADER_LENGTH +
			sizeof(payload),
		.id = MT6797_WIFI_SCAN_DONE_EVENT_ID,
		.sequence = 0xee,
	};

	return mt6797_wifi_hif_runtime_sink_event(context->scan->irq, &event);
}

static int mt6797_wifi_scan_hif_test_malformed(
	struct mt6797_wifi_scan_hif_test_context *context)
{
	u8 packet[160];
	struct mt6797_wifi_rx_view frame;
	size_t length;
	int ret;

	length = mt6797_wifi_scan_hif_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_hif_test_bssid, "test", 4,
		context->scan->expected_channel, 221);
	if (!length)
		return -EINVAL;
	ret = mt6797_wifi_rx_parse(packet, length, &frame);
	if (ret)
		return ret;
	return mt6797_wifi_hif_runtime_sink_frame(context->scan->irq, &frame);
}

static unsigned long mt6797_wifi_scan_hif_test_wait(
	void *data, struct completion *completion, unsigned long timeout)
{
	struct mt6797_wifi_scan_hif_test_context *context = data;

	context->waits++;
	KUNIT_EXPECT_GT(context->test, timeout, 0UL);
	switch (context->wait_mode) {
	case MT6797_WIFI_SCAN_HIF_TEST_TARGET_THEN_DONE:
		context->delivery_error = mt6797_wifi_scan_hif_test_frame(
			context, mt6797_wifi_scan_hif_test_bssid, "test", 4,
			context->scan->expected_channel, 100);
		if (!context->delivery_error)
			context->delivery_error =
				mt6797_wifi_scan_hif_test_done(context);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_DONE_THEN_TARGET:
		context->delivery_error = mt6797_wifi_scan_hif_test_done(context);
		if (!context->delivery_error)
			context->delivery_error = mt6797_wifi_scan_hif_test_frame(
				context, mt6797_wifi_scan_hif_test_bssid,
				"test", 4, context->scan->expected_channel, 100);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_FOREIGN_HIDDEN:
		context->delivery_error = mt6797_wifi_scan_hif_test_frame(
			context, mt6797_wifi_scan_hif_test_foreign, "test", 4,
			context->scan->expected_channel, 100);
		if (!context->delivery_error)
			context->delivery_error = mt6797_wifi_scan_hif_test_frame(
				context, mt6797_wifi_scan_hif_test_bssid,
				NULL, 0, context->scan->expected_channel, 100);
		if (!context->delivery_error)
			context->delivery_error =
				mt6797_wifi_scan_hif_test_done(context);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_MALFORMED:
		context->delivery_error =
			mt6797_wifi_scan_hif_test_malformed(context);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_TIMEOUT:
		return 0;
	case MT6797_WIFI_SCAN_HIF_TEST_DONE_AT_TIMEOUT:
		context->delivery_error = mt6797_wifi_scan_hif_test_done(context);
		return 0;
	case MT6797_WIFI_SCAN_HIF_TEST_ABORT:
		mt6797_wifi_hif_runtime_sink_abort(context->scan->irq, -EUCLEAN);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_SPURIOUS:
		complete(completion);
		break;
	case MT6797_WIFI_SCAN_HIF_TEST_WRONG_CHANNEL:
		context->delivery_error = mt6797_wifi_scan_hif_test_frame(
			context, mt6797_wifi_scan_hif_test_bssid, "test", 4,
			context->scan->expected_channel + 1, 100);
		break;
	}
	return completion_done(completion);
}

static void mt6797_wifi_scan_hif_test_terminal(void *data, int reason)
{
	struct mt6797_wifi_scan_hif_test_context *context = data;

	context->terminals++;
	context->terminal_reason = reason;
}

static void mt6797_wifi_scan_hif_test_init(
	struct kunit *test, struct mt6797_wifi_scan_hif_test_fixture *fixture,
	u8 wait_mode)
{
	memset(fixture, 0, sizeof(*fixture));
	mutex_init(&fixture->irq.lifecycle_lock);
	spin_lock_init(&fixture->irq.terminal_lock);
	spin_lock_init(&fixture->irq.sink_lock);
	fixture->irq.mode = MT6797_WIFI_HIF_IRQ_RUNTIME;
	fixture->context = (struct mt6797_wifi_scan_hif_test_context) {
		.test = test,
		.scan = &fixture->scan,
		.now = ms_to_ktime(100),
		.fail_send = UINT_MAX,
		.wait_mode = wait_mode,
	};
	fixture->backend = (struct mt6797_wifi_scan_hif_backend) {
		.send = mt6797_wifi_scan_hif_test_send,
		.now = mt6797_wifi_scan_hif_test_now,
		.wait = mt6797_wifi_scan_hif_test_wait,
		.terminal = mt6797_wifi_scan_hif_test_terminal,
		.context = &fixture->context,
	};
	memcpy(fixture->request.key.bssid,
	       mt6797_wifi_scan_hif_test_bssid, ETH_ALEN);
	memcpy(fixture->request.key.ssid, "test", 4);
	fixture->request.key.ssid_length = 4;
	fixture->request.max_age_ms = MT6797_WIFI_SCAN_HIF_TEST_MAX_AGE_MS;
	fixture->request.timeout_ms = MT6797_WIFI_SCAN_HIF_TEST_TIMEOUT_MS;
	fixture->request.command_sequence = 3;
	fixture->request.cancel_sequence = 4;
	fixture->request.scan_sequence = 7;
	fixture->request.band = MT6797_WIFI_SCAN_BAND_2GHZ;
	fixture->request.channel = 6;
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_hif_init(&fixture->scan,
		&fixture->irq, &fixture->backend), 0);
}

static void mt6797_wifi_scan_hif_success_lifetime_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;
	struct mt6797_wifi_rx_view late = {};

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_TARGET_THEN_DONE);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), 0);
	KUNIT_EXPECT_EQ(test, fixture.context.delivery_error, 0);
	KUNIT_EXPECT_EQ(test, fixture.context.sends, 1U);
	KUNIT_EXPECT_EQ(test, fixture.context.waits, 1U);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 0U);
	KUNIT_EXPECT_TRUE(test, result.scan_done);
	KUNIT_EXPECT_TRUE(test, result.target_seen);
	KUNIT_EXPECT_FALSE(test, result.cancel_sent);
	KUNIT_EXPECT_MEMEQ(test, result.target.bssid,
		mt6797_wifi_scan_hif_test_bssid, ETH_ALEN);
	KUNIT_EXPECT_MEMEQ(test, result.target.ssid, "test", 4);
	KUNIT_EXPECT_EQ(test, result.target.channel, (u8)6);
	KUNIT_EXPECT_EQ(test, fixture.context.address[0],
		(u32)MT6797_WIFI_MCR_WTDR1);
	KUNIT_EXPECT_EQ(test, fixture.context.length[0],
		(size_t)MT6797_WIFI_SCAN_COMMAND_LENGTH);
	KUNIT_EXPECT_PTR_EQ(test, fixture.irq.sink.context, NULL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_runtime_sink_frame(&fixture.irq, &late), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_validate(
		&fixture.scan.target_owner, 100, &result.target), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_validate(
		&fixture.scan.target_owner, 2100, &result.target), -ESTALE);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_close(&fixture.scan), 0);
}

static void mt6797_wifi_scan_hif_done_before_target_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_DONE_THEN_TARGET);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -ENODATA);
	KUNIT_EXPECT_EQ(test, fixture.context.delivery_error, 0);
	KUNIT_EXPECT_TRUE(test, result.scan_done);
	KUNIT_EXPECT_FALSE(test, result.target_seen);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 0U);
}

static void mt6797_wifi_scan_hif_foreign_hidden_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_FOREIGN_HIDDEN);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -ENODATA);
	KUNIT_EXPECT_EQ(test, fixture.context.delivery_error, 0);
	KUNIT_EXPECT_FALSE(test, result.target_seen);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 0U);
}

static void mt6797_wifi_scan_hif_malformed_terminal_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_MALFORMED);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -ERANGE);
	KUNIT_EXPECT_EQ(test, fixture.context.delivery_error, -ERANGE);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, fixture.context.terminal_reason, -ERANGE);
	KUNIT_EXPECT_FALSE(test, result.cancel_sent);
}

static void mt6797_wifi_scan_hif_timeout_cancel_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_TIMEOUT);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -ETIMEDOUT);
	KUNIT_ASSERT_EQ(test, fixture.context.sends, 2U);
	KUNIT_EXPECT_EQ(test, fixture.context.length[1],
		(size_t)MT6797_WIFI_SCAN_CANCEL_LENGTH);
	KUNIT_EXPECT_TRUE(test, result.cancel_sent);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, fixture.context.terminal_reason, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EALREADY);
	KUNIT_EXPECT_EQ(test, fixture.context.sends, 2U);
}

static void mt6797_wifi_scan_hif_late_done_wins_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_DONE_AT_TIMEOUT);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -ENODATA);
	KUNIT_EXPECT_EQ(test, fixture.context.sends, 1U);
	KUNIT_EXPECT_TRUE(test, result.scan_done);
	KUNIT_EXPECT_FALSE(test, result.cancel_sent);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 0U);
}

static void mt6797_wifi_scan_hif_abort_spurious_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_ABORT);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_SPURIOUS);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);
}

static void mt6797_wifi_scan_hif_send_and_reuse_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_TARGET_THEN_DONE);
	fixture.context.fail_send = 0;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EIO);
	KUNIT_EXPECT_EQ(test, fixture.context.waits, 0U);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EALREADY);
}

static void mt6797_wifi_scan_hif_wrong_channel_test(struct kunit *test)
{
	struct mt6797_wifi_scan_hif_test_fixture fixture;
	struct mt6797_wifi_scan_hif_result result;

	mt6797_wifi_scan_hif_test_init(test, &fixture,
		MT6797_WIFI_SCAN_HIF_TEST_WRONG_CHANNEL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_hif_run(&fixture.scan,
		&fixture.request, &result), -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture.context.delivery_error, -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture.context.terminals, 1U);
	KUNIT_EXPECT_FALSE(test, result.target_seen);
}

static struct kunit_case mt6797_wifi_scan_hif_cases[] = {
	KUNIT_CASE(mt6797_wifi_scan_hif_success_lifetime_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_done_before_target_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_foreign_hidden_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_malformed_terminal_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_timeout_cancel_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_late_done_wins_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_abort_spurious_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_send_and_reuse_test),
	KUNIT_CASE(mt6797_wifi_scan_hif_wrong_channel_test),
	{}
};

static struct kunit_suite mt6797_wifi_scan_hif_suite = {
	.name = "mt6797-wifi-scan-hif",
	.test_cases = mt6797_wifi_scan_hif_cases,
};

kunit_test_suite(mt6797_wifi_scan_hif_suite);

MODULE_LICENSE("GPL");
