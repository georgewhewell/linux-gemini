// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hif_pump.h"
#include "hif_regs.h"

struct mt6797_wifi_hif_pump_test_context {
	u8 event[MT6797_WIFI_EVENT_HEADER_LENGTH + 4];
	u16 lengths[2][2];
	u32 status;
	unsigned int lengths_index;
	unsigned int expired_calls;
	unsigned int expire_at;
	unsigned int delivered_events;
	unsigned int delivered_frames;
	int event_result;
	int frame_result;
	u8 delivered_id;
	u8 delivered_sequence;
	u8 delivered_payload;
	bool endless_rx;
	bool packet_fault;
};

static void mt6797_wifi_hif_pump_test_event(
	struct mt6797_wifi_hif_pump_test_context *context)
{
	memset(context->event, 0, sizeof(context->event));
	put_unaligned_le16(sizeof(context->event), context->event);
	put_unaligned_le16(0xe000, context->event + 2);
	context->event[4] = 0x0d;
	context->event[5] = 3;
	context->event[MT6797_WIFI_EVENT_HEADER_LENGTH] = 0x5a;
}

static int mt6797_wifi_hif_pump_test_status(void *data, u32 *status)
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	*status = context->status;
	return 0;
}

static int mt6797_wifi_hif_pump_test_tx(
	void *data, u16 released[MT6797_WIFI_HIF_TX_QUEUES])
{
	(void)data;
	memset(released, 0, sizeof(u16) * MT6797_WIFI_HIF_TX_QUEUES);
	released[0] = 2;
	released[15] = 3;
	return 0;
}

static int mt6797_wifi_hif_pump_test_lengths(void *data, u16 lengths[2])
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	if (context->endless_rx) {
		lengths[0] = sizeof(context->event);
		lengths[1] = 0;
		return 0;
	}
	if (context->lengths_index >= ARRAY_SIZE(context->lengths))
		return -EIO;
	memcpy(lengths, context->lengths[context->lengths_index++],
	       sizeof(context->lengths[0]));
	return 0;
}

static int mt6797_wifi_hif_pump_test_packet(void *data, unsigned int port,
					    u16 length, const u8 **packet)
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	if (context->packet_fault)
		return -EIO;
	if (port > 1 || length != sizeof(context->event))
		return -EINVAL;
	*packet = context->event;
	return 0;
}

static bool mt6797_wifi_hif_pump_test_expired(void *data)
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	if (!context->expire_at)
		return false;
	return ++context->expired_calls >= context->expire_at;
}

static int mt6797_wifi_hif_pump_test_deliver(
	void *data, const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	context->delivered_events++;
	context->delivered_id = event->id;
	context->delivered_sequence = event->sequence;
	if (event->payload_length)
		context->delivered_payload = event->payload[0];
	return context->event_result;
}

static int mt6797_wifi_hif_pump_test_deliver_frame(
	void *data, const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_hif_pump_test_context *context = data;

	if (frame->kind != MT6797_WIFI_RX_IEEE80211)
		return -EINVAL;
	context->delivered_frames++;
	return context->frame_result;
}

static void mt6797_wifi_hif_pump_test_ops(
	struct mt6797_wifi_hif_pump_test_context *context,
	struct mt6797_wifi_hif_pump_ops *ops)
{
	*ops = (struct mt6797_wifi_hif_pump_ops) {
		.read_status = mt6797_wifi_hif_pump_test_status,
		.read_tx_released = mt6797_wifi_hif_pump_test_tx,
		.read_lengths = mt6797_wifi_hif_pump_test_lengths,
		.read_packet = mt6797_wifi_hif_pump_test_packet,
		.deliver_event = mt6797_wifi_hif_pump_test_deliver,
		.deliver_frame = mt6797_wifi_hif_pump_test_deliver_frame,
		.expired = mt6797_wifi_hif_pump_test_expired,
		.context = context,
	};
}

static void mt6797_wifi_hif_pump_success_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_TX_DONE |
			  MT6797_WIFI_WHISR_RX1_DONE,
		.lengths = { { 0, MT6797_WIFI_EVENT_HEADER_LENGTH }, { 0, 0 } },
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status = 0;
	int ret;

	mt6797_wifi_hif_pump_test_event(&context);
	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	ret = mt6797_wifi_hif_pump_run(&ops, &stats, &status);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, status, context.status);
	KUNIT_EXPECT_EQ(test, stats.tx_released[0], 2ULL);
	KUNIT_EXPECT_EQ(test, stats.tx_released[15], 3ULL);
	KUNIT_EXPECT_EQ(test, stats.rx_packets, 1ULL);
	KUNIT_EXPECT_EQ(test, stats.events, 1ULL);
	KUNIT_EXPECT_EQ(test, stats.last_event_id, 0x0d);
	KUNIT_EXPECT_EQ(test, stats.last_event_sequence, 3);
	KUNIT_EXPECT_EQ(test, context.delivered_events, 1U);
	KUNIT_EXPECT_EQ(test, context.delivered_id, (u8)0x0d);
	KUNIT_EXPECT_EQ(test, context.delivered_sequence, (u8)3);
	KUNIT_EXPECT_EQ(test, context.delivered_payload, (u8)0x5a);
	context.event[MT6797_WIFI_EVENT_HEADER_LENGTH] = 0xa5;
	KUNIT_EXPECT_EQ(test, context.delivered_payload, (u8)0x5a);
}

