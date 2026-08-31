// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit coverage for the hardware-independent MediaTek STP codec.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mtk-stp.h>

struct mtk_stp_test_rx {
	u8 payload[MTK_STP_MAX_PAYLOAD];
	size_t length;
	unsigned int calls;
	unsigned int errors_left;
	int error;
};

struct mtk_stp_test_control_rx {
	struct mtk_stp_control control;
	unsigned int calls;
	unsigned int errors_left;
	int error;
};

static int mtk_stp_test_receive(void *context, const u8 *payload,
				size_t length)
{
	struct mtk_stp_test_rx *rx = context;

	if (length > sizeof(rx->payload))
		return -EMSGSIZE;

	memcpy(rx->payload, payload, length);
	rx->length = length;
	rx->calls++;
	if (rx->errors_left) {
		rx->errors_left--;
		return rx->error;
	}

	return 0;
}

static int mtk_stp_test_receive_control(void *context,
					const struct mtk_stp_control *control)
{
	struct mtk_stp_test_control_rx *rx = context;

	rx->control = *control;
	rx->calls++;
	if (rx->errors_left) {
		rx->errors_left--;
		return rx->error;
	}

	return 0;
}

static void mtk_stp_test_encode_mandatory(struct kunit *test)
{
	static const u8 payload[] = { 0xa1, 0xb2, 0xc3 };
	static const u8 expected[] = {
		0x80, 0x40, 0x03, 0x00, 0xa1, 0xb2, 0xc3, 0x00, 0x00,
	};
	u8 frame[sizeof(expected)];
	size_t frame_len = 0;

	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT,
				       3, 5, payload, sizeof(payload), frame,
				       sizeof(frame), &frame_len), 0);
	KUNIT_EXPECT_EQ(test, frame_len, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, frame, expected, sizeof(expected));
}

static void mtk_stp_test_encode_full(struct kunit *test)
{
	static const u8 payload[] = { 0x01, 0x02, 0x03 };
	static const u8 expected[] = {
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	u8 frame[sizeof(expected)];
	size_t frame_len = 0;

	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_GNSS, 3, 5,
				       payload, sizeof(payload), frame, sizeof(frame),
				       &frame_len), 0);
	KUNIT_EXPECT_EQ(test, frame_len, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, frame, expected, sizeof(expected));
}

static void mtk_stp_test_fragmented_full_rx(struct kunit *test)
{
	static const u8 frame[] = {
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	static const u8 expected[] = { 0x01, 0x02, 0x03 };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, rx), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame, 1), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame + 1, 3), 0);
	KUNIT_EXPECT_EQ(test, rx->calls, 0U);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, frame + 4, sizeof(frame) - 4), 0);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, rx->length, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, rx->payload, expected, sizeof(expected));
	KUNIT_EXPECT_EQ(test, stp->sequence, 3);
	KUNIT_EXPECT_EQ(test, stp->ack, 5);
}

static void mtk_stp_test_channel_isolation(struct kunit *test)
{
	static const u8 wmt_payload[] = { 0x57, 0x4d };
	static const u8 gnss_payload[] = { 0x47 };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *wmt_rx;
	struct mtk_stp_test_rx *gnss_rx;
	u8 frames[32];
	size_t wmt_len = 0;
	size_t gnss_len = 0;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	wmt_rx = kunit_kzalloc(test, sizeof(*wmt_rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wmt_rx);
	gnss_rx = kunit_kzalloc(test, sizeof(*gnss_rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, gnss_rx);
	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, wmt_rx), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, gnss_rx), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT, 0,
				       0, wmt_payload, sizeof(wmt_payload), frames,
				       sizeof(frames), &wmt_len), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_GNSS, 0,
				       0, gnss_payload, sizeof(gnss_payload),
				       frames + wmt_len, sizeof(frames) - wmt_len,
				       &gnss_len), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frames, wmt_len + gnss_len), 0);
	KUNIT_EXPECT_EQ(test, wmt_rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, gnss_rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, wmt_rx->length, sizeof(wmt_payload));
	KUNIT_EXPECT_EQ(test, gnss_rx->length, sizeof(gnss_payload));
	KUNIT_EXPECT_MEMEQ(test,
			   wmt_rx->payload, wmt_payload, sizeof(wmt_payload));
	KUNIT_EXPECT_MEMEQ(test,
			   gnss_rx->payload, gnss_payload, sizeof(gnss_payload));
}

