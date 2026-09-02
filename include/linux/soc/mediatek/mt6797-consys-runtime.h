/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS runtime lifecycle provider interface.
 *
 * Probe publishes validated resources only. Identity activation is a separate
 * explicit operation owned by a coordinator; no probe path starts hardware.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H

#include <linux/soc/mediatek/mt6797-consys.h>

struct device;
struct mt6797_consys_postid_ops;
struct mt6797_consys_postid_owner;

enum mt6797_consys_runtime_provider_state {
	MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED,
	MT6797_CONSYS_RUNTIME_PROVIDER_RESOURCES_READY,
	MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD,
	MT6797_CONSYS_RUNTIME_PROVIDER_REMOVING,
	MT6797_CONSYS_RUNTIME_PROVIDER_ERROR,
};

/* Caller-owned lease which pins the provider's mapped resources. */
struct mt6797_consys_runtime_resource_lease {
	struct device *device;
};

/* Caller-owned capability which pins the provider through its full lifetime. */
struct mt6797_consys_runtime_provider_lease {
	struct device *device;
	const struct mt6797_consys_runtime_lease *identity;
	bool owns_identity;
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

int mt6797_consys_runtime_resource_lease_acquire(struct device *dev,
		struct mt6797_consys_runtime_resource_lease *lease);
int mt6797_consys_runtime_resource_lease_release(
		struct mt6797_consys_runtime_resource_lease *lease);

/* Acquire identity only after EMI/setup work, returning the canonical lease. */
int mt6797_consys_runtime_platform_identity_acquire(
		struct mt6797_consys_runtime_resource_lease *resources,
		struct mt6797_consys_runtime_provider_lease *lease);
int mt6797_consys_runtime_platform_identity_release(
		struct mt6797_consys_runtime_provider_lease *lease);

/*
 * Borrow the already-held provider identity and block provider removal until
 * the matching release. The caller must serialize acquisition with device
 * removal; the returned capability must remain unchanged until release.
 */
int mt6797_consys_runtime_platform_lease_acquire(struct device *dev,
		struct mt6797_consys_runtime_provider_lease *lease);
int mt6797_consys_runtime_platform_lease_release(
		struct mt6797_consys_runtime_provider_lease *lease);

/*
 * Return the runtime-owned post-ID backend only for its canonical identity
 * lease. The caller must pass that same lease to postid_start(), which pins
 * it for the complete MBIST/AFE/reset-release transaction.
 */
int mt6797_consys_runtime_postid_backend(struct device *dev,
		const struct mt6797_consys_runtime_lease *identity,
		const struct mt6797_consys_postid_ops **ops, void **context);
int mt6797_consys_runtime_postid_owner_init(struct device *dev,
		const struct mt6797_consys_runtime_lease *identity,
		struct mt6797_consys_postid_owner *owner);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_RUNTIME_H */
