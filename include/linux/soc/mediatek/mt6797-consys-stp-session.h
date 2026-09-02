/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS full-STP session owner.
 *
 * This is a deliberately transport-neutral protocol owner.  It owns no
 * Device Tree node, power domain, register range, clock, reset, DMA channel,
 * firmware object, or radio function.  A future BTIF/CONSYS owner supplies a
 * byte-frame callback only after it has performed the separately reviewed
 * mandatory-to-full WMT handover.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_STP_SESSION_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_STP_SESSION_H

#include <linux/types.h>

#include <linux/soc/mediatek/mtk-stp.h>

/* MTKSTP_TX_TIMEOUT and MTKSTP_RETRY_LIMIT in the MT6797 vendor tree. */
#define MT6797_CONSYS_STP_TX_TIMEOUT_MS	180
#define MT6797_CONSYS_STP_RETRY_LIMIT		10

struct mt6797_consys_stp_session;
struct mt6797_consys_stp_session_lease;
struct mt6797_consys_stp_session_bootstrap_lease;

/*
 * transmit() receives exactly one complete full-STP frame or the four-byte
 * resynchronisation pattern.  It must either consume the entire supplied
 * buffer and return zero, or return an error; partial progress is forbidden.
 * It and receive() run in process context and must not re-enter a session API.
 */
typedef int (*mt6797_consys_stp_tx_cb_t)(void *context, const u8 *frame,
						 size_t frame_length);
typedef int (*mt6797_consys_stp_rx_cb_t)(void *context,
						 enum mtk_stp_channel channel,
						 const u8 *payload,
						 size_t payload_length);
typedef int (*mt6797_consys_stp_gnss_rx_cb_t)(void *context,
						      const u8 *payload,
						      size_t payload_length);

struct mt6797_consys_stp_session_ops {
	mt6797_consys_stp_tx_cb_t transmit;
	mt6797_consys_stp_rx_cb_t receive;
	void *transmit_context;
	void *receive_context;
};

/*
 * Allocate one single-use full-STP owner.  It is initially inert.  The caller
 * retains the lifetime of both callback contexts and must serialize all public
 * calls. Separate contexts let a transport lease own TX while an endpoint owns
 * delivery without exposing either capability to the other.
 */
struct mt6797_consys_stp_session *
mt6797_consys_stp_session_create(const struct mt6797_consys_stp_session_ops *ops);

/*
 * Commit the host-side full-STP state and return the capability required for
 * transmission.  This is valid exactly once, after the owner has independently
 * verified the vendor mandatory-to-full handover.  An enum value such as
 * MTK_STP_MODE_FULL cannot substitute for this capability.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_consys_stp_session_begin_full(struct mt6797_consys_stp_session *session);

/*
 * Start the provisional full-STP half of the vendor BTIF handover.
 * This exists solely for the exact mandatory -> full WMT transition in
 * wmt_ic_soc.c:1008-1044: after the mandatory set reply, the host switches to
 * full STP, waits 10 ms, and verifies the controller's full-mode reply.  It
 * is not an active full-session capability.  In particular, it cannot be
 * passed to a normal endpoint or PIO attachment.
 */
const struct mt6797_consys_stp_session_bootstrap_lease *
mt6797_consys_stp_session_begin_full_bootstrap(
					struct mt6797_consys_stp_session *session);

/* Send/receive through a provisional full-STP capability during verification. */
int mt6797_consys_stp_session_bootstrap_transmit(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length);
int mt6797_consys_stp_session_bootstrap_receive(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease,
		const u8 *data, size_t length);
/* Run the same 180-ms retained-frame timeout/retry semantics while provisional. */
int mt6797_consys_stp_session_bootstrap_timeout(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease);

/*
 * Record that the bootstrap controller's strict WMT transaction matcher
 * accepted the final full-mode query reply.  Promotion is rejected until this
 * marker is set; callers must not set it from an unsolicited event path.
 */
int mt6797_consys_stp_session_bootstrap_confirm(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease);

/*
 * Irreversibly expose the normal full-STP lease after the bootstrap owner has
 * matched the exact final full-mode WMT query reply.  This preserves the
 * existing sequence/window state; it must never reset the protocol.
 */
const struct mt6797_consys_stp_session_lease *
mt6797_consys_stp_session_bootstrap_promote(
		const struct mt6797_consys_stp_session_bootstrap_lease *lease);

/*
 * Validate an opaque full-STP capability without exposing its owning session.
 * The caller retains the lease/session lifetime and serializes teardown with
 * users of the capability.
 */
bool mt6797_consys_stp_session_lease_active(
			const struct mt6797_consys_stp_session_lease *lease);

/*
 * Bind one GNSS endpoint to an already-active full-STP owner. The GNSS client
 * never receives a raw transport or generic STP codec: only this opaque lease
 * can submit its channel-2 payloads and receive channel-2 frames. Unbind
 * requires the same callback/context pair so another endpoint cannot detach it.
 */
int mt6797_consys_stp_session_bind_gnss(
		const struct mt6797_consys_stp_session_lease *lease,
		mt6797_consys_stp_gnss_rx_cb_t receive, void *receive_context);
int mt6797_consys_stp_session_unbind_gnss(
		const struct mt6797_consys_stp_session_lease *lease,
		mt6797_consys_stp_gnss_rx_cb_t receive, void *receive_context);

/* Send one WMT or GNSS payload through an active full-STP capability. */
int mt6797_consys_stp_session_transmit(
		const struct mt6797_consys_stp_session_lease *lease,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length);

/* Feed arbitrary full-STP byte fragments from the owning transport. */
int mt6797_consys_stp_session_receive(struct mt6797_consys_stp_session *session,
						      const u8 *data, size_t length);

/* Feed RX only through an active full-STP capability. */
int mt6797_consys_stp_session_receive_lease(
			const struct mt6797_consys_stp_session_lease *lease,
			const u8 *data, size_t length);

/*
 * Run one vendor 180-ms timeout tick.  A future owner schedules this only
 * after MT6797_CONSYS_STP_TX_TIMEOUT_MS without a cumulative ACK.  It emits
 * the documented resync pattern and retransmits retained frames.  On retry
 * exhaustion or a transmit error it permanently fails this session.
 */
int mt6797_consys_stp_session_timeout(
		const struct mt6797_consys_stp_session_lease *lease);

/*
 * Permanently stop this session and invalidate its transmission capability.
 * There is intentionally no re-arm operation: after real transport/controller
 * recovery, a future owner creates a new session before calling mtk_wmt_reset.
 */
void mt6797_consys_stp_session_quiesce(struct mt6797_consys_stp_session *session);
void mt6797_consys_stp_session_destroy(struct mt6797_consys_stp_session *session);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_STP_SESSION_H */
