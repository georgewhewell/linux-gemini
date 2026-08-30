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

enum mt6797_consys_runtime_provider_state {
	MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED,
	MT6797_CONSYS_RUNTIME_PROVIDER_RESOURCES_READY,
	MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD,
	MT6797_CONSYS_RUNTIME_PROVIDER_REMOVING,
};

/* Caller-owned capability which pins the provider through its full lifetime. */
struct mt6797_consys_runtime_provider_lease {
	struct device *device;
	const struct mt6797_consys_runtime_lease *identity;
};

/*
 * Return the live provider lease associated with @dev. The caller must
 * serialize this with provider removal and then let the BTIF owner pin the
 * returned capability before any hardware-facing attachment.
 */
const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_platform_lease(struct device *dev);

/* Read-only distinction between resource setup and identity ownership. */
enum mt6797_consys_runtime_provider_state
mt6797_consys_runtime_platform_state(struct device *dev);

/*
 * Borrow the already-held provider identity and block provider removal until
 * the matching release. The caller must serialize acquisition with device
 * removal; the returned capability must remain unchanged until release.
 */
int mt6797_consys_runtime_platform_lease_acquire(struct device *dev,
		struct mt6797_consys_runtime_provider_lease *lease);
int mt6797_consys_runtime_platform_lease_release(
		struct mt6797_consys_runtime_provider_lease *lease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H */
