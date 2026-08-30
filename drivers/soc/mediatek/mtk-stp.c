// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-independent MediaTek STP frame codec and multiplexer.
 *
 * Vendor evidence establishes a four-byte header and two-byte trailer.  The
 * initial mandatory mode carries zero checksum/trailer bytes; full mode uses
 * a header byte sum and CRC-16 of the payload.  Transport ownership, WMT
 * semantics, function control, power saving, sequencing windows and retries
 * intentionally remain outside this foundation.
 */

#include <linux/crc16.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mtk-stp.h>

static bool mtk_stp_supported_channel(u8 channel)
{
	return channel == MTK_STP_GNSS || channel == MTK_STP_WMT;
}

static size_t mtk_stp_max_payload(enum mtk_stp_mode mode)
{
	return mode == MTK_STP_MODE_MANDATORY ?
		MTK_STP_MANDATORY_MAX_PAYLOAD : MTK_STP_MAX_PAYLOAD;
}

static u8 mtk_stp_header_checksum(const u8 *header)
{
	return header[0] + header[1] + header[2];
}

static bool mtk_stp_is_sync(const struct mtk_stp *stp, u8 byte)
{
	if (stp->mode == MTK_STP_MODE_FULL)
		return (byte & 0xc0) == 0x80;

	return byte & 0x80;
}

static void mtk_stp_reset_frame(struct mtk_stp *stp)
{
	stp->rx_phase = MTK_STP_RX_HEADER;
	stp->header_len = 0;
	stp->trailer_len = 0;
	stp->payload_len = 0;
	stp->payload_pos = 0;
}

void mtk_stp_init(struct mtk_stp *stp, enum mtk_stp_mode mode)
{
	if (!stp)
		return;

	memset(stp, 0, sizeof(*stp));
	stp->mode = mode;
	mtk_stp_reset_frame(stp);
}
EXPORT_SYMBOL_GPL(mtk_stp_init);

void mtk_stp_reset(struct mtk_stp *stp)
{
	if (stp)
		mtk_stp_reset_frame(stp);
}
EXPORT_SYMBOL_GPL(mtk_stp_reset);

int mtk_stp_register_client(struct mtk_stp *stp, enum mtk_stp_channel channel,
			    mtk_stp_rx_cb_t receive, void *context)
{
	if (!stp || !receive || !mtk_stp_supported_channel(channel))
		return -EINVAL;

	if (stp->clients[channel].receive)
		return -EBUSY;

	stp->clients[channel].receive = receive;
	stp->clients[channel].context = context;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_stp_register_client);

void mtk_stp_unregister_client(struct mtk_stp *stp,
			       enum mtk_stp_channel channel)
{
	if (!stp || !mtk_stp_supported_channel(channel))
		return;

	stp->clients[channel].receive = NULL;
	stp->clients[channel].context = NULL;
}
EXPORT_SYMBOL_GPL(mtk_stp_unregister_client);

int mtk_stp_encode(enum mtk_stp_mode mode, enum mtk_stp_channel channel,
		   u8 sequence, u8 ack, const u8 *payload, size_t payload_len,
		   u8 *frame, size_t frame_size, size_t *frame_len)
{
	size_t required;
	u16 crc;

	if ((!payload && payload_len) || !frame || !frame_len ||
	    !mtk_stp_supported_channel(channel))
		return -EINVAL;
	if (mode != MTK_STP_MODE_MANDATORY && mode != MTK_STP_MODE_FULL)
		return -EINVAL;
	if (!payload_len)
		return -EOPNOTSUPP;
	if (payload_len > mtk_stp_max_payload(mode) || payload_len > 0x0fff)
		return -EMSGSIZE;

	required = MTK_STP_HEADER_SIZE + payload_len + MTK_STP_TRAILER_SIZE;
	if (frame_size < required)
		return -ENOSPC;

	if (mode == MTK_STP_MODE_MANDATORY)
		frame[0] = 0x80;
	else
		frame[0] = 0x80 | ((sequence & 0x7) << 3) | (ack & 0x7);
	frame[1] = (channel << 4) | ((payload_len >> 8) & 0xf);
	frame[2] = payload_len;
	frame[3] = mode == MTK_STP_MODE_FULL ?
		mtk_stp_header_checksum(frame) : 0;
	memcpy(frame + MTK_STP_HEADER_SIZE, payload, payload_len);

	if (mode == MTK_STP_MODE_FULL) {
		crc = crc16(0, payload, payload_len);
		frame[MTK_STP_HEADER_SIZE + payload_len] = crc;
		frame[MTK_STP_HEADER_SIZE + payload_len + 1] = crc >> 8;
	} else {
		frame[MTK_STP_HEADER_SIZE + payload_len] = 0;
		frame[MTK_STP_HEADER_SIZE + payload_len + 1] = 0;
	}

	*frame_len = required;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_stp_encode);

