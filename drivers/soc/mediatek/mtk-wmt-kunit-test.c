// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mtk-stp.h>
#include <linux/soc/mediatek/mtk-wmt.h>

struct mtk_wmt_test_transport {
	struct kunit *test;
	struct mtk_stp stp;
	struct mtk_wmt *wmt;
	u8 reply[MTK_STP_HEADER_SIZE + MTK_WMT_HEADER_SIZE + 1 +
		 MTK_STP_TRAILER_SIZE];
	size_t reply_len;
	u8 unsolicited_reply[MTK_STP_HEADER_SIZE + MTK_WMT_HEADER_SIZE + 1 +
		     MTK_STP_TRAILER_SIZE];
	size_t unsolicited_reply_len;
	unsigned int unsolicited;
	unsigned int transmits;
	bool reply_enabled;
	bool unsolicited_first;
	bool transmit_error;
};

struct mtk_wmt_test_async {
	struct mtk_wmt *wmt;
	const struct mtk_wmt_match *expected;
	struct completion done;
	int result;
};

struct mtk_wmt_test_payload_transport {
	struct mtk_wmt *wmt;
	u8 packet[MTK_WMT_HEADER_SIZE + 3];
	size_t packet_len;
	u8 reply[MTK_WMT_HEADER_SIZE + 2];
	size_t reply_len;
	unsigned int transmits;
};

static void mtk_wmt_test_unsolicited(void *context,
				     const struct mtk_wmt_packet *event)
{
	struct mtk_wmt_test_transport *transport = context;

	transport->unsolicited++;
}

static int mtk_wmt_test_transmit(void *context, const u8 *frame,
				 size_t frame_len)
{
	struct mtk_wmt_test_transport *transport = context;
	int ret;

	if (!frame) {
		KUNIT_FAIL(transport->test, "transmit called without a frame");
		return -EINVAL;
	}
	transport->transmits++;
	KUNIT_EXPECT_GT(transport->test, frame_len, (size_t)0);
	if (!transport->reply_enabled)
		return 0;
	if (transport->unsolicited_first) {
		ret = mtk_stp_rx(&transport->stp, transport->unsolicited_reply,
				 transport->unsolicited_reply_len);
		if (ret)
			return ret;
	}

	/* Deliberately divide the STP header and WMT packet across callbacks. */
	ret = mtk_stp_rx(&transport->stp, transport->reply, 1);
	if (ret)
		return ret;
	ret = mtk_stp_rx(&transport->stp, transport->reply + 1,
			 transport->reply_len - 1);
	if (ret)
		return ret;
	return transport->transmit_error ? -EIO : 0;
}

static int mtk_wmt_test_payload_transmit(void *context, const u8 *packet,
					 size_t packet_len)
{
	struct mtk_wmt_test_payload_transport *transport = context;

	if (packet_len > sizeof(transport->packet))
		return -ENOSPC;
	memcpy(transport->packet, packet, packet_len);
	transport->packet_len = packet_len;
	transport->transmits++;
	return mtk_wmt_receive(transport->wmt, transport->reply,
			       transport->reply_len);
}

static int mtk_wmt_test_transaction_thread(void *data)
{
	struct mtk_wmt_test_async *async = data;

	async->result = mtk_wmt_transaction(async->wmt, 0x6, NULL, 0,
					    async->expected, msecs_to_jiffies(100));
	complete(&async->done);
	return 0;
}

static int mtk_wmt_test_reset_thread(void *data)
{
	struct mtk_wmt_test_async *async = data;

	mtk_wmt_reset(async->wmt);
	complete(&async->done);
	return 0;
}

static void mtk_wmt_test_make_reply(struct kunit *test, u8 *frame,
				    size_t frame_size, size_t *frame_len,
				    u8 opcode, const u8 *data, size_t data_len)
{
	u8 event[MTK_WMT_HEADER_SIZE + 1] = {
		MTK_WMT_DIR_EVENT, opcode, data_len, data_len >> 8,
	};

	KUNIT_ASSERT_LE(test, data_len, (size_t)1);
	if (data_len)
		event[MTK_WMT_HEADER_SIZE] = data[0];

	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT, 0, 0,
				       event, sizeof(event), frame, frame_size,
				       frame_len), 0);
}