static void mt6797_wifi_hif_pump_malformed_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_RX0_DONE,
		.lengths = { { MT6797_WIFI_EVENT_HEADER_LENGTH, 0 }, { 0, 0 } },
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_event(&context);
	put_unaligned_le16(sizeof(context.event) + 1, context.event);
	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, stats.rx_packets, 0ULL);
}

static void mt6797_wifi_hif_pump_overflow_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_RX0_DONE,
		.endless_rx = true,
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_event(&context);
	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, stats.rx_packets,
			(u64)MT6797_WIFI_HIF_RX_BUDGET);
}

static void mt6797_wifi_hif_pump_timeout_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_RX0_DONE,
		.expire_at = 2,
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -ETIMEDOUT);
}

static void mt6797_wifi_hif_pump_packet_fault_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_RX0_DONE,
		.lengths = { { MT6797_WIFI_EVENT_HEADER_LENGTH, 0 }, { 0, 0 } },
		.packet_fault = true,
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_event(&context);
	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EIO);
}

static void mt6797_wifi_hif_pump_status_fault_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_ABNORMAL,
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EIO);
	context.status = MT6797_WIFI_WHISR_FW_OWN_BACK;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EPROTO);
	context.status = 0;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -ENODATA);
}

static void mt6797_wifi_hif_pump_tx_overflow_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_stats stats = {};
	u16 released[MT6797_WIFI_HIF_TX_QUEUES] = { 1 };

	stats.tx_released[0] = U64_MAX;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_account_tx(&stats, released), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, stats.tx_released[0], U64_MAX);
}

static void mt6797_wifi_hif_pump_typed_dispatch_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_rx_view frame = {
		.kind = MT6797_WIFI_RX_IEEE80211,
	};

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_pump_dispatch(NULL, &stats, &frame), 0);
	frame.kind = MT6797_WIFI_RX_ETHERNET;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_pump_dispatch(NULL, &stats, &frame), 0);
	KUNIT_EXPECT_EQ(test, stats.rx_packets, 2ULL);
	KUNIT_EXPECT_EQ(test, stats.ieee80211_frames, 1ULL);
	KUNIT_EXPECT_EQ(test, stats.ethernet_frames, 1ULL);
	frame.kind = 99;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_dispatch(NULL, &stats, &frame), -EPROTO);
}

static void mt6797_wifi_hif_pump_event_error_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {
		.status = MT6797_WIFI_WHISR_RX0_DONE,
		.lengths = { { sizeof(context.event), 0 }, { 0, 0 } },
		.event_result = -EPROTO,
	};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops;
	u32 status;

	mt6797_wifi_hif_pump_test_event(&context);
	mt6797_wifi_hif_pump_test_ops(&context, &ops);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_pump_run(&ops, &stats, &status), -EPROTO);
	KUNIT_EXPECT_EQ(test, context.delivered_events, 1U);
	KUNIT_EXPECT_EQ(test, stats.rx_packets, 1ULL);
	KUNIT_EXPECT_EQ(test, stats.events, 1ULL);
}

static void mt6797_wifi_hif_pump_event_selection_test(struct kunit *test)
{
	struct mt6797_wifi_hif_pump_test_context context = {};
	struct mt6797_wifi_hif_pump_stats stats = {};
	struct mt6797_wifi_hif_pump_ops ops = {
		.deliver_event = mt6797_wifi_hif_pump_test_deliver,
		.deliver_frame = mt6797_wifi_hif_pump_test_deliver_frame,
		.context = &context,
	};
	struct mt6797_wifi_rx_view frame = {
		.kind = MT6797_WIFI_RX_IEEE80211,
	};
	u8 payload = 0x5a;

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_pump_dispatch(&ops, &stats, &frame), 0);
	KUNIT_EXPECT_EQ(test, context.delivered_events, 0U);
	KUNIT_EXPECT_EQ(test, context.delivered_frames, 1U);
	frame = (struct mt6797_wifi_rx_view) {
		.kind = MT6797_WIFI_RX_EVENT,
		.payload = &payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_EVENT_HEADER_LENGTH + sizeof(payload),
		.event_id = 0x27,
		.sequence = 4,
	};
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_pump_dispatch(&ops, &stats, &frame), 0);
	KUNIT_EXPECT_EQ(test, context.delivered_events, 1U);
	KUNIT_EXPECT_EQ(test, context.delivered_frames, 1U);
	KUNIT_EXPECT_EQ(test, context.delivered_id, (u8)0x27);
	KUNIT_EXPECT_EQ(test, context.delivered_payload, payload);
}

static struct kunit_case mt6797_wifi_hif_pump_cases[] = {
	KUNIT_CASE(mt6797_wifi_hif_pump_success_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_malformed_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_overflow_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_timeout_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_packet_fault_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_status_fault_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_tx_overflow_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_typed_dispatch_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_event_error_test),
	KUNIT_CASE(mt6797_wifi_hif_pump_event_selection_test),
	{}
};

static struct kunit_suite mt6797_wifi_hif_pump_suite = {
	.name = "mt6797-wifi-hif-pump",
	.test_cases = mt6797_wifi_hif_pump_cases,
};

kunit_test_suite(mt6797_wifi_hif_pump_suite);

MODULE_LICENSE("GPL");
