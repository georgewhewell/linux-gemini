/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounded MT6797 WMT patch-download controller.
 *
 * This interface deliberately accepts an already-owned WMT transaction
 * endpoint. It does not discover BTIF, CONSYS, Device Tree or power
 * resources, and it cannot establish a transport by itself.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H

struct device;
struct mtk_wmt;

/*
 * Request and preflight the one captured Gemini manifest, then issue only the
 * vendor-evidenced patch-address and patch-data WMT transactions. This is
 * blocked unless the controller's explicit read-only command-line/module
 * parameter is set. The caller must exclusively own the WMT endpoint for the
 * whole call and retains ownership of transport recovery.
 */
int mt6797_consys_wmt_patch_download(struct device *dev, struct mtk_wmt *wmt);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_WMT_PATCH_H */
