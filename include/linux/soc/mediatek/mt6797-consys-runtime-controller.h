/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS runtime bootstrap controller interface.
 *
 * A controller platform device has already acquired managed links to the
 * separately gated CONSYS runtime and BTIF PIO platform providers.  start()
 * runs the one vendor-evidenced mandatory-to-full STP handover and retains the
 * resulting opaque session.  It creates no wireless client.  stop() fences
 * and destroys that handover before releasing the BTIF runtime pin.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H

struct device;

/*
 * Both the controller DT property and its default-false parameter gate start.
 * The caller must retain @dev and serialize this operation with controller
 * removal; managed supplier links then preserve the runtime/BTIF providers.
 */
int mt6797_consys_runtime_controller_start(struct device *dev);

/* Safe for teardown and idempotent after a failed or completed handover. */
void mt6797_consys_runtime_controller_stop(struct device *dev);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_CONTROLLER_H */