static void mtk_wmt_test_packet_codec(struct kunit *test)
{
	u8 packet[MTK_WMT_HEADER_SIZE + 3];
	u8 data[] = { 3, 0xa5, 0x5a };
	struct mtk_wmt_packet event;
	size_t packet_len;

	KUNIT_ASSERT_EQ(test,
			mtk_wmt_encode_command(0x6, data, sizeof(data), packet,
					       sizeof(packet), &packet_len), 0);
	KUNIT_ASSERT_EQ(test, packet_len, sizeof(packet));
	KUNIT_EXPECT_EQ(test, packet[0], (u8)MTK_WMT_DIR_COMMAND);
	KUNIT_EXPECT_EQ(test, packet[1], (u8)0x6);
	KUNIT_EXPECT_EQ(test, packet[2], (u8)3);
	KUNIT_EXPECT_EQ(test, packet[4], (u8)3);
	KUNIT_EXPECT_EQ(test, mtk_wmt_decode_event(packet, sizeof(packet), &event),
			-EPROTO);

	packet[0] = MTK_WMT_DIR_EVENT;
	packet[2] = 4;
	KUNIT_EXPECT_EQ(test,
			mtk_wmt_decode_event(packet, sizeof(packet), &event), -EMSGSIZE);
	packet[2] = 3;
	KUNIT_ASSERT_EQ(test,
			mtk_wmt_decode_event(packet, sizeof(packet), &event), 0);
	KUNIT_EXPECT_EQ(test, event.opcode, (u8)0x6);
	KUNIT_EXPECT_EQ(test, event.data_len, (size_t)3);
}

static void mtk_wmt_test_zero_sdu_event(struct kunit *test)
{
	struct mtk_wmt wmt;
	struct mtk_wmt_packet event;
	u8 command[MTK_WMT_HEADER_SIZE];
	u8 zero[] = { MTK_WMT_DIR_EVENT, 0x6, 0, 0 };
	size_t command_len;

	KUNIT_ASSERT_EQ(test,
			mtk_wmt_encode_command(0x6, NULL, 0, command,
					       sizeof(command), &command_len), 0);
	KUNIT_EXPECT_EQ(test, command_len, sizeof(command));
	KUNIT_EXPECT_EQ(test, command[2], (u8)0);
	KUNIT_EXPECT_EQ(test, command[3], (u8)0);

	KUNIT_ASSERT_EQ(test, mtk_wmt_decode_event(zero, sizeof(zero), &event), 0);
	KUNIT_EXPECT_EQ(test, event.opcode, (u8)0x6);
	KUNIT_EXPECT_EQ(test, event.data_len, (size_t)0);

	mtk_wmt_init(&wmt, MTK_STP_MODE_MANDATORY, NULL, NULL);
	wmt.expected.opcode = 0x6;
	wmt.expected.data = NULL;
	wmt.expected.data_len = 0;
	wmt.pending = true;
	KUNIT_EXPECT_EQ(test, mtk_wmt_receive(&wmt, zero, sizeof(zero)), 0);
	KUNIT_EXPECT_FALSE(test, wmt.pending);
	KUNIT_EXPECT_TRUE(test, completion_done(&wmt.response));
}

