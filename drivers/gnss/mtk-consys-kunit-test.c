// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coverage for the opaque-full-STP MediaTek CONSYS GNSS endpoint. */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mtk-gnss.h>

struct mtk_gnss_test_backend {
	u8 frame[MTK_STP_HEADER_SIZE + MTK_STP_MAX_PAYLOAD +
		 MTK_STP_TRAILER_SIZE];
	size_t frame_len;
	unsigned int power_on_calls;
	unsigned int power_off_calls;
	int tx_result;
	int power_on_result;
	int power_off_result;
	struct mt6797_consys_stp_session *session;
	const u8 *loopback_frame;
	size_t loopback_frame_len;
	bool loopback_from_power;
	int loopback_result;
	bool block_power_off;
	struct completion power_off_entered;
	struct completion allow_power_off;
};

struct mtk_gnss_test {
	struct device *dev;
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mtk_gnss gnss;
	struct mtk_gnss_test_backend backend;
};

static int mtk_gnss_test_transmit(void *context, const u8 *frame,
				  size_t frame_len)
{
	struct mtk_gnss_test_backend *backend = context;

	if (frame_len > sizeof(backend->frame))
		return -EMSGSIZE;
	memcpy(backend->frame, frame, frame_len);
	backend->frame_len = frame_len;
	return backend->tx_result;
}

static int mtk_gnss_test_session_receive(void *context,
					 enum mtk_stp_channel channel,
					 const u8 *payload, size_t payload_length)
{
	return channel == MTK_STP_WMT ? 0 : -EOPNOTSUPP;
}

static int mtk_gnss_test_set_power(void *context, bool on)
{
	struct mtk_gnss_test_backend *backend = context;

	if (on) {
		backend->power_on_calls++;
		if (backend->loopback_from_power)
			backend->loopback_result =
				mt6797_consys_stp_session_receive(backend->session,
					backend->loopback_frame,
					backend->loopback_frame_len);
		return backend->power_on_result;
	}

	backend->power_off_calls++;
	if (backend->block_power_off) {
		complete(&backend->power_off_entered);
		wait_for_completion(&backend->allow_power_off);
	}
	if (backend->loopback_from_power)
		backend->loopback_result =
			mt6797_consys_stp_session_receive(backend->session,
				backend->loopback_frame, backend->loopback_frame_len);
	return backend->power_off_result;
}

static int mtk_gnss_test_init(struct kunit *test)
{
	struct mtk_gnss_test *ctx;
	struct mtk_gnss_transport transport;
	struct mt6797_consys_stp_session_ops session_ops;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->dev = kunit_device_register(test, "mtk-gnss-test");
	if (IS_ERR(ctx->dev))
		return PTR_ERR(ctx->dev);
	session_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mtk_gnss_test_transmit,
		.receive = mtk_gnss_test_session_receive,
		.transmit_context = &ctx->backend,
		.receive_context = &ctx->backend,
	};
	ctx->session = mt6797_consys_stp_session_create(&session_ops);
	if (IS_ERR(ctx->session))
		return PTR_ERR(ctx->session);
	ctx->backend.session = ctx->session;
	init_completion(&ctx->backend.power_off_entered);
	init_completion(&ctx->backend.allow_power_off);
	ctx->session_lease = mt6797_consys_stp_session_begin_full(ctx->session);
	if (IS_ERR(ctx->session_lease))
		return PTR_ERR(ctx->session_lease);
	transport.set_power = mtk_gnss_test_set_power;
	transport.context = &ctx->backend;

	return mtk_gnss_register(&ctx->gnss, ctx->dev, ctx->session_lease,
				 &transport);
}

struct mtk_gnss_test_unregister {
	struct mtk_gnss *gnss;
	struct completion entered;
	struct completion done;
};

static int mtk_gnss_test_unregister_thread(void *context)
{
	struct mtk_gnss_test_unregister *unregister = context;

	complete(&unregister->entered);
	mtk_gnss_unregister(unregister->gnss);
	complete(&unregister->done);
	return 0;
}

static void mtk_gnss_test_exit(struct kunit *test)
{
	struct mtk_gnss_test *ctx = test->priv;

	mtk_gnss_unregister(&ctx->gnss);
	mt6797_consys_stp_session_destroy(ctx->session);
}

static size_t mtk_gnss_test_full_frame(struct kunit *test, u8 *frame,
					       size_t frame_size, u8 sequence, u8 ack,
					       const u8 *payload, size_t payload_length)
{
	size_t frame_length;

	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_GNSS,
					     sequence, ack, payload, payload_length, frame,
					     frame_size, &frame_length), 0);
	return frame_length;
}

