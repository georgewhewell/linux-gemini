// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coverage for the MediaTek CONSYS GNSS STP endpoint. */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/string.h>

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
	struct mtk_stp *stp;
	const u8 *loopback_frame;
	size_t loopback_frame_len;
	bool loopback_from_transmit;
	bool loopback_from_power;
	int loopback_result;
};

struct mtk_gnss_test {
	struct device *dev;
	struct mtk_stp stp;
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
	if (backend->loopback_from_transmit)
		backend->loopback_result =
			mtk_stp_rx(backend->stp, backend->loopback_frame,
				   backend->loopback_frame_len);
	return backend->tx_result;
}

static int mtk_gnss_test_set_power(void *context, bool on)
{
	struct mtk_gnss_test_backend *backend = context;

	if (on) {
		backend->power_on_calls++;
		if (backend->loopback_from_power)
			backend->loopback_result =
				mtk_stp_rx(backend->stp, backend->loopback_frame,
					   backend->loopback_frame_len);
		return backend->power_on_result;
	}

	backend->power_off_calls++;
	if (backend->loopback_from_power)
		backend->loopback_result =
			mtk_stp_rx(backend->stp, backend->loopback_frame,
				   backend->loopback_frame_len);
	return backend->power_off_result;
}

static int mtk_gnss_test_init(struct kunit *test)
{
	struct mtk_gnss_test *ctx;
	struct mtk_gnss_transport transport;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->dev = kunit_device_register(test, "mtk-gnss-test");
	if (IS_ERR(ctx->dev))
		return PTR_ERR(ctx->dev);

	mtk_stp_init(&ctx->stp, MTK_STP_MODE_MANDATORY);
	ctx->backend.stp = &ctx->stp;
	transport.transmit = mtk_gnss_test_transmit;
	transport.set_power = mtk_gnss_test_set_power;
	transport.context = &ctx->backend;

	return mtk_gnss_register(&ctx->gnss, ctx->dev, &ctx->stp,
				 MTK_STP_MODE_MANDATORY, &transport);
}

static void mtk_gnss_test_exit(struct kunit *test)
{
	struct mtk_gnss_test *ctx = test->priv;

	mtk_gnss_unregister(&ctx->gnss);
}

static void mtk_gnss_test_lifecycle_and_stream(struct kunit *test)
{
	static const u8 outbound[] = { 0x24, 0x47, 0x50, 0x52, 0x4d, 0x43 };
	static const u8 expected_frame[] = {
		0x80, 0x20, 0x06, 0x00, 0x24, 0x47, 0x50, 0x52, 0x4d, 0x43,
		0x00, 0x00,
	};
	static const u8 inbound[] = { 0xa1, 0xb2, 0xc3 };
	struct mtk_gnss_test *ctx = test->priv;
	u8 frame[16];
	u8 received[sizeof(inbound)];
	size_t frame_len;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	KUNIT_EXPECT_EQ(test, ctx->backend.power_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&ctx->gnss, outbound,
					     sizeof(outbound)), (int)sizeof(outbound));
	KUNIT_EXPECT_EQ(test, ctx->backend.frame_len, sizeof(expected_frame));
	KUNIT_EXPECT_MEMEQ(test, ctx->backend.frame, expected_frame,
			   sizeof(expected_frame));

	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_MANDATORY,
					     MTK_STP_GNSS, 0, 0, inbound,
					     sizeof(inbound), frame, sizeof(frame),
					     &frame_len), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(&ctx->stp, frame, 2), 0);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(&ctx->stp, frame + 2, frame_len - 2), 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo),
			(unsigned int)sizeof(inbound));
	KUNIT_EXPECT_EQ(test,
			kfifo_out(&ctx->gnss.gdev->read_fifo, received,
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

static void mtk_gnss_test_callbacks_can_feed_stp(struct kunit *test)
{
	static const u8 inbound[] = { 0x61, 0x62, 0x63 };
	static const u8 outbound[] = { 0x64 };
	struct mtk_gnss_test *ctx = test->priv;
	u8 frame[MTK_STP_HEADER_SIZE + sizeof(inbound) + MTK_STP_TRAILER_SIZE];
	size_t frame_len;

	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_MANDATORY,
					     MTK_STP_GNSS, 0, 0, inbound,
					     sizeof(inbound), frame, sizeof(frame),
					     &frame_len), 0);
	ctx->backend.loopback_frame = frame;
	ctx->backend.loopback_frame_len = frame_len;
	ctx->backend.loopback_from_power = true;
	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	KUNIT_EXPECT_EQ(test, ctx->backend.loopback_result, 0);
	/* Receive during power-on is deliberately dropped until start succeeds. */
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo), 0U);

	ctx->backend.loopback_from_power = false;
	ctx->backend.loopback_from_transmit = true;
	KUNIT_EXPECT_EQ(test, mtk_gnss_write(&ctx->gnss, outbound,
					     sizeof(outbound)), (int)sizeof(outbound));
	KUNIT_EXPECT_EQ(test, ctx->backend.loopback_result, 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->gnss.gdev->read_fifo),
			(unsigned int)sizeof(inbound));

	kfifo_reset(&ctx->gnss.gdev->read_fifo);
	ctx->backend.loopback_from_transmit = false;
	ctx->backend.loopback_from_power = true;
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
	size_t frame_len;
	unsigned int filled;

	KUNIT_ASSERT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	filled = kfifo_in(&ctx->gnss.gdev->read_fifo, filler, sizeof(filler));
	KUNIT_ASSERT_EQ(test, filled, (unsigned int)sizeof(filler));
	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_MANDATORY,
					     MTK_STP_GNSS, 0, 0, inbound,
					     sizeof(inbound), frame, sizeof(frame),
					     &frame_len), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(&ctx->stp, frame, frame_len), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ctx->stp.callback_errors, 1U);
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
		.transmit = mtk_gnss_test_transmit,
		.set_power = mtk_gnss_test_set_power,
		.context = &ctx->backend,
	};
	int ret;

	ret = mtk_gnss_register(&ctx->gnss, ctx->dev, &ctx->stp,
				MTK_STP_MODE_MANDATORY, &transport);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_EQ(test, mtk_gnss_start(&ctx->gnss), 0);
	mtk_gnss_stop(&ctx->gnss);

	mtk_gnss_unregister(&ctx->gnss);
	ret = mtk_gnss_register(&ctx->gnss, ctx->dev, &ctx->stp,
				MTK_STP_MODE_MANDATORY, &transport);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static struct kunit_case mtk_gnss_test_cases[] = {
	KUNIT_CASE(mtk_gnss_test_lifecycle_and_stream),
	KUNIT_CASE(mtk_gnss_test_tx_failure_requires_reset),
	KUNIT_CASE(mtk_gnss_test_callbacks_can_feed_stp),
	KUNIT_CASE(mtk_gnss_test_fifo_overflow_is_reported),
	KUNIT_CASE(mtk_gnss_test_power_off_failure_requires_reset),
	KUNIT_CASE(mtk_gnss_test_reregister),
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
