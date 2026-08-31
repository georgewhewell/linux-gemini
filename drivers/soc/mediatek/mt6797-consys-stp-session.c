// SPDX-License-Identifier: GPL-2.0-only
/*
 * Transport-neutral MT6797 CONSYS full-STP session owner.
 *
 * Wire semantics deliberately mirror the retained MT6797 vendor STP core:
 *
 * - seven transmit credits and three-bit sequence numbers, initialized as
 *   txseq=0, txack=7, rxack=7, expected_rxseq=0 (stp_core.c:580-604 and
 *   stp_core.h:106-119);
 * - full data framing, retained frames, and a 180-ms retry timer
 *   (stp_core.c:1151-1201, stp_core.h:110-113);
 * - cumulative acknowledgement processing (stp_core.c:1225-1260);
 * - ACKs for accepted and duplicate data, with the production default NAK bit
 *   masked rather than interpreted (stp_core.c:1069-1101,2077-2093);
 * - four 0x7f bytes followed by retransmission from rxack+1 on timeout
 *   (stp_core.c:632-754).
 *
 * This is not a BTIF driver and cannot reach hardware on its own.  A future
 * owner must establish the documented mandatory-to-full WMT handover before it
 * calls begin_full(): wmt_core.c:747-809 selects BTIF mandatory mode, then
 * wmt_ic_soc.c:1008-1044 queries, changes and verifies the full mode. It must
 * quiesce the byte transport before quiescing this object. There is deliberately
 * no automatic post-error rearm capability.
 */

#include <linux/crc16.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_STP_SESSION_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

#define MT6797_CONSYS_STP_WINDOW_SIZE	7
#define MT6797_CONSYS_STP_SEQUENCE_COUNT	8
#define MT6797_CONSYS_STP_MAX_FRAME_SIZE \
	(MTK_STP_HEADER_SIZE + MTK_STP_MAX_PAYLOAD + MTK_STP_TRAILER_SIZE)

enum mt6797_consys_stp_rx_state {
	MT6797_CONSYS_STP_RX_SYNC,
	MT6797_CONSYS_STP_RX_HEADER,
	MT6797_CONSYS_STP_RX_PAYLOAD,
	MT6797_CONSYS_STP_RX_CRC,
	MT6797_CONSYS_STP_RX_RESYNC,
};

enum mt6797_consys_stp_session_state {
	MT6797_CONSYS_STP_NEW,
	MT6797_CONSYS_STP_BOOTSTRAP,
	MT6797_CONSYS_STP_ACTIVE,
	MT6797_CONSYS_STP_FENCED,
};

struct mt6797_consys_stp_tx_slot {
	bool valid;
	size_t frame_length;
	u8 frame[MT6797_CONSYS_STP_MAX_FRAME_SIZE];
};

struct mt6797_consys_stp_session {
	struct mutex lock;
	struct mt6797_consys_stp_session_ops ops;
	mt6797_consys_stp_gnss_rx_cb_t gnss_receive;
	void *gnss_receive_context;
	struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_session_bootstrap_lease *bootstrap_lease;
	enum mt6797_consys_stp_session_state state;
	enum mt6797_consys_stp_rx_state rx_state;
	struct mt6797_consys_stp_tx_slot tx[MT6797_CONSYS_STP_SEQUENCE_COUNT];
	u8 txseq;
	u8 txack;
	u8 rxack;
	u8 expected_rxseq;
	u8 credits;
	u8 retries;
	u8 header[MTK_STP_HEADER_SIZE];
	u8 header_length;
	u8 crc[MTK_STP_TRAILER_SIZE];
	u8 crc_length;
	u16 payload_length;
	u16 payload_position;
	u8 channel;
	bool last_rx_ok;
	bool bootstrap_verified;
	u8 resync_count;
	u8 payload[MTK_STP_MAX_PAYLOAD];
	struct mt6797_consys_stp_session_diagnostics diagnostics;
};

struct mt6797_consys_stp_session_lease {
	struct mt6797_consys_stp_session *session;
};

struct mt6797_consys_stp_session_bootstrap_lease {
	struct mt6797_consys_stp_session *session;
};

static const u8 mt6797_consys_stp_resync[] = { 0x7f, 0x7f, 0x7f, 0x7f };

static void mt6797_consys_stp_record_ingress(
		struct mt6797_consys_stp_session *session, const u8 *data, size_t length)
{
	size_t first = min_t(size_t, length, MT6797_CONSYS_STP_EVIDENCE_SIZE);
	size_t tail = min_t(size_t, length, MT6797_CONSYS_STP_EVIDENCE_SIZE);

	session->diagnostics.rx_bytes += length;
	if (!length)
		return;
	session->diagnostics.ingress_prefix_length = first;
	session->diagnostics.ingress_tail_length = tail;
	if (first)
		memcpy(session->diagnostics.ingress_prefix, data, first);
	if (tail)
		memcpy(session->diagnostics.ingress_tail, data + length - tail, tail);
}

static u8 mt6797_consys_stp_sequence_next(u8 sequence)
{
	return (sequence + 1) & 0x7;
}

static bool mt6797_consys_stp_channel_supported(enum mtk_stp_channel channel)
{
	return channel == MTK_STP_BT || channel == MTK_STP_WMT ||
		channel == MTK_STP_GNSS;
}

static u8 mt6797_consys_stp_header_checksum(const u8 header[MTK_STP_HEADER_SIZE])
{
	return header[0] + header[1] + header[2];
}

static bool mt6797_consys_stp_lease_valid(
		const struct mt6797_consys_stp_session_lease *lease)
{
	return lease && lease->session &&
		lease == lease->session->lease &&
		lease->session->state == MT6797_CONSYS_STP_ACTIVE;
}

