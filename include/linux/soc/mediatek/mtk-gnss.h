/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek CONSYS GNSS client for the STP multiplexer.
 *
 * This is a kernel-internal endpoint, not a hardware driver.  A transport
 * owner that has established the controller-specific WMT GPS transaction may
 * register one instance against its STP decoder.  There is intentionally no
 * platform binding: neither the MT6797 GNSS control command nor its required
 * firmware state has been established for mainline.
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_GNSS_H
#define __LINUX_SOC_MEDIATEK_MTK_GNSS_H

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <linux/soc/mediatek/mtk-stp.h>

struct device;
struct gnss_device;

/*
 * transmit() consumes one complete STP frame and returns zero on success.
 * set_power() performs the transport owner's proven WMT GPS on/off operation.
 * Both callbacks run without the client lock and may synchronously feed STP
 * receive data. They must return before the caller may start another session
 * transition; the client serializes that boundary for them.
 */
struct mtk_gnss_transport {
	int (*transmit)(void *context, const u8 *frame, size_t frame_len);
	int (*set_power)(void *context, bool on);
	void *context;
};

struct mtk_gnss {
	/* Serializes session state, callbacks, and the GNSS raw FIFO handoff. */
	struct mutex lock;
	struct mtk_stp *stp;
	struct gnss_device *gdev;
	struct mtk_gnss_transport transport;
	enum mtk_stp_mode stp_mode;
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
 * Register an ordinary /dev/gnssN endpoint on STP channel 2.  gnss must be
 * zero-initialized before its first registration. The caller serializes this
 * function and mtk_gnss_unregister() against mtk_stp_rx(), as required by the
 * STP client registration API. This function performs no hardware access and
 * does not power the GNSS function on. Unregister waits for an issued callback
 * but does not itself quiesce the shared byte transport before STP unbind.
 */
int mtk_gnss_register(struct mtk_gnss *gnss, struct device *parent,
		      struct mtk_stp *stp, enum mtk_stp_mode stp_mode,
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
 * only clears local session state; the owner remains responsible for resetting
 * the shared STP and WMT layers at their proper transaction boundary.
 */
void mtk_gnss_transport_reset(struct mtk_gnss *gnss);

#endif /* __LINUX_SOC_MEDIATEK_MTK_GNSS_H */
