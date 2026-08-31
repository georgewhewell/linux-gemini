/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Transport-neutral MediaTek WMT packet and transaction helpers.
 *
 * This interface deliberately models no controller bring-up sequence.  It
 * provides the packet envelope shared by vendor WMT implementations and a
 * single-flight request/response fence for a future transport owner.
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_WMT_H
#define __LINUX_SOC_MEDIATEK_MTK_WMT_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mtk-stp.h>

#define MTK_WMT_HEADER_SIZE	4
#define MTK_WMT_MAX_DATA	(MTK_STP_MAX_PAYLOAD - MTK_WMT_HEADER_SIZE)
#define MTK_WMT_TRACE_EVENTS	8
#define MTK_WMT_TRACE_BYTES	8

/*
 * Vendor drivers/misc/mediatek/connectivity/common/common_main/core/include/
 * wmt_core.h defines WMT_PKT_TYPE_CMD as 1 and WMT_PKT_TYPE_EVENT as 2.
 * The paired core/wmt_core.c command and event vectors use those values at
 * byte zero.
 */
#define MTK_WMT_DIR_EVENT	2
#define MTK_WMT_DIR_COMMAND	1

struct mtk_wmt_packet {
	u8 opcode;
	/* Raw SDU bytes; this may be empty. */
	const u8 *data;
	size_t data_len;
};

/*
 * A response is accepted only if every field below is equal.  data points to
 * caller-owned memory which must stay valid until mtk_wmt_transaction()
 * returns.
 */
struct mtk_wmt_match {
	u8 opcode;
	/* Exact raw-SDU match; this may be empty. */
	const u8 *data;
	size_t data_len;
};

/* Bounded evidence for one explicitly armed WMT transaction. */
struct mtk_wmt_trace_event {
	u8 opcode;
	u16 data_len;
	u32 elapsed_ms;
	u8 prefix_length;
	u8 tail_length;
	u8 prefix[MTK_WMT_TRACE_BYTES];
	u8 tail[MTK_WMT_TRACE_BYTES];
};

struct mtk_wmt_trace {
	u8 command_opcode;
	u64 decoded_events;
	u64 decode_failures;
	u32 malformed_length;
	u8 malformed_prefix_length;
	u8 malformed_tail_length;
	u8 malformed_prefix[MTK_WMT_TRACE_BYTES];
	u8 malformed_tail[MTK_WMT_TRACE_BYTES];
	u8 event_count;
	struct mtk_wmt_trace_event events[MTK_WMT_TRACE_EVENTS];
};

typedef int (*mtk_wmt_tx_cb_t)(void *context, const u8 *frame,
				      size_t frame_len);
/*
 * Transmit exactly one complete WMT packet, without an STP envelope.  This is
 * for a transport owner which already owns STP framing and its full-mode
 * sequence/window lifetime; it cannot be substituted with an enum mode alone.
 */
typedef int (*mtk_wmt_payload_tx_cb_t)(void *context, const u8 *packet,
					      size_t packet_len);
typedef void (*mtk_wmt_unsolicited_cb_t)(void *context,
					 const struct mtk_wmt_packet *packet);

struct mtk_wmt {
	/* Serializes callers: at most one command can be in flight. */
	struct mutex transaction_lock;
	/* Serializes reset requests with entry to the transport callback. */
	struct mutex issue_lock;
	/* Serializes receive/reset with the waiting transaction. */
	spinlock_t state_lock;
	/* Reset requests announced before issue_lock is claimed by a transaction. */
	atomic_t reset_requests;
	struct completion response;
	enum mtk_stp_mode stp_mode;
	mtk_wmt_tx_cb_t transmit;
	mtk_wmt_payload_tx_cb_t transmit_payload;
	void *transport_context;
	mtk_wmt_unsolicited_cb_t unsolicited;
	void *unsolicited_context;
	struct mtk_wmt_match expected;
	/* A capture transaction matches opcode/length and copies its raw SDU. */
	bool capture_pending;
	u8 capture_opcode;
	size_t capture_data_len;
	u8 *capture_data;
	u32 generation;
	int result;
	bool pending;
	/* A timeout/cancel may leave an indistinguishable late reply behind. */
	bool reset_required;
	/* Protected by state_lock; armed only for bounded failure evidence. */
	bool trace_active;
	unsigned long trace_started;
	struct mtk_wmt_trace trace;
};