static void mtk_stp_test_integrity_and_length(struct kunit *test)
{
	static const u8 bad_header[] = {
		0x9d, 0x20, 0x03, 0xc1, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	static const u8 bad_crc[] = {
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa0,
	};
	static const u8 mandatory_header[] = { 0x80, 0x47, 0xd0, 0x00 };
	static const u8 full_header[] = { 0x80, 0x48, 0x01, 0xc9 };
	struct mtk_stp *stp;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, bad_header, sizeof(bad_header)), -EBADMSG);
	mtk_stp_reset(stp);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, bad_crc, sizeof(bad_crc)), -EBADMSG);
	mtk_stp_reset(stp);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, full_header, sizeof(full_header)), -EMSGSIZE);

	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, mandatory_header, sizeof(mandatory_header)),
			-EMSGSIZE);
}

static void mtk_stp_test_error_then_valid_frame(struct kunit *test)
{
	static const u8 bad_then_valid[] = {
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa0,
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	static const u8 payload[] = { 0x01, 0x02, 0x03 };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, rx), 0);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, bad_then_valid, sizeof(bad_then_valid)),
			-EBADMSG);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_MEMEQ(test, rx->payload, payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, stp->rx_errors, 1U);
	KUNIT_EXPECT_EQ(test, stp->callback_errors, 0U);
	KUNIT_EXPECT_EQ(test, stp->last_error, -EBADMSG);
}

static void mtk_stp_test_false_sync_then_valid_frame(struct kunit *test)
{
	/* Byte 1 is a false full-mode sync; byte 4 starts the valid frame. */
	static const u8 false_sync_then_valid[] = {
		0x9d, 0x90, 0x03, 0x31,
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	static const u8 payload[] = { 0x01, 0x02, 0x03 };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, rx), 0);

	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, false_sync_then_valid,
				   sizeof(false_sync_then_valid)), -EBADMSG);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_MEMEQ(test, rx->payload, payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, stp->rx_errors, 2U);
	KUNIT_EXPECT_EQ(test, stp->dropped_bytes, 4U);
}

static void mtk_stp_test_false_sync_all_splits(struct kunit *test)
{
	static const u8 false_sync_then_valid[] = {
		0x9d, 0x90, 0x03, 0x31,
		0x9d, 0x20, 0x03, 0xc0, 0x01, 0x02, 0x03, 0x10, 0xa1,
	};
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;
	size_t i;
	int byte_error = 0;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);

	for (i = 0; i <= sizeof(false_sync_then_valid); i++) {
		int ret;
		int next;

		memset(rx, 0, sizeof(*rx));
		mtk_stp_init(stp, MTK_STP_MODE_FULL);
		KUNIT_ASSERT_EQ(test,
				mtk_stp_register_client(stp, MTK_STP_GNSS,
							mtk_stp_test_receive, rx), 0);
		ret = mtk_stp_rx(stp, false_sync_then_valid, i);
		next = mtk_stp_rx(stp, false_sync_then_valid + i,
				  sizeof(false_sync_then_valid) - i);
		if (!ret)
			ret = next;
		KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
		KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	}

	memset(rx, 0, sizeof(*rx));
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, rx), 0);
	for (i = 0; i < sizeof(false_sync_then_valid); i++) {
		int ret = mtk_stp_rx(stp, false_sync_then_valid + i, 1);

		if (!byte_error)
			byte_error = ret;
		KUNIT_EXPECT_TRUE(test, !ret || ret == -EBADMSG);
	}
	KUNIT_EXPECT_EQ(test, byte_error, -EBADMSG);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
}

static void mtk_stp_test_callback_error_then_valid_frame(struct kunit *test)
{
	static const u8 payload[] = { 0x01, 0x02, 0x03 };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;
	u8 frames[2 * (MTK_STP_HEADER_SIZE + sizeof(payload) +
			 MTK_STP_TRAILER_SIZE)];
	size_t frame_len;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_GNSS,
						mtk_stp_test_receive, rx), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_GNSS, 0, 0,
				       payload, sizeof(payload), frames, sizeof(frames),
				       &frame_len), 0);
	memcpy(frames + frame_len, frames, frame_len);
	rx->error = -EIO;
	rx->errors_left = 1;

	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frames, 2 * frame_len), -EIO);
	KUNIT_EXPECT_EQ(test, rx->calls, 2U);
	KUNIT_EXPECT_MEMEQ(test, rx->payload, payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, stp->rx_errors, 1U);
	KUNIT_EXPECT_EQ(test, stp->callback_errors, 1U);
	KUNIT_EXPECT_EQ(test, stp->frames, 1U);
}