static bool mt6797_consys_stp_bootstrap_lease_valid(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease)
{
	return lease && lease->session &&
		lease == lease->session->bootstrap_lease &&
		lease->session->state == MT6797_CONSYS_STP_BOOTSTRAP;
}

static bool mt6797_consys_stp_session_full_state(
		const struct mt6797_consys_stp_session *session)
{
	return session->state == MT6797_CONSYS_STP_BOOTSTRAP ||
		session->state == MT6797_CONSYS_STP_ACTIVE;
}

static void mt6797_consys_stp_reset_parser(
		struct mt6797_consys_stp_session *session)
{
	session->rx_state = MT6797_CONSYS_STP_RX_SYNC;
	session->header_length = 0;
	session->crc_length = 0;
	session->payload_length = 0;
	session->payload_position = 0;
	session->resync_count = 0;
}

static void mt6797_consys_stp_reset_protocol(
		struct mt6797_consys_stp_session *session)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(session->tx); index++)
		session->tx[index].valid = false;
	session->txseq = 0;
	session->txack = MT6797_CONSYS_STP_SEQUENCE_COUNT - 1;
	session->rxack = MT6797_CONSYS_STP_SEQUENCE_COUNT - 1;
	session->expected_rxseq = 0;
	session->credits = MT6797_CONSYS_STP_WINDOW_SIZE;
	session->retries = 0;
	session->last_rx_ok = true;
	session->bootstrap_verified = false;
	mt6797_consys_stp_reset_parser(session);
}

static int mt6797_consys_stp_transmit_locked(
		struct mt6797_consys_stp_session *session,
		const u8 *frame, size_t frame_length)
{
	int ret;

	ret = session->ops.transmit(session->ops.transmit_context, frame,
					     frame_length);
	session->diagnostics.tx_frames++;
	if (frame_length == sizeof(mt6797_consys_stp_resync) &&
	    !memcmp(frame, mt6797_consys_stp_resync, sizeof(mt6797_consys_stp_resync)))
		session->diagnostics.tx_resync_frames++;
	if (ret == -EAGAIN) {
		/* A whole-frame transport backpressure result consumed no bytes. */
		return ret;
	}
	if (ret)
		session->state = MT6797_CONSYS_STP_FENCED;
	return ret;
}

static int mt6797_consys_stp_send_ack_locked(
		struct mt6797_consys_stp_session *session, u8 ack)
{
	u8 frame[MTK_STP_HEADER_SIZE];

	/* Vendor stp_send_ack() has static fgEnableNak == 0: no NAK bit. */
	frame[0] = 0x80 | (ack & 0x7);
	frame[1] = 0;
	frame[2] = 0;
	frame[3] = mt6797_consys_stp_header_checksum(frame);

	session->diagnostics.tx_ack_frames++;
	return mt6797_consys_stp_transmit_locked(session, frame, sizeof(frame));
}

static int mt6797_consys_stp_process_ack_locked(
		struct mt6797_consys_stp_session *session, u8 ack)
{
	u8 sequence = mt6797_consys_stp_sequence_next(session->rxack);
	unsigned int released = 0;

	/* The vendor accepts only an ACK between rxack+1 and txseq-1. */
	while (sequence != session->txseq) {
		if (sequence == ack)
			break;
		sequence = mt6797_consys_stp_sequence_next(sequence);
	}
	if (sequence == session->txseq)
		return 0;

	sequence = mt6797_consys_stp_sequence_next(session->rxack);
	for (;;) {
		if (!session->tx[sequence].valid) {
			session->state = MT6797_CONSYS_STP_FENCED;
			return -EPROTO;
		}
		session->tx[sequence].valid = false;
		released++;
		if (sequence == ack)
			break;
		sequence = mt6797_consys_stp_sequence_next(sequence);
	}

	session->rxack = ack;
	session->credits += released;
	if (session->credits > MT6797_CONSYS_STP_WINDOW_SIZE) {
		session->state = MT6797_CONSYS_STP_FENCED;
		return -EPROTO;
	}
	session->retries = 0;
	return 0;
}

static int mt6797_consys_stp_finish_frame_locked(
		struct mt6797_consys_stp_session *session)
{
	u16 crc;
	u8 sequence;
	u8 ack;
	int ret;

	crc = crc16(0, session->payload, session->payload_length);
	if (session->crc[0] != (u8)crc || session->crc[1] != (u8)(crc >> 8)) {
		session->diagnostics.rx_crc_failures++;
		pr_warn_ratelimited("mt6797-consys-stp: full-STP CRC header=%*ph len=%u got=%*ph want=%04x\n",
				    MTK_STP_HEADER_SIZE, session->header,
				    session->payload_length, MTK_STP_TRAILER_SIZE,
				    session->crc, crc);
		return -EBADMSG;
	}

	sequence = (session->header[0] >> 3) & 0x7;
	ack = session->header[0] & 0x7;

	if (sequence == session->expected_rxseq) {
		session->diagnostics.rx_data_frames++;
		if (session->channel == MTK_STP_WMT)
			session->diagnostics.rx_wmt_frames++;
		if (session->channel == MTK_STP_GNSS && session->gnss_receive)
			ret = session->gnss_receive(session->gnss_receive_context,
						    session->payload,
						    session->payload_length);
		else if (mt6797_consys_stp_channel_supported(session->channel))
			ret = session->ops.receive(session->ops.receive_context,
						   session->channel,
						   session->payload,
						   session->payload_length);
		else
			ret = -EOPNOTSUPP;
		if (ret) {
			/*
			 * Vendor stp_process_packet() only advances rxseq/processes the
			 * peer ACK after a successful enqueue (stp_core.c:1356-1366).
			 * Its failure path sends no ACK and waits for a retry
			 * (stp_core.c:1369-1387).  Preserve both properties here.
			 */
			session->last_rx_ok = false;
			return ret;
		}

		ret = mt6797_consys_stp_process_ack_locked(session, ack);
		if (ret)
			return ret;
		session->txack = sequence;
		session->expected_rxseq = mt6797_consys_stp_sequence_next(sequence);
		session->last_rx_ok = true;
		ret = mt6797_consys_stp_send_ack_locked(session, session->txack);
		return ret;
	}