static int mtk_stp_parse_header(struct mtk_stp *stp)
{
	u8 channel;
	u16 payload_len;

	if (stp->mode == MTK_STP_MODE_FULL) {
		if ((stp->header[0] & 0xc0) != 0x80 ||
		    stp->header[3] != mtk_stp_header_checksum(stp->header))
			return -EBADMSG;
	} else if (!(stp->header[0] & 0x80)) {
		return -EBADMSG;
	}

	channel = (stp->header[1] & 0x70) >> 4;
	payload_len = ((stp->header[1] & 0xf) << 8) | stp->header[2];
	if (channel > MTK_STP_MAX_CHANNEL ||
	    payload_len > mtk_stp_max_payload(stp->mode))
		return -EMSGSIZE;
	if (!payload_len)
		return -EOPNOTSUPP;

	stp->channel = channel;
	stp->nak = stp->header[1] >> 7;
	stp->sequence = (stp->header[0] >> 3) & 0x7;
	stp->ack = stp->header[0] & 0x7;
	stp->payload_len = payload_len;
	stp->payload_pos = 0;
	stp->rx_phase = MTK_STP_RX_PAYLOAD;

	return 0;
}

static int mtk_stp_finish_frame(struct mtk_stp *stp, bool *callback_error)
{
	u16 crc;
	int ret = 0;

	if (stp->mode == MTK_STP_MODE_FULL) {
		crc = crc16(0, stp->payload, stp->payload_len);
		if (stp->trailer[0] != (u8)crc ||
		    stp->trailer[1] != (u8)(crc >> 8))
			return -EBADMSG;
	}

	*callback_error = false;
	if (stp->payload_len && mtk_stp_supported_channel(stp->channel) &&
	    stp->clients[stp->channel].receive)
		ret = stp->clients[stp->channel].receive(stp->clients[stp->channel].context,
							      stp->payload,
							      stp->payload_len);
	if (ret)
		*callback_error = true;
	else
		stp->frames++;

	return ret;
}

static void mtk_stp_record_error(struct mtk_stp *stp, int error,
				 bool callback_error)
{
	stp->rx_errors++;
	if (callback_error)
		stp->callback_errors++;
	stp->last_error = error;
}

static int mtk_stp_feed_header(struct mtk_stp *stp, u8 byte)
{
	int ret;

	stp->header[stp->header_len++] = byte;
	while (stp->header_len) {
		if (!mtk_stp_is_sync(stp, stp->header[0])) {
			memmove(stp->header, stp->header + 1, --stp->header_len);
			stp->dropped_bytes++;
			continue;
		}
		if (stp->header_len < MTK_STP_HEADER_SIZE)
			return 0;

		ret = mtk_stp_parse_header(stp);
		if (!ret)
			return 0;

		/* Slide one byte to consider overlapping sync candidates. */
		memmove(stp->header, stp->header + 1, --stp->header_len);
		stp->dropped_bytes++;
		return ret;
	}

	return 0;
}

int mtk_stp_rx(struct mtk_stp *stp, const u8 *data, size_t length)
{
	size_t i;
	int first_error = 0;

	if (!stp || (!data && length))
		return -EINVAL;
	if (stp->mode != MTK_STP_MODE_MANDATORY &&
	    stp->mode != MTK_STP_MODE_FULL)
		return -EINVAL;

	for (i = 0; i < length; i++) {
		int frame_ret = 0;
		bool callback_error = false;

		switch (stp->rx_phase) {
		case MTK_STP_RX_HEADER:
			frame_ret = mtk_stp_feed_header(stp, data[i]);
			break;
		case MTK_STP_RX_PAYLOAD:
			stp->payload[stp->payload_pos++] = data[i];
			if (stp->payload_pos == stp->payload_len)
				stp->rx_phase = MTK_STP_RX_TRAILER;
			break;
		case MTK_STP_RX_TRAILER:
			stp->trailer[stp->trailer_len++] = data[i];
			if (stp->trailer_len == MTK_STP_TRAILER_SIZE) {
				frame_ret = mtk_stp_finish_frame(stp, &callback_error);
				mtk_stp_reset_frame(stp);
			}
			break;
		}

		if (frame_ret) {
			mtk_stp_record_error(stp, frame_ret, callback_error);
			if (!first_error)
				first_error = frame_ret;
		}
		if (stp->rx_phase == MTK_STP_RX_TRAILER &&
		    stp->trailer_len == MTK_STP_TRAILER_SIZE)
			mtk_stp_reset_frame(stp);
	}

	return first_error;
}
EXPORT_SYMBOL_GPL(mtk_stp_rx);

MODULE_DESCRIPTION("MediaTek STP codec and WMT/GNSS multiplexer");
MODULE_LICENSE("GPL");