static void mtk_wmt_test_transaction_fragmented_stp(struct kunit *test)
{
	struct mtk_wmt_test_transport *transport;
	struct mtk_wmt *wmt;
	u8 response[] = { 0 };
	struct mtk_wmt_match expected = { .opcode = 0x6, .data = response,
		.data_len = sizeof(response) };

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	wmt = kunit_kzalloc(test, sizeof(*wmt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	KUNIT_ASSERT_NOT_NULL(test, wmt);
	mtk_wmt_init(wmt, MTK_STP_MODE_MANDATORY, mtk_wmt_test_transmit,
		     transport);
	transport->test = test;
	transport->wmt = wmt;
	mtk_stp_init(&transport->stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(&transport->stp, MTK_STP_WMT,
						      mtk_wmt_receive, wmt), 0);
	mtk_wmt_set_unsolicited(wmt, mtk_wmt_test_unsolicited, transport);
	mtk_wmt_test_make_reply(test, transport->reply, sizeof(transport->reply),
				&transport->reply_len, expected.opcode, expected.data,
				expected.data_len);
	mtk_wmt_test_make_reply(test, transport->unsolicited_reply,
				sizeof(transport->unsolicited_reply),
				&transport->unsolicited_reply_len, 0x7, response,
				sizeof(response));
	transport->reply_enabled = true;
	transport->unsolicited_first = true;

	KUNIT_EXPECT_EQ(test,
			mtk_wmt_transaction(wmt, 0x6, NULL, 0, &expected,
					    msecs_to_jiffies(100)), 0);
	KUNIT_EXPECT_EQ(test, transport->unsolicited, 1U);
}

static void mtk_wmt_test_timeout_fences_late_reply(struct kunit *test)
{
	struct mtk_wmt_test_transport *transport;
	struct mtk_wmt *wmt;
	u8 response[] = { 0 };
	struct mtk_wmt_match expected = { .opcode = 0x6, .data = response,
		.data_len = sizeof(response) };

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	wmt = kunit_kzalloc(test, sizeof(*wmt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	KUNIT_ASSERT_NOT_NULL(test, wmt);
	mtk_wmt_init(wmt, MTK_STP_MODE_MANDATORY, mtk_wmt_test_transmit,
		     transport);
	transport->test = test;
	transport->wmt = wmt;
	KUNIT_EXPECT_EQ(test,
			mtk_wmt_transaction(wmt, 0x6, NULL, 0, &expected, 1),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test,
			mtk_wmt_transaction(wmt, 0x6, NULL, 0, &expected, 1),
			-EPIPE);
	mtk_wmt_reset(wmt);
}

static void mtk_wmt_test_preframed_payload_transport(struct kunit *test)
{
	struct mtk_wmt_test_payload_transport *transport;
	struct mtk_wmt *wmt;
	u8 command[] = { 0xa5, 0x5a };
	u8 response[] = { 0 };
	u8 expected_packet[] = { MTK_WMT_DIR_COMMAND, 0x6, 2, 0, 0xa5, 0x5a };
	struct mtk_wmt_match expected = { .opcode = 0x6, .data = response,
		.data_len = sizeof(response) };

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	wmt = kunit_kzalloc(test, sizeof(*wmt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	KUNIT_ASSERT_NOT_NULL(test, wmt);
	transport->wmt = wmt;
	transport->reply[0] = MTK_WMT_DIR_EVENT;
	transport->reply[1] = 0x6;
	transport->reply[2] = sizeof(response);
	transport->reply[3] = 0;
	transport->reply[4] = response[0];
	transport->reply_len = sizeof(transport->reply);
	mtk_wmt_init_payload_transport(wmt, MTK_STP_MODE_FULL,
				       mtk_wmt_test_payload_transmit, transport);

	KUNIT_EXPECT_EQ(test, mtk_wmt_transaction(wmt, 0x6, command,
						    sizeof(command), &expected,
						    msecs_to_jiffies(100)), 0);
	KUNIT_EXPECT_EQ(test, transport->transmits, 1U);
	KUNIT_EXPECT_EQ(test, transport->packet_len, sizeof(expected_packet));
	KUNIT_EXPECT_MEMEQ(test, transport->packet, expected_packet,
			 sizeof(expected_packet));
}

static void mtk_wmt_test_reset_before_issue(struct kunit *test)
{
	struct mtk_wmt_test_transport *transport;
	struct mtk_wmt *wmt;
	u8 response[] = { 0 };
	struct mtk_wmt_match expected = { .opcode = 0x6, .data = response,
		.data_len = sizeof(response) };
	struct mtk_wmt_test_async transaction = { .expected = &expected };
	struct mtk_wmt_test_async reset = {};
	struct task_struct *task;
	unsigned int retries;
	unsigned long waited;

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	wmt = kunit_kzalloc(test, sizeof(*wmt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	KUNIT_ASSERT_NOT_NULL(test, wmt);
	mtk_wmt_init(wmt, MTK_STP_MODE_MANDATORY, mtk_wmt_test_transmit,
		     transport);
	transport->test = test;
	transport->wmt = wmt;
	transaction.wmt = wmt;
	reset.wmt = wmt;
	init_completion(&transaction.done);
	init_completion(&reset.done);

	/* Queue reset before the transaction's pre-issue barrier. */
	mutex_lock(&wmt->issue_lock);
	task = kthread_run(mtk_wmt_test_reset_thread, &reset, "mtk-wmt-reset");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	for (retries = 0; retries < 100 && !atomic_read(&wmt->reset_requests);
	     retries++)
		usleep_range(1000, 2000);
	KUNIT_ASSERT_EQ(test, atomic_read(&wmt->reset_requests), 1);
	task = kthread_run(mtk_wmt_test_transaction_thread, &transaction,
			   "mtk-wmt-transaction");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	for (retries = 0; retries < 100 && !READ_ONCE(wmt->pending); retries++)
		usleep_range(1000, 2000);
	KUNIT_ASSERT_TRUE(test, READ_ONCE(wmt->pending));
	mutex_unlock(&wmt->issue_lock);

	waited = wait_for_completion_timeout(&transaction.done,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	waited = wait_for_completion_timeout(&reset.done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, transaction.result, -ECANCELED);
	KUNIT_EXPECT_EQ(test, transport->transmits, 0U);
}

static void mtk_wmt_test_sync_reply_transport_error_fences(struct kunit *test)
{
	struct mtk_wmt_test_transport *transport;
	struct mtk_wmt *wmt;
	u8 response[] = { 0 };
	struct mtk_wmt_match expected = { .opcode = 0x6, .data = response,
		.data_len = sizeof(response) };

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	wmt = kunit_kzalloc(test, sizeof(*wmt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	KUNIT_ASSERT_NOT_NULL(test, wmt);
	mtk_wmt_init(wmt, MTK_STP_MODE_MANDATORY, mtk_wmt_test_transmit,
		     transport);
	transport->test = test;
	transport->wmt = wmt;
	mtk_stp_init(&transport->stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(&transport->stp, MTK_STP_WMT,
						      mtk_wmt_receive, wmt), 0);
	mtk_wmt_test_make_reply(test, transport->reply, sizeof(transport->reply),
				&transport->reply_len, expected.opcode, expected.data,
				expected.data_len);
	transport->reply_enabled = true;
	transport->transmit_error = true;

	KUNIT_EXPECT_EQ(test,
			mtk_wmt_transaction(wmt, 0x6, NULL, 0, &expected, 1),
			-EIO);
	KUNIT_EXPECT_EQ(test,
			mtk_wmt_transaction(wmt, 0x6, NULL, 0, &expected, 1),
			-EPIPE);
}

static void mtk_wmt_test_unmatched_is_unsolicited(struct kunit *test)
{
	struct mtk_wmt_test_transport *transport;
	struct mtk_wmt wmt;
	u8 event[] = { MTK_WMT_DIR_EVENT, 0x7, 1, 0, 0 };

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	mtk_wmt_init(&wmt, MTK_STP_MODE_MANDATORY, NULL, NULL);
	mtk_wmt_set_unsolicited(&wmt, mtk_wmt_test_unsolicited, transport);
	KUNIT_EXPECT_EQ(test, mtk_wmt_receive(&wmt, event, sizeof(event)), 0);
	KUNIT_EXPECT_EQ(test, transport->unsolicited, 1U);
}

static void mtk_wmt_test_capture_variable_reply(struct kunit *test)
{
	struct mtk_wmt_test_payload_transport transport = { };
	struct mtk_wmt wmt;
	u8 command[] = { 4, 0 };
	u8 reply[] = { 0, 0 };

	mtk_wmt_init_payload_transport(&wmt, MTK_STP_MODE_FULL,
				       mtk_wmt_test_payload_transmit, &transport);
	transport.wmt = &wmt;
	transport.reply[0] = MTK_WMT_DIR_EVENT;
	transport.reply[1] = 0x12;
	transport.reply[2] = 2;
	transport.reply[3] = 0;
	transport.reply[4] = 4;
	transport.reply[5] = 2;
	transport.reply_len = 6;

	KUNIT_ASSERT_EQ(test, mtk_wmt_transaction_capture(&wmt, 0x12, command,
						     sizeof(command), 0x12, 2,
						     reply, sizeof(reply),
						     msecs_to_jiffies(100)), 0);
	KUNIT_EXPECT_EQ(test, reply[0], (u8)4);
	KUNIT_EXPECT_EQ(test, reply[1], (u8)2);
}

static struct kunit_case mtk_wmt_test_cases[] = {
	KUNIT_CASE(mtk_wmt_test_packet_codec),
	KUNIT_CASE(mtk_wmt_test_zero_sdu_event),
	KUNIT_CASE(mtk_wmt_test_transaction_fragmented_stp),
	KUNIT_CASE(mtk_wmt_test_timeout_fences_late_reply),
	KUNIT_CASE(mtk_wmt_test_preframed_payload_transport),
	KUNIT_CASE(mtk_wmt_test_reset_before_issue),
	KUNIT_CASE(mtk_wmt_test_sync_reply_transport_error_fences),
	KUNIT_CASE(mtk_wmt_test_unmatched_is_unsolicited),
	KUNIT_CASE(mtk_wmt_test_capture_variable_reply),
	{}
};

static struct kunit_suite mtk_wmt_test_suite = {
	.name = "mtk-wmt",
	.test_cases = mtk_wmt_test_cases,
};

kunit_test_suite(mtk_wmt_test_suite);

MODULE_LICENSE("GPL");