	/* Duplicate or out-of-order data receives the last cumulative ACK. */
	if (session->last_rx_ok)
		return mt6797_consys_stp_send_ack_locked(session, session->txack);

	return -EILSEQ;
}

static int mt6797_consys_stp_process_header_locked(
		struct mt6797_consys_stp_session *session)
{
	u16 length;
	u8 ack;

	if ((session->header[0] & 0xc0) != 0x80 ||
	    session->header[3] != mt6797_consys_stp_header_checksum(session->header)) {
		pr_warn_ratelimited("mt6797-consys-stp: full-STP header=%*ph want-csum=%02x\n",
				    MTK_STP_HEADER_SIZE, session->header,
				    mt6797_consys_stp_header_checksum(session->header));
		return -EBADMSG;
	}
	/* Keep the vendor's effective default: the NAK bit is ignored. */
	session->channel = (session->header[1] >> 4) & 0x7;
	length = ((session->header[1] & 0x0f) << 8) | session->header[2];
	if (length > MTK_STP_MAX_PAYLOAD)
		return -EMSGSIZE;
	if (!length) {
		ack = session->header[0] & 0x7;
		session->diagnostics.rx_ack_frames++;
		return mt6797_consys_stp_process_ack_locked(session, ack);
	}

	session->payload_length = length;
	session->payload_position = 0;
	session->crc_length = 0;
	session->rx_state = MT6797_CONSYS_STP_RX_PAYLOAD;
	return 0;
}

static int mt6797_consys_stp_receive_byte_locked(
		struct mt6797_consys_stp_session *session, u8 byte)
{
	int ret;

	switch (session->rx_state) {
	case MT6797_CONSYS_STP_RX_SYNC:
		if ((byte & 0xc0) == 0x80) {
			session->header[0] = byte;
			session->header_length = 1;
			session->rx_state = MT6797_CONSYS_STP_RX_HEADER;
			return 0;
		}
		if (byte == 0x7f) {
			session->resync_count = 1;
			session->rx_state = MT6797_CONSYS_STP_RX_RESYNC;
			return 0;
		}
		/* Vendor full mode accepts the optional 0x55 delimiter in SYNC. */
		if (byte == 0x55)
			return 0;
		pr_warn_ratelimited("mt6797-consys-stp: RX full-STP sync mismatch byte=%02x\n",
					    byte);
		return -EBADMSG;
	case MT6797_CONSYS_STP_RX_HEADER:
		session->header[session->header_length++] = byte;
		if (session->header_length != MTK_STP_HEADER_SIZE)
			return 0;
		ret = mt6797_consys_stp_process_header_locked(session);
		if (!ret && !session->payload_length)
			mt6797_consys_stp_reset_parser(session);
		return ret;
	case MT6797_CONSYS_STP_RX_PAYLOAD:
		session->payload[session->payload_position++] = byte;
		if (session->payload_position == session->payload_length)
			session->rx_state = MT6797_CONSYS_STP_RX_CRC;
		return 0;
	case MT6797_CONSYS_STP_RX_CRC:
		session->crc[session->crc_length++] = byte;
		if (session->crc_length != MTK_STP_TRAILER_SIZE)
			return 0;
		ret = mt6797_consys_stp_finish_frame_locked(session);
		mt6797_consys_stp_reset_parser(session);
		return ret;
	case MT6797_CONSYS_STP_RX_RESYNC:
		if (byte == 0x7f) {
			session->resync_count++;
			if (session->resync_count == ARRAY_SIZE(mt6797_consys_stp_resync))
				mt6797_consys_stp_reset_parser(session);
			return 0;
		}
		mt6797_consys_stp_reset_parser(session);
		return mt6797_consys_stp_receive_byte_locked(session, byte);
	}

	return -EPROTO;
}

static int mt6797_consys_stp_timeout_locked(
		struct mt6797_consys_stp_session *session)
{
	u8 sequence;
	int ret;

	if (session->credits == MT6797_CONSYS_STP_WINDOW_SIZE)
		return 0;
	session->diagnostics.retry_ticks++;
	if (session->retries > MT6797_CONSYS_STP_RETRY_LIMIT) {
		session->state = MT6797_CONSYS_STP_FENCED;
		return -ETIMEDOUT;
	}

	ret = mt6797_consys_stp_transmit_locked(session,
						 mt6797_consys_stp_resync,
						 sizeof(mt6797_consys_stp_resync));
	if (ret)
		return ret;

	sequence = mt6797_consys_stp_sequence_next(session->rxack);
	while (sequence != session->txseq) {
		if (!session->tx[sequence].valid) {
			session->state = MT6797_CONSYS_STP_FENCED;
			return -EPROTO;
		}
		ret = mt6797_consys_stp_transmit_locked(session,
							 session->tx[sequence].frame,
							 session->tx[sequence].frame_length);
		if (ret)
			return ret;
		sequence = mt6797_consys_stp_sequence_next(sequence);
	}

	session->retries++;
	if (session->retries > MT6797_CONSYS_STP_RETRY_LIMIT) {
		session->state = MT6797_CONSYS_STP_FENCED;
		return -ETIMEDOUT;
	}

	return 0;
}