static void mtk_gnss_test_lifecycle_and_stream(struct kunit *test)
{
	static const u8 outbound[] = { 0x24, 0x47, 0x50, 0x52, 0x4d, 0x43 };
	static const u8 inbound[] = { 0xa1, 0xb2, 0xc3 };
	struct mtk_gnss_test *ctx = test->priv;
	u8 expected[MTK_STP_HEADER_SIZE + sizeof(outbound) + MTK_STP_TRAILER_SIZE];
	u8 frame[MTK_STP_HEADER_SIZE + sizeof(inbound) + MTK_STP_TRAILER_SIZE];
	u8 received[sizeof(inbound)];
	size_t expected_length;
	size_t frame_length;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	KUNIT_EXPECT_EQ(test, ctx->backend.power_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&ctx->gnss, outbound,
					     sizeof(outbound)), (int)sizeof(outbound));
	expected_length = mtk_gnss_test_full_frame(test, expected, sizeof(expected),
							0, 7, outbound, sizeof(outbound));
	KUNIT_EXPECT_EQ(test, ctx->backend.frame_len, expected_length);
	KUNIT_EXPECT_MEMEQ(test, ctx->backend.frame, expected, expected_length);

	frame_length = mtk_gnss_test_full_frame(test, frame, sizeof(frame), 0, 0,
						     inbound, sizeof(inbound));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(ctx->session, frame,
								  frame_length), 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo),
				(unsigned int)sizeof(inbound));
	KUNIT_EXPECT_EQ(test, kfifo_out(&ctx->gnss.gdev->read_fifo, received,
				  sizeof(received)), (unsigned int)sizeof(received));
	KUNIT_EXPECT_MEMEQ(test, received, inbound, sizeof(inbound));

	mtk_gnss_stop(&ctx->gnss);
	KUNIT_EXPECT_EQ(test, ctx->backend.power_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&ctx->gnss, outbound,
					     sizeof(outbound)), -EPIPE);
}

static void mtk_gnss_test_tx_failure_requires_reset(struct kunit *test)
{
	static const u8 payload[] = { 0x01 };
	struct mtk_gnss_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	ctx->backend.tx_result = -EIO;
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&ctx->gnss, payload,
					     sizeof(payload)), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->gnss.faulted);
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), -EIO);

	mtk_gnss_transport_reset(&ctx->gnss);
	ctx->backend.tx_result = 0;
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	KUNIT_EXPECT_EQ(test, ctx->backend.power_on_calls, 2U);
}

static void mtk_gnss_test_power_callback_can_feed_full_stp(struct kunit *test)
{
	static const u8 inbound[] = { 0x61, 0x62, 0x63 };
	struct mtk_gnss_test *ctx = test->priv;
	u8 frame[MTK_STP_HEADER_SIZE + sizeof(inbound) + MTK_STP_TRAILER_SIZE];
	size_t frame_length;

	frame_length = mtk_gnss_test_full_frame(test, frame, sizeof(frame), 0, 7,
						     inbound, sizeof(inbound));
	ctx->backend.loopback_frame = frame;
	ctx->backend.loopback_frame_len = frame_length;
	ctx->backend.loopback_from_power = true;
	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	KUNIT_EXPECT_EQ(test, ctx->backend.loopback_result, 0);
	/* Input during power-on is deliberately dropped until start succeeds. */
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo), 0U);

	frame_length = mtk_gnss_test_full_frame(test, frame, sizeof(frame), 1, 7,
						     inbound, sizeof(inbound));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(ctx->session, frame,
								  frame_length), 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo),
				(unsigned int)sizeof(inbound));

	kfifo_reset(&ctx->gnss.gdev->read_fifo);
	frame_length = mtk_gnss_test_full_frame(test, frame, sizeof(frame), 2, 7,
						     inbound, sizeof(inbound));
	ctx->backend.loopback_frame_len = frame_length;
	mtk_gnss_stop(&ctx->gnss);
	KUNIT_EXPECT_EQ(test, ctx->backend.loopback_result, 0);
	/* A frame received before power-off completes remains visible to userspace. */
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo),
				(unsigned int)sizeof(inbound));
}

