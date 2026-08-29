/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RESOURCE_OWNER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RESOURCE_OWNER_H

#include <linux/mutex.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-state-owner-source.h>

#define MT6797_DVFSP_RESOURCE_OWNER_ABI	1

/*
 * This object owns resource lifetime and serialization only. It is not a
 * state provider and never registers the handoff owner by itself.
 */
struct mt6797_dvfsp_resource_owner {
	struct mutex lifecycle_lock;
	struct mutex transition_lock;
	struct mt6797_dvfsp_state_source_devices devices;
	u64 generation;
	u64 transition_handle;
	bool initialized;
	bool attached;
	bool source_bound;
	bool writes_disabled;
};

int mt6797_dvfsp_resource_owner_init(
	struct mt6797_dvfsp_resource_owner *owner, u64 transition_handle);

int mt6797_dvfsp_resource_owner_attach(
	struct mt6797_dvfsp_resource_owner *owner,
	const struct mt6797_dvfsp_state_source_devices *devices);

int mt6797_dvfsp_resource_owner_detach(
	struct mt6797_dvfsp_resource_owner *owner);

/* Bind a calibrated source before borrowing the attached device tuple. */
int mt6797_dvfsp_resource_owner_bind_source(
	struct mt6797_dvfsp_resource_owner *owner);

int mt6797_dvfsp_resource_owner_unbind_source(
	struct mt6797_dvfsp_resource_owner *owner);

/* The caller must hold the owner transition lock while advancing the epoch. */
int mt6797_dvfsp_resource_owner_advance_generation(
	struct mt6797_dvfsp_resource_owner *owner);

int mt6797_dvfsp_resource_owner_arbitration_ops_init(
	const struct mt6797_dvfsp_resource_owner *owner,
	struct mt6797_dvfsp_state_owner_arbitration_ops *ops);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RESOURCE_OWNER_H */