struct mt6797_consys_stp_session *
mt6797_consys_stp_session_create(const struct mt6797_consys_stp_session_ops *ops)
{
	struct mt6797_consys_stp_session *session;
	struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_session_bootstrap_lease *bootstrap_lease;

	if (!ops || !ops->transmit || !ops->receive)
		return ERR_PTR(-EINVAL);

	session = kzalloc_obj(*session, GFP_KERNEL);
	if (!session)
		return ERR_PTR(-ENOMEM);
	lease = kzalloc_obj(*lease, GFP_KERNEL);
	if (!lease) {
		kfree(session);
		return ERR_PTR(-ENOMEM);
	}
	bootstrap_lease = kzalloc_obj(*bootstrap_lease, GFP_KERNEL);
	if (!bootstrap_lease) {
		kfree(lease);
		kfree(session);
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&session->lock);
	session->ops = *ops;
	session->lease = lease;
	session->bootstrap_lease = bootstrap_lease;
	lease->session = session;
	bootstrap_lease->session = session;
	mt6797_consys_stp_reset_protocol(session);
	return session;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_create);

const struct mt6797_consys_stp_session_lease *
mt6797_consys_stp_session_begin_full(struct mt6797_consys_stp_session *session)
{
	const struct mt6797_consys_stp_session_lease *lease;

	if (!session)
		return ERR_PTR(-EINVAL);

	mutex_lock(&session->lock);
	if (session->state == MT6797_CONSYS_STP_FENCED)
		lease = ERR_PTR(-EPIPE);
	else if (session->state != MT6797_CONSYS_STP_NEW)
		lease = ERR_PTR(-EBUSY);
	else {
		mt6797_consys_stp_reset_protocol(session);
		session->state = MT6797_CONSYS_STP_ACTIVE;
		lease = session->lease;
	}
	mutex_unlock(&session->lock);

	return lease;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_begin_full);

const struct mt6797_consys_stp_session_bootstrap_lease *
mt6797_consys_stp_session_begin_full_bootstrap(
					struct mt6797_consys_stp_session *session)
{
	const struct mt6797_consys_stp_session_bootstrap_lease *lease;

	if (!session)
		return ERR_PTR(-EINVAL);

	mutex_lock(&session->lock);
	if (session->state == MT6797_CONSYS_STP_FENCED)
		lease = ERR_PTR(-EPIPE);
	else if (session->state != MT6797_CONSYS_STP_NEW)
		lease = ERR_PTR(-EBUSY);
	else {
		mt6797_consys_stp_reset_protocol(session);
		session->state = MT6797_CONSYS_STP_BOOTSTRAP;
		lease = session->bootstrap_lease;
	}
	mutex_unlock(&session->lock);

	return lease;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_begin_full_bootstrap);

bool mt6797_consys_stp_session_lease_active(
			const struct mt6797_consys_stp_session_lease *lease)
{
	struct mt6797_consys_stp_session *session;
	bool active;

	if (!lease || !lease->session)
		return false;
	session = lease->session;
	mutex_lock(&session->lock);
	active = mt6797_consys_stp_lease_valid(lease);
	mutex_unlock(&session->lock);
	return active;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_lease_active);

int mt6797_consys_stp_session_bind_gnss(
		const struct mt6797_consys_stp_session_lease *lease,
		mt6797_consys_stp_gnss_rx_cb_t receive, void *receive_context)
{
	struct mt6797_consys_stp_session *session;
	int ret = 0;

	if (!lease || !lease->session || !receive)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_lease_valid(lease))
		ret = -ESHUTDOWN;
	else if (session->gnss_receive)
		ret = -EBUSY;
	else {
		session->gnss_receive = receive;
		session->gnss_receive_context = receive_context;
	}
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bind_gnss);

int mt6797_consys_stp_session_unbind_gnss(
		const struct mt6797_consys_stp_session_lease *lease,
		mt6797_consys_stp_gnss_rx_cb_t receive, void *receive_context)
{
	struct mt6797_consys_stp_session *session;
	int ret = 0;

	if (!lease || !lease->session || !receive)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_lease_valid(lease))
		ret = -ESHUTDOWN;
	else if (session->gnss_receive != receive ||
		 session->gnss_receive_context != receive_context)
		ret = -ENOENT;
	else {
		session->gnss_receive = NULL;
		session->gnss_receive_context = NULL;
	}
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_unbind_gnss);

static int mt6797_consys_stp_session_transmit_locked(
		struct mt6797_consys_stp_session *session,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length)
{
	struct mt6797_consys_stp_tx_slot *slot;
	u16 crc;
	int ret;

	if (!session->credits) {
		ret = -EAGAIN;
		goto out;
	}

	slot = &session->tx[session->txseq];
	if (slot->valid) {
		session->state = MT6797_CONSYS_STP_FENCED;
		ret = -EPROTO;
		goto out;
	}

	slot->frame[0] = 0x80 | ((session->txseq & 0x7) << 3) |
		(session->txack & 0x7);
	slot->frame[1] = (channel << 4) | ((payload_length >> 8) & 0x0f);
	slot->frame[2] = payload_length;
	slot->frame[3] = mt6797_consys_stp_header_checksum(slot->frame);
	memcpy(slot->frame + MTK_STP_HEADER_SIZE, payload, payload_length);
	crc = crc16(0, payload, payload_length);
	slot->frame[MTK_STP_HEADER_SIZE + payload_length] = crc;
	slot->frame[MTK_STP_HEADER_SIZE + payload_length + 1] = crc >> 8;
	slot->frame_length = MTK_STP_HEADER_SIZE + payload_length +
		MTK_STP_TRAILER_SIZE;
	slot->valid = true;
	session->txseq = mt6797_consys_stp_sequence_next(session->txseq);
	session->credits--;