static void mtk_gnss_test_fifo_overflow_is_reported(struct kunit *test)
{
	static const u8 inbound[] = { 0x41, 0x42, 0x43 };
	static const u8 filler[4095];
	struct mtk_gnss_test *ctx = test->priv;
	u8 frame[MTK_STP_HEADER_SIZE + sizeof(inbound) + MTK_STP_TRAILER_SIZE];
	size_t frame_length;
	unsigned int filled;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	filled = kfifo_in(&ctx->gnss.gdev->read_fifo, filler, sizeof(filler));
	KUNIT_ASSERT_EQ(test, filled, (unsigned int)sizeof(filler));
	frame_length = mtk_gnss_test_full_frame(test, frame, sizeof(frame), 0, 7,
						     inbound, sizeof(inbound));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(ctx->session, frame,
								  frame_length), -ENOSPC);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo), 4096U);
}

static void mtk_gnss_test_power_off_failure_requires_reset(struct kunit *test)
{
	struct mtk_gnss_test *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	ctx->backend.power_off_result = -EIO;
	mtk_gnss_stop(&ctx->gnss);
	KUNIT_EXPECT_TRUE(test, ctx->gnss.faulted);
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), -EIO);

	mtk_gnss_transport_reset(&ctx->gnss);
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
}

static void mtk_gnss_test_reregister(struct kunit *test)
{
	struct mtk_gnss_test *ctx = test->priv;
	struct mtk_gnss_transport transport = {
		.set_power = mtk_gnss_test_set_power,
		.context = &ctx->backend,
	};
	int ret;

	ret = mtk_gnss_register(&ctx->gnss, ctx->dev, ctx->session_lease,
				&transport);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	mtk_gnss_stop(&ctx->gnss);

	mtk_gnss_unregister(&ctx->gnss);
	ret = mtk_gnss_register(&ctx->gnss, ctx->dev, ctx->session_lease,
				&transport);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void mtk_gnss_test_unregistered_endpoint_is_safe(struct kunit *test)
{
	static const u8 payload[] = { 0x01 };
	struct mtk_gnss gnss = { };

	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&gnss), -ENODEV);
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&gnss, payload, sizeof(payload)),
			-ENODEV);
	mtk_gnss_stop(&gnss);
	mtk_gnss_transport_reset(&gnss);
	mtk_gnss_unregister(&gnss);
}

static void mtk_gnss_test_teardown_waits_for_transition_and_serializes(
									struct kunit *test)
{
	struct mtk_gnss_test_unregister first;
	struct mtk_gnss_test_unregister second;
	struct mtk_gnss_test *ctx = test->priv;
	struct task_struct *first_task;
	struct task_struct *second_task;
	unsigned long waited;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	init_completion(&first.entered);
	init_completion(&first.done);
	init_completion(&second.entered);
	init_completion(&second.done);
	first.gnss = &ctx->gnss;
	second.gnss = &ctx->gnss;
	ctx->backend.block_power_off = true;
	first_task = kthread_run(mtk_gnss_test_unregister_thread, &first,
				 "mtk-gnss-unregister-1");
	KUNIT_ASSERT_FALSE(test, IS_ERR(first_task));
	waited = wait_for_completion_timeout(&ctx->backend.power_off_entered,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	second_task = kthread_run(mtk_gnss_test_unregister_thread, &second,
				  "mtk-gnss-unregister-2");
	KUNIT_ASSERT_FALSE(test, IS_ERR(second_task));
	waited = wait_for_completion_timeout(&second.entered, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	/* The second void teardown must wait, not detach or put the same gdev. */
	waited = wait_for_completion_timeout(&second.done, msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, waited, 0UL);
	ctx->backend.block_power_off = false;
	complete(&ctx->backend.allow_power_off);
	waited = wait_for_completion_timeout(&first.done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	waited = wait_for_completion_timeout(&second.done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->backend.power_off_calls, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->gnss.registered);
}

static struct kunit_case mtk_gnss_test_cases[] = {
	KUNIT_CASE(mtk_gnss_test_lifecycle_and_stream),
	KUNIT_CASE(mtk_gnss_test_tx_failure_requires_reset),
	KUNIT_CASE(mtk_gnss_test_power_callback_can_feed_full_stp),
	KUNIT_CASE(mtk_gnss_test_fifo_overflow_is_reported),
	KUNIT_CASE(mtk_gnss_test_power_off_failure_requires_reset),
	KUNIT_CASE(mtk_gnss_test_reregister),
	KUNIT_CASE(mtk_gnss_test_unregistered_endpoint_is_safe),
	KUNIT_CASE(mtk_gnss_test_teardown_waits_for_transition_and_serializes),
	{}
};

static struct kunit_suite mtk_gnss_test_suite = {
	.name = "mtk-gnss-consys",
	.init = mtk_gnss_test_init,
	.exit = mtk_gnss_test_exit,
	.test_cases = mtk_gnss_test_cases,
};

kunit_test_suite(mtk_gnss_test_suite);

MODULE_LICENSE("GPL");
