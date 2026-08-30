/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek CONSYS GNSS client for the STP multiplexer.
 *
 * This is a kernel-internal endpoint, not a hardware driver.  A transport
 * owner that has established the controller-specific WMT GPS transaction may
 * register one instance against an active opaque full-STP session. There is
 * intentionally no platform binding: neither the MT6797 GNSS control command
 * nor its required firmware state has been established for mainline.
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
 * so that a transport recovery is required before reopening.
 */
int mtk_gnss_start(struct mtk_gnss *gnss);
void mtk_gnss_stop(struct mtk_gnss *gnss);
int mtk_gnss_write(struct mtk_gnss *gnss, const u8 *data, size_t data_len);

/*
 * Notify this client that its owner has reset/quiesced the controller. It
 * only clears local GNSS state; the owner remains responsible for resetting
 * its shared STP and WMT layers at their proper transaction boundary.
 */
void mtk_gnss_transport_reset(struct mtk_gnss *gnss);

#endif /* __LINUX_SOC_MEDIATEK_MTK_GNSS_H */