	ret = mt6797_consys_stp_transmit_locked(session, slot->frame,
							  slot->frame_length);
	if (ret == -EAGAIN) {
		slot->valid = false;
		session->txseq = (session->txseq + MT6797_CONSYS_STP_SEQUENCE_COUNT - 1) &
			(MT6797_CONSYS_STP_SEQUENCE_COUNT - 1);
		session->credits++;
	}
out:
	return ret;
}

int mt6797_consys_stp_session_transmit(
		const struct mt6797_consys_stp_session_lease *lease,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session || (!payload && payload_length) ||
	    !payload_length)
		return -EINVAL;
	/* Channel zero is never published to a normal full-STP client. */
	if (channel == MTK_STP_BT)
		return -EOPNOTSUPP;
	if (!mt6797_consys_stp_channel_supported(channel))
		return -EOPNOTSUPP;
	if (payload_length > MTK_STP_MAX_PAYLOAD)
		return -EMSGSIZE;

	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_session_transmit_locked(session, channel,
								 payload, payload_length);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_transmit);

int mt6797_consys_stp_session_bootstrap_transmit(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session || (!payload && payload_length) ||
	    !payload_length)
		return -EINVAL;
	if (!mt6797_consys_stp_channel_supported(channel))
		return -EOPNOTSUPP;
	if (payload_length > MTK_STP_MAX_PAYLOAD)
		return -EMSGSIZE;

	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_bootstrap_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_session_transmit_locked(session, channel,
								 payload, payload_length);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_transmit);

static int mt6797_consys_stp_session_receive_locked(
			struct mt6797_consys_stp_session *session,
			const u8 *data, size_t length)
{
	int first_error = 0;
	size_t index;
	int ret;

	for (index = 0; index < length; index++) {
		ret = mt6797_consys_stp_receive_byte_locked(session, data[index]);
		/*
		 * Vendor btif_rx_irq_handler() invokes a void receive callback
		 * (btif_plat.c:816-859), while stp_core.c:2069-2074 leaves an
		 * unexpected byte in SYNC for a later frame.  Keep that local,
		 * recoverable framing fault out of the PIO fence path; endpoint and
		 * protocol failures remain terminal to this dispatch.
		 */
		if (ret && ret != -EBADMSG && !first_error)
			first_error = ret;
		if (ret) {
			session->diagnostics.rx_parser_failures++;
			mt6797_consys_stp_reset_parser(session);
		}
		if (!mt6797_consys_stp_session_full_state(session)) {
			if (!first_error)
				first_error = -ESHUTDOWN;
			break;
		}
	}
	return first_error;
}

int mt6797_consys_stp_session_receive(struct mt6797_consys_stp_session *session,
						      const u8 *data, size_t length)
{
	int ret;

	if (!session || (!data && length))
		return -EINVAL;

	mutex_lock(&session->lock);
	mt6797_consys_stp_record_ingress(session, data, length);
	if (session->state != MT6797_CONSYS_STP_ACTIVE)
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_session_receive_locked(session, data, length);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_receive);

int mt6797_consys_stp_session_receive_lease(
			const struct mt6797_consys_stp_session_lease *lease,
			const u8 *data, size_t length)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session || (!data && length))
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	mt6797_consys_stp_record_ingress(session, data, length);
	if (!mt6797_consys_stp_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_session_receive_locked(session, data, length);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_receive_lease);

int mt6797_consys_stp_session_bootstrap_receive(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease,
		const u8 *data, size_t length)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session || (!data && length))
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	mt6797_consys_stp_record_ingress(session, data, length);
	if (!mt6797_consys_stp_bootstrap_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_session_receive_locked(session, data, length);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_receive);

int mt6797_consys_stp_session_bootstrap_timeout(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_bootstrap_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_timeout_locked(session);
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_timeout);

int mt6797_consys_stp_session_bootstrap_diagnostics_snapshot(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease,
		struct mt6797_consys_stp_session_diagnostics *diagnostics)
{
	struct mt6797_consys_stp_session *session;

