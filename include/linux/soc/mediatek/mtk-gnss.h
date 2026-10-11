/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek CONSYS GNSS client for the STP multiplexer.
 *
 * A transport owner supplies an active opaque full-STP session and exact
 * controller power operation. The MT6797 platform binding borrows the canonical
 * CONSYS owner's current session and WMT function-control operation.
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_GNSS_H
#define __LINUX_SOC_MEDIATEK_MTK_GNSS_H

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

struct device;
struct gnss_device;
struct mt6797_consys_stp_session_lease;

/*
 * set_power() performs the transport owner's exact, separately reviewed WMT
 * GPS on/off controller operation. It does not imply board-level GPS_SYNC,
 * LNA, VCN28 PALDO, co-clock, or RF safety; vendor wmt_func.c:405-606 keeps
 * those controls outside the six-byte WMT function command. The callback runs
 * without the client lock and must return before another GNSS transition.
 */
struct mtk_gnss_transport {
	int (*set_power)(void *context, bool on);
	/* The shared owner manages whole-frame backpressure and retry timers. */
	int (*transmit)(void *context, const u8 *data, size_t length);
	void *context;
};

struct mtk_gnss {
	/* Serializes session state, callbacks, and the GNSS raw FIFO handoff. */
	struct mutex lock;
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct gnss_device *gdev;
	struct mtk_gnss_transport transport;
	/* Waited on before a conflicting power or transmit callback is issued. */
	wait_queue_head_t transition_queue;
	bool initialized;
	bool transitioning;
	bool unregistering;
	bool registered;
	bool active;
	bool faulted;
};

/*
 * Register an ordinary /dev/gnssN endpoint on channel 2 of an active full-STP
 * session. gnss must be zero-initialized before its first registration. This
 * function binds no hardware and does not power GPS on. Its opaque session
 * lease provides sequencing/window/ACK ownership and must outlive GNSS until
 * unregister has detached its receiver.
 */
int mtk_gnss_register(struct mtk_gnss *gnss, struct device *parent,
		      const struct mt6797_consys_stp_session_lease *session_lease,
		      const struct mtk_gnss_transport *transport);
void mtk_gnss_unregister(struct mtk_gnss *gnss);

/*
 * These are the GNSS endpoint session operations.  start() calls the supplied
 * controller-specific power callback; stop() makes a failed power-off sticky
 * so that a transport recovery is required before reopening. Calls made on a
 * zero-initialized endpoint before registration are rejected or are no-ops,
 * which lets common error-unwind paths call them unconditionally.
 */
int mtk_gnss_start(struct mtk_gnss *gnss);
void mtk_gnss_stop(struct mtk_gnss *gnss);
int mtk_gnss_write(struct mtk_gnss *gnss, const u8 *data, size_t data_len);

#endif /* __LINUX_SOC_MEDIATEK_MTK_GNSS_H */
