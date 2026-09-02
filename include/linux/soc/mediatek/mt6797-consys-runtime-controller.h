/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS runtime bootstrap controller interface.
 *
 * A controller platform device has already acquired managed links to the
 * CONSYS runtime and BTIF PIO platform providers. start() runs the
 * vendor-evidenced mandatory-to-full STP handover and retains the resulting
 * opaque session. It creates no wireless client. stop() closes and destroys
 * that handover before releasing the BTIF runtime pin.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H

#include <linux/types.h>

struct device;
struct regulator;
struct mt6797_consys_runtime_controller;
struct mt6797_consys_stp_session_lease;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;

/*
 * One controller-owned GNSS borrower.  The object is caller-owned, must be
 * zero-initialized before its first get, and must remain unchanged until
 * mt6797_consys_runtime_controller_gnss_session_put().
 * It is deliberately not a general STP capability: it exists to compose the
 * one channel-2 GNSS endpoint with the controller's retained full-STP lease.
 */
struct mt6797_consys_runtime_controller_gnss_lease {
	struct mt6797_consys_runtime_controller *controller;
	u64 generation;
};

/* One retained WLAN HIF borrower; zero-initialize and return it before stop. */
struct mt6797_consys_runtime_controller_wifi_lease {
	struct mt6797_consys_runtime_controller *controller;
	u64 generation;
};

/* One Bluetooth channel-zero borrower; zero-initialize before get. */
struct mt6797_consys_runtime_controller_bt_lease {
	struct mt6797_consys_runtime_controller *controller;
	u64 generation;
};

/*
 * The caller must retain @dev and serialize this operation with controller
 * removal; managed supplier links then preserve the runtime/BTIF providers.
 */
int mt6797_consys_runtime_controller_start(struct device *dev,
		struct regulator *bt_paldo, struct regulator *wifi_paldo);

/* Safe for teardown and idempotent after a failed or completed handover. */
void mt6797_consys_runtime_controller_stop(struct device *dev);

/*
 * Borrow the controller's active full-STP lease for exactly one GNSS endpoint.
 * This neither registers /dev/gnssN nor issues WMT GPS function control.  The
 * returned session stays valid only until the matching put(); a GNSS caller
 * must detach its channel-2 callback (mtk_gnss_unregister()) before that put.
 * stop() rejects new borrows and waits for the outstanding borrower, then
 * destroys the bootstrap/session in the required reverse order.
 *
 * The caller must keep @dev alive while this lease is held.  This is separate
 * from the runtime lifecycle; it cannot make an inactive controller runnable.
 */
int mt6797_consys_runtime_controller_gnss_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease,
		const struct mt6797_consys_stp_session_lease **session_lease);
void mt6797_consys_runtime_controller_gnss_session_put(
		struct mt6797_consys_runtime_controller_gnss_lease *gnss_lease);

/*
 * Channel-zero endpoint lease. It may coexist with the separately owned GNSS
 * and WLAN channels on one full-STP session. WMT transactions remain serialized
 * by the bootstrap transaction lock; this exposes only BT FUNC_CTRL, not a
 * general WMT path. The caller must serialize bt_function_set() with the
 * matching bt_session_put().
 */
int mt6797_consys_runtime_controller_bt_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease,
		const struct mt6797_consys_stp_session_lease **session_lease);
void mt6797_consys_runtime_controller_bt_session_put(
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease);
int mt6797_consys_runtime_controller_bt_function_set(
		struct mt6797_consys_runtime_controller_bt_lease *bt_lease,
		bool enable);

int mt6797_consys_runtime_controller_wifi_session_get(struct device *dev,
		struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease,
		const struct mt6797_consys_stp_session_lease **session_lease);
void mt6797_consys_runtime_controller_wifi_session_put(
		struct mt6797_consys_runtime_controller_wifi_lease *wifi_lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H */