	if (!lease || !lease->session || !diagnostics)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	*diagnostics = session->diagnostics;
	diagnostics->txseq = session->txseq;
	diagnostics->txack = session->txack;
	diagnostics->rxack = session->rxack;
	diagnostics->expected_rxseq = session->expected_rxseq;
	diagnostics->credits = session->credits;
	diagnostics->retries = session->retries;
	diagnostics->rx_state = session->rx_state;
	diagnostics->channel = session->channel;
	diagnostics->payload_length = session->payload_length;
	diagnostics->payload_position = session->payload_position;
	memcpy(diagnostics->last_header, session->header, sizeof(session->header));
	memcpy(diagnostics->last_crc, session->crc, sizeof(session->crc));
	mutex_unlock(&session->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_diagnostics_snapshot);

int mt6797_consys_stp_session_bootstrap_confirm(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease)
{
	struct mt6797_consys_stp_session *session;
	int ret = 0;

	if (!lease || !lease->session)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_bootstrap_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		session->bootstrap_verified = true;
	mutex_unlock(&session->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_confirm);

const struct mt6797_consys_stp_session_lease *
mt6797_consys_stp_session_bootstrap_promote(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease)
{
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_lease *active;

	if (!lease || !lease->session)
		return ERR_PTR(-EINVAL);
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_bootstrap_lease_valid(lease)) {
		active = ERR_PTR(-ESHUTDOWN);
	} else if (!session->bootstrap_verified) {
		active = ERR_PTR(-EACCES);
	} else {
		/* Keep txseq/txack/rxack and every retained frame intact. */
		session->state = MT6797_CONSYS_STP_ACTIVE;
		active = session->lease;
	}
	mutex_unlock(&session->lock);
	return active;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_bootstrap_promote);

int mt6797_consys_stp_session_timeout(
		const struct mt6797_consys_stp_session_lease *lease)
{
	struct mt6797_consys_stp_session *session;
	int ret;

	if (!lease || !lease->session)
		return -EINVAL;
	session = lease->session;
	mutex_lock(&session->lock);
	if (!mt6797_consys_stp_lease_valid(lease))
		ret = -ESHUTDOWN;
	else
		ret = mt6797_consys_stp_timeout_locked(session);
	mutex_unlock(&session->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_timeout);

void mt6797_consys_stp_session_quiesce(struct mt6797_consys_stp_session *session)
{
	if (!session)
		return;

	mutex_lock(&session->lock);
	session->state = MT6797_CONSYS_STP_FENCED;
	mt6797_consys_stp_reset_parser(session);
	mutex_unlock(&session->lock);
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_quiesce);

void mt6797_consys_stp_session_destroy(struct mt6797_consys_stp_session *session)
{
	if (!session || IS_ERR(session))
		return;

	mt6797_consys_stp_session_quiesce(session);
	kfree(session->bootstrap_lease);
	kfree(session->lease);
	kfree(session);
}
EXPORT_SYMBOL_GPL(mt6797_consys_stp_session_destroy);

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_STP_SESSION_KUNIT_TEST)

#define MT6797_CONSYS_STP_TEST_MAX_TX	32

struct mt6797_consys_stp_test_link {
	u8 frames[MT6797_CONSYS_STP_TEST_MAX_TX][MT6797_CONSYS_STP_MAX_FRAME_SIZE];
	size_t lengths[MT6797_CONSYS_STP_TEST_MAX_TX];
	unsigned int tx_count;
	int transmit_error;
	unsigned int rx_count;
	enum mtk_stp_channel last_channel;
	u8 last_payload[MTK_STP_MAX_PAYLOAD];
	size_t last_payload_length;
	int receive_error;
};

static int mt6797_consys_stp_test_transmit(void *context, const u8 *frame,
						    size_t frame_length)
{
	struct mt6797_consys_stp_test_link *link = context;

	if (link->tx_count == MT6797_CONSYS_STP_TEST_MAX_TX ||
	    frame_length > MT6797_CONSYS_STP_MAX_FRAME_SIZE)
		return -ENOSPC;
	if (link->transmit_error)
		return link->transmit_error;
	memcpy(link->frames[link->tx_count], frame, frame_length);
	link->lengths[link->tx_count++] = frame_length;
	return 0;
}

static int mt6797_consys_stp_test_receive(void *context,
					       enum mtk_stp_channel channel,
					       const u8 *payload, size_t payload_length)
{
	struct mt6797_consys_stp_test_link *link = context;

	link->rx_count++;
	link->last_channel = channel;
	link->last_payload_length = payload_length;
	memcpy(link->last_payload, payload, payload_length);
	return link->receive_error;
}

static int mt6797_consys_stp_test_receive_gnss(void *context,
						 const u8 *payload, size_t payload_length)
{
	return mt6797_consys_stp_test_receive(context, MTK_STP_GNSS, payload,
						    payload_length);
}

static struct mt6797_consys_stp_session *
mt6797_consys_stp_test_session(struct kunit *test,
				      struct mt6797_consys_stp_test_link **link_out,
				      const struct mt6797_consys_stp_session_lease **lease_out)
{
	struct mt6797_consys_stp_session_ops ops;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;

	link = kunit_kzalloc(test, sizeof(*link), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, link);
	ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_consys_stp_test_transmit,
		.receive = mt6797_consys_stp_test_receive,
		.transmit_context = link,
		.receive_context = link,
	};
	session = mt6797_consys_stp_session_create(&ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(session));
	*lease_out = mt6797_consys_stp_session_begin_full(session);
	KUNIT_ASSERT_FALSE(test, IS_ERR(*lease_out));
	*link_out = link;
	return session;
}

static size_t mt6797_consys_stp_test_data_frame(u8 *frame,
						 enum mtk_stp_channel channel,
						 u8 sequence, u8 ack,
						 const u8 *payload, size_t payload_length)
{
	return ({
		size_t frame_length;
		int ret = mtk_stp_encode(MTK_STP_MODE_FULL, channel, sequence, ack,
						payload, payload_length, frame,
						MT6797_CONSYS_STP_MAX_FRAME_SIZE,
						&frame_length);
		WARN_ON_ONCE(ret);
		frame_length;
	});
}