void mtk_wmt_init(struct mtk_wmt *wmt, enum mtk_stp_mode stp_mode,
		  mtk_wmt_tx_cb_t transmit, void *transport_context);
/*
 * Initialize a WMT matcher on a pre-framed-payload transport.  The callback
 * gets the complete WMT command packet, starting with type/opcode/length.
 * It is the caller's responsibility to own the underlying mandatory or
 * full-STP parser, ACK/window state, timeouts, and recovery lifecycle.
 */
void mtk_wmt_init_payload_transport(struct mtk_wmt *wmt,
				    enum mtk_stp_mode stp_mode,
				    mtk_wmt_payload_tx_cb_t transmit,
				    void *transport_context);
void mtk_wmt_set_unsolicited(struct mtk_wmt *wmt,
			     mtk_wmt_unsolicited_cb_t receive, void *context);

/*
 * Build a command WMT payload. WMT's four-byte header contains type, opcode,
 * and little-endian SDU length. The SDU is raw operation-specific data and
 * may be empty, so the packet is exactly four bytes plus that declared length.
 */
int mtk_wmt_encode_command(u8 opcode, const u8 *data, size_t data_len,
			   u8 *packet, size_t packet_size,
			   size_t *packet_len);

/* Decode one complete WMT event payload, rejecting direction and length lies. */
int mtk_wmt_decode_event(const u8 *packet, size_t packet_len,
			 struct mtk_wmt_packet *event);

/* STP WMT-channel callback; it may also be called directly by a transport. */
int mtk_wmt_receive(void *context, const u8 *payload, size_t payload_len);

/* Arm/snapshot/disarm bounded event evidence around one caller-selected request. */
void mtk_wmt_trace_begin(struct mtk_wmt *wmt, u8 command_opcode);
void mtk_wmt_trace_end(struct mtk_wmt *wmt, struct mtk_wmt_trace *trace);

/*
 * Send exactly one command and await one exact reply.  On timeout, signal, or
 * transport failure this returns an error and fences subsequent commands with
 * -EPIPE.  The owner must call mtk_wmt_reset() after it has recovered the
 * underlying transport/controller before sending another command.
 */
int mtk_wmt_transaction(struct mtk_wmt *wmt, u8 opcode,
			const u8 *data, size_t data_len,
			const struct mtk_wmt_match *expected,
			unsigned long timeout);

/*
 * As mtk_wmt_transaction(), but accept an event by opcode and exact SDU
 * length, then copy that raw SDU into @reply.  This exists for controller
 * commands such as the vendor MT6631 efuse read whose final byte is data, not
 * a pre-known status value.  @reply_len must equal @expected_data_len.
 */
int mtk_wmt_transaction_capture(struct mtk_wmt *wmt, u8 opcode,
				const u8 *data, size_t data_len,
				u8 expected_opcode, size_t expected_data_len,
				u8 *reply, size_t reply_len,
				unsigned long timeout);

/*
 * Local cancellation/generation fence; this never emits a controller reset.
 *
 * Calling this function announces a reset before waiting for issue_lock. A
 * transaction which has not claimed issue_lock observes that request and does
 * not enter its transmit callback. Once the callback has claimed issue_lock,
 * reset waits for it to return; it cannot revoke or interrupt that callback.
 * The transport owner must first quiesce/disconnect a non-cancellable
 * transport callback, and this function must not be called from that callback
 * or another atomic context.
 */
void mtk_wmt_reset(struct mtk_wmt *wmt);

#endif /* __LINUX_SOC_MEDIATEK_MTK_WMT_H */
