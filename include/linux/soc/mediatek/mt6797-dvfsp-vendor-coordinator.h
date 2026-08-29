/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_COORDINATOR_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_COORDINATOR_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-owner.h>

#define MT6797_DVFSP_VENDOR_COORDINATOR_ABI	1

/*
 * This object is owned by the explicit platform caller.  It records the
 * exact identities passed to the owner so teardown cannot target another
 * vendor adapter accidentally.
 */
struct mt6797_dvfsp_vendor_coordinator {
	struct mt6797_dvfsp_vendor_owner *owner;
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops;
	void *integration_context;
	bool started;
	struct mt6797_dvfsp_vendor_owner *caller_owner;
	const struct mt6797_dvfsp_vendor_integration_ops *caller_integration_ops;
	void *caller_integration_context;
	bool caller_bound;
};

/* Bind the exact identities that the vendor probe/remove hooks will use. */
int mt6797_dvfsp_vendor_coordinator_bind_caller(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context);
int mt6797_dvfsp_vendor_coordinator_unbind_caller(
	struct mt6797_dvfsp_vendor_coordinator *coordinator);
int mt6797_dvfsp_vendor_coordinator_vendor_bind(
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context);
int mt6797_dvfsp_vendor_coordinator_vendor_unbind(void);
int mt6797_dvfsp_vendor_coordinator_vendor_probe(void);
int mt6797_dvfsp_vendor_coordinator_vendor_remove(void);

int mt6797_dvfsp_vendor_coordinator_start(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context);

int mt6797_dvfsp_vendor_coordinator_caller_probe(
	struct mt6797_dvfsp_vendor_coordinator *coordinator);
int mt6797_dvfsp_vendor_coordinator_caller_remove(
	struct mt6797_dvfsp_vendor_coordinator *coordinator);

int mt6797_dvfsp_vendor_coordinator_forward_runtime_event(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	unsigned int type, unsigned int cpu);

int mt6797_dvfsp_vendor_coordinator_stop(
	struct mt6797_dvfsp_vendor_coordinator *coordinator);

bool mt6797_dvfsp_vendor_coordinator_started(
	const struct mt6797_dvfsp_vendor_coordinator *coordinator);

bool mt6797_dvfsp_vendor_coordinator_caller_bound(
	const struct mt6797_dvfsp_vendor_coordinator *coordinator);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_COORDINATOR_H */