static void mt6797_consys_stp_test_wraparound(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 ack[] = { 0x86, 0x00, 0x00, 0x86 };
	u8 payload = 0x5a;
	unsigned int index;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	for (index = 0; index < MT6797_CONSYS_STP_WINDOW_SIZE; index++)
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	KUNIT_ASSERT_EQ(test, link->frames[0][0], (u8)0x87);
	KUNIT_ASSERT_EQ(test, link->frames[6][0], (u8)0xb7);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, ack,
							 sizeof(ack)), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	KUNIT_EXPECT_EQ(test, link->frames[7][0], (u8)0xbf);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_credit_exhaustion(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 ack[] = { 0x80, 0x00, 0x00, 0x80 };
	u8 payload = 0;
	unsigned int index;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	for (index = 0; index < MT6797_CONSYS_STP_WINDOW_SIZE; index++)
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), -EAGAIN);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, ack,
							 sizeof(ack)), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_backpressure_releases_frame_slot(
								     struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 payload = 0x5a;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	link->transmit_error = -EAGAIN;
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
									 MTK_STP_WMT, &payload,
									 sizeof(payload)), -EAGAIN);
	KUNIT_EXPECT_EQ(test, link->tx_count, 0U);
	link->transmit_error = 0;
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
									 MTK_STP_WMT, &payload,
									 sizeof(payload)), 0);
	KUNIT_EXPECT_EQ(test, link->frames[0][0], (u8)0x87);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_cumulative_ack(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 ack[] = { 0x82, 0x00, 0x00, 0x82 };
	u8 payload = 0;
	unsigned int index;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	for (index = 0; index < 3; index++)
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, ack,
							 sizeof(ack)), 0);
	for (index = 0; index < MT6797_CONSYS_STP_WINDOW_SIZE; index++)
		KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_duplicate_and_out_of_order(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 *frame;
	u8 payload = 0x37;
	size_t frame_length;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 7,
								&payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
							 frame_length), 0);
	KUNIT_ASSERT_EQ(test, link->rx_count, 1U);
	KUNIT_ASSERT_EQ(test, link->frames[0][0], (u8)0x80);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
							 frame_length), 0);
	KUNIT_ASSERT_EQ(test, link->rx_count, 1U);
	KUNIT_ASSERT_EQ(test, link->frames[1][0], (u8)0x80);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 2, 7,
								&payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
							 frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->frames[2][0], (u8)0x80);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_receive_failure_retries_without_ack(
									struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 *frame;
	u8 payload = 0x37;
	size_t frame_length;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 7,
									&payload, sizeof(payload));

	link->receive_error = -ENOSPC;
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
								 frame_length), -ENOSPC);
	KUNIT_EXPECT_EQ(test, link->rx_count, 1U);
	KUNIT_EXPECT_EQ(test, link->tx_count, 0U);

	link->receive_error = 0;
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
								 frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 2U);
	KUNIT_EXPECT_EQ(test, link->tx_count, 1U);
	KUNIT_EXPECT_EQ(test, link->frames[0][0], (u8)0x80);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_nak_and_timeout_retry(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 nak_ack[] = { 0x80, 0x80, 0x00, 0x00 };
	u8 payload = 0xa5;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	/* Vendor default ignores the NAK bit but still consumes the ACK number. */
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, nak_ack,
							 sizeof(nak_ack)), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_timeout(lease), 0);
	KUNIT_EXPECT_EQ(test, link->tx_count, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_timeout(lease), 0);
	KUNIT_EXPECT_EQ(test, link->lengths[2], sizeof(mt6797_consys_stp_resync));
	KUNIT_EXPECT_MEMEQ(test, link->frames[2], mt6797_consys_stp_resync,
				 sizeof(mt6797_consys_stp_resync));
	KUNIT_EXPECT_MEMEQ(test, link->frames[3], link->frames[1], link->lengths[1]);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_retry_limit_fences(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 payload = 0;
	unsigned int index;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), 0);
	for (index = 0; index < MT6797_CONSYS_STP_RETRY_LIMIT; index++)
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_timeout(lease), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_timeout(lease), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), -ESHUTDOWN);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_malformed_and_quiesce(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 malformed[] = { 0x80, 0x40, 0x00, 0x00 };
	u8 payload = 0;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, malformed,
								 sizeof(malformed)), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 0U);
	mt6797_consys_stp_session_quiesce(session);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
					 MTK_STP_WMT, &payload, sizeof(payload)), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, &payload,
							 sizeof(payload)), -ESHUTDOWN);
	KUNIT_EXPECT_TRUE(test, IS_ERR(mt6797_consys_stp_session_begin_full(session)));
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_fragmented_delimiter_resync_and_crc_recovery(
										  struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 *frame;
	u8 *bytes;
	u8 delimiter = 0x55;
	u8 resync[] = { 0x7f, 0x7f, 0x7f, 0x7f };
	u8 payload = 0x37;
	size_t frame_length;
	size_t split;

	/* Every data-frame boundary is a valid BTIF receive fragmentation point. */
	for (split = 0; split <= MTK_STP_HEADER_SIZE + sizeof(payload) +
		MTK_STP_TRAILER_SIZE; split++) {
		session = mt6797_consys_stp_test_session(test, &link, &lease);
		frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, frame);
		frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 7,
									&payload, sizeof(payload));
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
									split), 0);
		KUNIT_EXPECT_EQ(test, link->rx_count, split == frame_length ? 1U : 0U);
		KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session,
									frame + split,
									frame_length - split), 0);
		KUNIT_EXPECT_EQ(test, link->rx_count, 1U);
		mt6797_consys_stp_session_destroy(session);
	}

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	bytes = kunit_kmalloc(test, 2 * MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bytes);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 7,
									&payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, &delimiter, 1), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, resync, 2), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, resync + 2, 2), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
									frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 1U);

	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 1, 7,
									&payload, sizeof(payload));
	memcpy(bytes, frame, frame_length);
	bytes[frame_length - 1] ^= 1;
	memcpy(bytes + frame_length, frame, frame_length);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, bytes,
									2 * frame_length), 0);
	/* The CRC-rejected seq=1 did not advance expected_rxseq. */
	KUNIT_EXPECT_EQ(test, link->rx_count, 2U);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_receive_sequence_wraps(struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 *frame;
	u8 payload = 0x91;
	size_t frame_length;
	u8 sequence;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	for (sequence = 0; sequence < MT6797_CONSYS_STP_SEQUENCE_COUNT; sequence++) {
		frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT,
									sequence, 7, &payload,
									sizeof(payload));
		KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
									frame_length), 0);
	}
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 7,
									&payload, sizeof(payload));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
									frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 9U);
	KUNIT_EXPECT_EQ(test, link->tx_count, 9U);
	KUNIT_EXPECT_EQ(test, link->frames[8][0], (u8)0x80);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_bootstrap_promotes_without_reset(
								 struct kunit *test)
{
	const struct mt6797_consys_stp_session_bootstrap_lease *bootstrap;
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	struct mt6797_consys_stp_session_ops ops;
	u8 *frame;
	u8 payload = 0x5a;
	size_t frame_length;

