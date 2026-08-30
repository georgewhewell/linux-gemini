/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek Serial Transport Protocol (STP) framing helpers.
 *
 * This is deliberately a transport-neutral, kernel-internal interface.  It
 * neither discovers hardware nor sends a WMT command.  A future BTIF transport
 * supplies bytes to mtk_stp_rx() and sends frames returned by mtk_stp_encode().
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_STP_H
#define __LINUX_SOC_MEDIATEK_MTK_STP_H

#include <linux/types.h>

#define MTK_STP_HEADER_SIZE	4
#define MTK_STP_TRAILER_SIZE	2
#define MTK_STP_MAX_PAYLOAD	2048
#define MTK_STP_MANDATORY_MAX_PAYLOAD	1999
#define MTK_STP_MAX_CHANNEL	7

/* The only clients exposed by this initial mux. */
enum mtk_stp_channel {
	MTK_STP_GNSS	= 2,
	MTK_STP_WMT	= 4,
};

enum mtk_stp_mode {
	/* Initial mode: fixed prefix and zero integrity fields on transmit. */
	MTK_STP_MODE_MANDATORY,
	/* Normal mode: sequence/ack, header checksum and payload CRC-16. */
	MTK_STP_MODE_FULL,
};

typedef int (*mtk_stp_rx_cb_t)(void *context, const u8 *payload,
				      size_t length);

struct mtk_stp_client {
	mtk_stp_rx_cb_t receive;
	void *context;
};

enum mtk_stp_rx_phase {
	MTK_STP_RX_HEADER,
	MTK_STP_RX_PAYLOAD,
	MTK_STP_RX_TRAILER,
};

/*
 * The caller must serialize mtk_stp_rx() with itself and client unregister.
 * Callbacks run synchronously in the caller's context.  FULL mode only
 * handles data frames here: header-only ACK/window control and the vendor
 * resynchronization handshake need a transport owner.
 */
struct mtk_stp {
	enum mtk_stp_mode mode;
	struct mtk_stp_client clients[MTK_STP_MAX_CHANNEL + 1];
	enum mtk_stp_rx_phase rx_phase;
	u8 header[MTK_STP_HEADER_SIZE];
	u8 header_len;
	u8 trailer[MTK_STP_TRAILER_SIZE];
	u8 trailer_len;
	u8 channel;
	u8 nak;
	u8 sequence;
	u8 ack;
	u16 payload_len;
	u16 payload_pos;
	u8 payload[MTK_STP_MAX_PAYLOAD];
	/* Lifetime receive diagnostics, reset only by mtk_stp_init(). */
	u32 rx_errors;
	u32 callback_errors;
	u32 dropped_bytes;
	u32 frames;
	int last_error;
};

void mtk_stp_init(struct mtk_stp *stp, enum mtk_stp_mode mode);
void mtk_stp_reset(struct mtk_stp *stp);

int mtk_stp_register_client(struct mtk_stp *stp, enum mtk_stp_channel channel,
			    mtk_stp_rx_cb_t receive, void *context);
void mtk_stp_unregister_client(struct mtk_stp *stp,
			       enum mtk_stp_channel channel);

/*
 * Encode exactly one STP frame.  In mandatory mode this emits the fixed
 * 0x80 prefix and zero integrity fields used by stp_send_data_no_ps().
 * FULL mode does not implement windowing,
 * acknowledgment scheduling or retransmission; sequence and ack are caller
 * supplied protocol fields.  Zero-length frames return -EOPNOTSUPP: full-mode
 * header-only control packets require separate protocol handling.
 */
int mtk_stp_encode(enum mtk_stp_mode mode, enum mtk_stp_channel channel,
		   u8 sequence, u8 ack, const u8 *payload, size_t payload_len,
		   u8 *frame, size_t frame_size, size_t *frame_len);

/*
 * Feed an arbitrary byte fragment.  Every supplied byte is consumed even
 * when this returns an error: parsing resynchronizes and continues with later
 * frames in the same fragment.  The return value is the first malformed-frame
 * or callback error in the fragment (zero when none); rx_errors,
 * callback_errors, dropped_bytes and last_error record the full outcome.
 * Partial frames return zero and are retained for the next call.
 */
int mtk_stp_rx(struct mtk_stp *stp, const u8 *data, size_t length);

#endif /* __LINUX_SOC_MEDIATEK_MTK_STP_H */