static void mtk_stp_test_noise_prefix_then_valid_frame(struct kunit *test)
{
	static const u8 payload[] = { 0x57 };
	static const u8 noise[] = { 0x00, 0x55, 0x7f };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;
	u8 bytes[sizeof(noise) + MTK_STP_HEADER_SIZE + sizeof(payload) +
		 MTK_STP_TRAILER_SIZE];
	size_t frame_len;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, rx), 0);
	memcpy(bytes, noise, sizeof(noise));
	KUNIT_ASSERT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT, 0, 0,
				       payload, sizeof(payload), bytes + sizeof(noise),
				       sizeof(bytes) - sizeof(noise), &frame_len), 0);

	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, bytes, sizeof(noise) + frame_len), 0);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, stp->dropped_bytes, (u32)sizeof(noise));
	KUNIT_EXPECT_EQ(test, stp->rx_errors, 0U);
}

static void mtk_stp_test_nak_masks_channel(struct kunit *test)
{
	static const u8 frame[] = { 0x80, 0xc0, 0x01, 0x00, 0x5a, 0x00, 0x00 };
	static const u8 payload[] = { 0x5a };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, rx), 0);

	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame, sizeof(frame)), 0);
	KUNIT_EXPECT_EQ(test, stp->channel, (u8)MTK_STP_WMT);
	KUNIT_EXPECT_EQ(test, stp->nak, (u8)1);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_MEMEQ(test, rx->payload, payload, sizeof(payload));
}

static void mtk_stp_test_zero_payload_is_unsupported(struct kunit *test)
{
	static const u8 full_header[] = { 0x80, 0x20, 0x00, 0xa0 };
	u8 frame[MTK_STP_HEADER_SIZE + MTK_STP_TRAILER_SIZE];
	size_t frame_len = 0;
	struct mtk_stp *stp;

	KUNIT_EXPECT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_GNSS, 0, 0,
				       NULL, 0, frame, sizeof(frame), &frame_len),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_encode(MTK_STP_MODE_MANDATORY, MTK_STP_WMT, 0,
				       0, NULL, 0, frame, sizeof(frame), &frame_len),
			-EOPNOTSUPP);

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_rx(stp, full_header, sizeof(full_header)), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, stp->rx_errors, 1U);
}

static void mtk_stp_test_full_ack_encode_and_fragmented_rx(struct kunit *test)
{
	static const u8 expected[] = { 0x85, 0x00, 0x00, 0x85 };
	struct mtk_stp *stp;
	struct mtk_stp_test_control_rx *rx;
	u8 frame[MTK_STP_HEADER_SIZE];
	size_t frame_len = 0;
	size_t split;

	KUNIT_ASSERT_EQ(test, mtk_stp_encode_ack(5, frame, sizeof(frame), &frame_len), 0);
	KUNIT_EXPECT_EQ(test, frame_len, sizeof(expected));
	KUNIT_EXPECT_MEMEQ(test, frame, expected, sizeof(expected));
	KUNIT_EXPECT_EQ(test, mtk_stp_encode_ack(0, frame, sizeof(frame) - 1,
						   &frame_len), -ENOSPC);

	for (split = 0; split <= sizeof(frame); split++) {
		stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, stp);
		rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, rx);
		mtk_stp_init(stp, MTK_STP_MODE_FULL);
		KUNIT_ASSERT_EQ(test, mtk_stp_register_control(stp,
						mtk_stp_test_receive_control, rx), 0);
		KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame, split), 0);
		KUNIT_EXPECT_EQ(test, rx->calls, split == sizeof(frame) ? 1U : 0U);
		KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame + split,
						sizeof(frame) - split), 0);
		KUNIT_EXPECT_EQ(test, rx->calls, 1U);
		KUNIT_EXPECT_EQ(test, rx->control.sequence, (u8)0);
		KUNIT_EXPECT_EQ(test, rx->control.ack, (u8)5);
		KUNIT_EXPECT_EQ(test, rx->control.nak, (u8)0);
		KUNIT_EXPECT_EQ(test, stp->control_frames, 1U);
	}
}

static void mtk_stp_test_control_callback_error_then_data(struct kunit *test)
{
	static const u8 payload[] = { 0x5a };
	struct mtk_stp *stp;
	struct mtk_stp_test_control_rx *control_rx;
	struct mtk_stp_test_rx *data_rx;
	u8 bytes[MTK_STP_HEADER_SIZE + MTK_STP_HEADER_SIZE + sizeof(payload) +
		 MTK_STP_TRAILER_SIZE];
	size_t ack_len;
	size_t data_len;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	control_rx = kunit_kzalloc(test, sizeof(*control_rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, control_rx);
	data_rx = kunit_kzalloc(test, sizeof(*data_rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data_rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_control(stp,
					mtk_stp_test_receive_control, control_rx), 0);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(stp, MTK_STP_WMT,
					mtk_stp_test_receive, data_rx), 0);
	KUNIT_ASSERT_EQ(test, mtk_stp_encode_ack(7, bytes, sizeof(bytes), &ack_len), 0);
	KUNIT_ASSERT_EQ(test, mtk_stp_encode(MTK_STP_MODE_FULL, MTK_STP_WMT, 1, 7,
					payload, sizeof(payload), bytes + ack_len,
					sizeof(bytes) - ack_len, &data_len), 0);
	control_rx->errors_left = 1;
	control_rx->error = -EIO;

	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, bytes, ack_len + data_len), -EIO);
	KUNIT_EXPECT_EQ(test, control_rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, data_rx->calls, 1U);
	KUNIT_EXPECT_EQ(test, stp->callback_errors, 1U);
	KUNIT_EXPECT_EQ(test, stp->frames, 1U);
	KUNIT_EXPECT_EQ(test, stp->control_frames, 0U);
}