	link = kunit_kzalloc(test, sizeof(*link), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, link);
	ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_consys_stp_test_transmit,
		.receive = mt6797_consys_stp_test_receive,
		.transmit_context = link,
		.receive_context = link,
	};
	session = mt6797_consys_stp_session_create(&ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(session));
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	bootstrap = mt6797_consys_stp_session_begin_full_bootstrap(session);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	KUNIT_EXPECT_TRUE(test, IS_ERR(mt6797_consys_stp_session_begin_full(session)));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_bootstrap_transmit(bootstrap,
										 MTK_STP_WMT,
										 &payload, sizeof(payload)), 0);
	KUNIT_ASSERT_EQ(test, link->frames[0][0], (u8)0x87);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_bootstrap_timeout(bootstrap),
				0);
	KUNIT_EXPECT_EQ(test, link->lengths[1],
				sizeof(mt6797_consys_stp_resync));
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_WMT, 0, 0,
									 &payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_bootstrap_receive(bootstrap,
										frame, frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->frames[3][0], (u8)0x80);
	lease = mt6797_consys_stp_session_bootstrap_promote(bootstrap);
	KUNIT_EXPECT_TRUE(test, IS_ERR(lease));
	KUNIT_EXPECT_EQ(test, PTR_ERR(lease), -EACCES);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_bootstrap_confirm(bootstrap),
				0);
	lease = mt6797_consys_stp_session_bootstrap_promote(bootstrap);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	KUNIT_EXPECT_TRUE(test, mt6797_consys_stp_session_lease_active(lease));
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(lease,
							 MTK_STP_WMT,
							 &payload, sizeof(payload)), 0);
	/* Initial submit, resync marker, retry, received-data ACK, then seq 1. */
	KUNIT_EXPECT_EQ(test, link->tx_count, 5U);
	KUNIT_EXPECT_EQ(test, link->frames[4][0], (u8)0x88);
	KUNIT_EXPECT_TRUE(test,
			 IS_ERR(mt6797_consys_stp_session_bootstrap_promote(bootstrap)));
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_gnss_binding_is_lease_owned(
								  struct kunit *test)
{
	const struct mt6797_consys_stp_session_lease *lease;
	struct mt6797_consys_stp_test_link *link;
	struct mt6797_consys_stp_session *session;
	u8 *frame;
	u8 payload = 0x5a;
	size_t frame_length;

	session = mt6797_consys_stp_test_session(test, &link, &lease);
	frame = kunit_kmalloc(test, MT6797_CONSYS_STP_MAX_FRAME_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_bind_gnss(lease,
									  mt6797_consys_stp_test_receive_gnss,
									  link), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_bind_gnss(lease,
									   mt6797_consys_stp_test_receive_gnss,
									   link), -EBUSY);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_GNSS, 0, 7,
									 &payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
										frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 1U);
	KUNIT_EXPECT_EQ(test, link->last_channel, MTK_STP_GNSS);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_unbind_gnss(lease,
									    mt6797_consys_stp_test_receive_gnss,
									    session), -ENOENT);
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_unbind_gnss(lease,
									    mt6797_consys_stp_test_receive_gnss,
									    link), 0);
	frame_length = mt6797_consys_stp_test_data_frame(frame, MTK_STP_GNSS, 1, 7,
									 &payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test, mt6797_consys_stp_session_receive(session, frame,
										frame_length), 0);
	KUNIT_EXPECT_EQ(test, link->rx_count, 2U);
	mt6797_consys_stp_session_destroy(session);
}

static void mt6797_consys_stp_test_null_session_lease_is_rejected(
							   struct kunit *test)
{
	struct mt6797_consys_stp_session_lease invalid = {};
	u8 payload = 0;

	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_transmit(&invalid,
								 MTK_STP_WMT,
								 &payload, sizeof(payload)), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_consys_stp_session_timeout(&invalid), -EINVAL);
}

static struct kunit_case mt6797_consys_stp_session_test_cases[] = {
	KUNIT_CASE(mt6797_consys_stp_test_wraparound),
	KUNIT_CASE(mt6797_consys_stp_test_credit_exhaustion),
	KUNIT_CASE(mt6797_consys_stp_test_backpressure_releases_frame_slot),
	KUNIT_CASE(mt6797_consys_stp_test_cumulative_ack),
	KUNIT_CASE(mt6797_consys_stp_test_duplicate_and_out_of_order),
	KUNIT_CASE(mt6797_consys_stp_test_receive_failure_retries_without_ack),
	KUNIT_CASE(mt6797_consys_stp_test_nak_and_timeout_retry),
	KUNIT_CASE(mt6797_consys_stp_test_retry_limit_fences),
	KUNIT_CASE(mt6797_consys_stp_test_malformed_and_quiesce),
	KUNIT_CASE(mt6797_consys_stp_test_fragmented_delimiter_resync_and_crc_recovery),
	KUNIT_CASE(mt6797_consys_stp_test_receive_sequence_wraps),
	KUNIT_CASE(mt6797_consys_stp_test_bootstrap_promotes_without_reset),
	KUNIT_CASE(mt6797_consys_stp_test_gnss_binding_is_lease_owned),
	KUNIT_CASE(mt6797_consys_stp_test_null_session_lease_is_rejected),
	{ }
};

static struct kunit_suite mt6797_consys_stp_session_test_suite = {
	.name = "mt6797-consys-stp-session",
	.test_cases = mt6797_consys_stp_session_test_cases,
};

kunit_test_suite(mt6797_consys_stp_session_test_suite);
#endif

MODULE_DESCRIPTION("Transport-neutral MT6797 CONSYS full-STP session owner");
MODULE_LICENSE("GPL");
