/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS runtime lifecycle provider interface.
 *
 * This is intentionally only a handoff of the already-held core capability.
 * It neither discovers the provider nor starts hardware. A future consumer
 * must resolve the explicitly enabled provider node, retain that platform
 * binding for its complete use, then pass this lease to the BTIF owner.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H

#include <linux/soc/mediatek/mt6797-consys.h>

struct device;

/*
 * Return the live provider lease associated with @dev. The caller must
 * serialize this with provider removal and then let the BTIF owner pin the
 * returned capability before any hardware-facing attachment.
 */
const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_platform_lease(struct device *dev);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H */