static void mtk_stp_test_full_nak_is_masked_and_mandatory_control_rejected(
									 struct kunit *test)
{
	static const u8 full_ack_with_nak[] = { 0x83, 0x80, 0x00, 0x03 };
	static const u8 mandatory_zero_header[] = { 0x80, 0x00, 0x00, 0x00 };
	struct mtk_stp *stp;
	struct mtk_stp_test_control_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_control(stp,
					mtk_stp_test_receive_control, rx), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, full_ack_with_nak,
					sizeof(full_ack_with_nak)), 0);
	KUNIT_EXPECT_EQ(test, rx->control.ack, (u8)3);
	KUNIT_EXPECT_EQ(test, rx->control.nak, (u8)0);

	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, mandatory_zero_header,
					sizeof(mandatory_zero_header)), -EOPNOTSUPP);
}

static void mtk_stp_test_mandatory_is_permissive(struct kunit *test)
{
	static const u8 frame[] = {
		0xb8, 0x40, 0x01, 0xa5, 0x5a, 0x12, 0x34,
	};
	static const u8 expected[] = { 0x5a };
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_MANDATORY);
	KUNIT_ASSERT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, rx), 0);
	KUNIT_EXPECT_EQ(test, mtk_stp_rx(stp, frame, sizeof(frame)), 0);
	KUNIT_EXPECT_EQ(test, rx->calls, 1U);
	KUNIT_EXPECT_MEMEQ(test, rx->payload, expected, sizeof(expected));
}

static void mtk_stp_test_client_registration(struct kunit *test)
{
	struct mtk_stp *stp;
	struct mtk_stp_test_rx *rx;

	stp = kunit_kzalloc(test, sizeof(*stp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stp);
	rx = kunit_kzalloc(test, sizeof(*rx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rx);
	mtk_stp_init(stp, MTK_STP_MODE_FULL);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, rx), 0);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_register_client(stp, MTK_STP_WMT,
						mtk_stp_test_receive, rx), -EBUSY);
	KUNIT_EXPECT_EQ(test,
			mtk_stp_register_client(stp, 7, mtk_stp_test_receive, rx),
			-EINVAL);
}

static struct kunit_case mtk_stp_test_cases[] = {
	KUNIT_CASE(mtk_stp_test_encode_mandatory),
	KUNIT_CASE(mtk_stp_test_encode_full),
	KUNIT_CASE(mtk_stp_test_fragmented_full_rx),
	KUNIT_CASE(mtk_stp_test_channel_isolation),
	KUNIT_CASE(mtk_stp_test_integrity_and_length),
	KUNIT_CASE(mtk_stp_test_error_then_valid_frame),
	KUNIT_CASE(mtk_stp_test_false_sync_then_valid_frame),
	KUNIT_CASE(mtk_stp_test_false_sync_all_splits),
	KUNIT_CASE(mtk_stp_test_callback_error_then_valid_frame),
	KUNIT_CASE(mtk_stp_test_noise_prefix_then_valid_frame),
	KUNIT_CASE(mtk_stp_test_nak_masks_channel),
	KUNIT_CASE(mtk_stp_test_zero_payload_is_unsupported),
	KUNIT_CASE(mtk_stp_test_full_ack_encode_and_fragmented_rx),
	KUNIT_CASE(mtk_stp_test_control_callback_error_then_data),
	KUNIT_CASE(mtk_stp_test_full_nak_is_masked_and_mandatory_control_rejected),
	KUNIT_CASE(mtk_stp_test_mandatory_is_permissive),
	KUNIT_CASE(mtk_stp_test_client_registration),
	{ }
};

static struct kunit_suite mtk_stp_test_suite = {
	.name = "mtk-stp",
	.test_cases = mtk_stp_test_cases,
};

kunit_test_suite(mtk_stp_test_suite);

MODULE_LICENSE("GPL");
