/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_OWNER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_OWNER_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-calibrated-provider.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer-bridge.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-provider.h>

#define MT6797_DVFSP_VENDOR_OWNER_ABI	2
#define MT6797_DVFSP_VENDOR_WRITER_REGISTRATION_ABI	1
#define MT6797_DVFSP_VENDOR_RUNTIME_EVENT_ABI	1
#define MT6797_DVFSP_VENDOR_RUNTIME_EVENT_COUNT	8
#define MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE	(~0U)

enum mt6797_dvfsp_vendor_runtime_event_type {
	MT6797_DVFSP_VENDOR_RUNTIME_CPU_ONLINE,
	MT6797_DVFSP_VENDOR_RUNTIME_CPU_DOWN_PREPARE,
	MT6797_DVFSP_VENDOR_RUNTIME_CPU_DOWN_FAILED,
	MT6797_DVFSP_VENDOR_RUNTIME_PM_SUSPEND_PREPARE,
	MT6797_DVFSP_VENDOR_RUNTIME_PM_POST_SUSPEND,
	MT6797_DVFSP_VENDOR_RUNTIME_CLOCK_TRANSITION,
	MT6797_DVFSP_VENDOR_RUNTIME_RAIL_TRANSITION,
	MT6797_DVFSP_VENDOR_RUNTIME_PCM_FAULT,
};

struct mt6797_dvfsp_vendor_runtime_ops {
	u32 abi;
	int (*event)(void *context, unsigned int type, unsigned int cpu);
};

#define MT6797_DVFSP_VENDOR_OWNER_LIFECYCLE_ABI	1

struct mt6797_dvfsp_vendor_owner_lifecycle_ops {
	u32 abi;
	int (*register_writer)(void *context);
	int (*unregister_writer)(void *context);
};

#define MT6797_DVFSP_VENDOR_INTEGRATION_ABI	2

/*
 * Opaque, source-independent callbacks for the external vendor adapter.
 * Both trees validate their local ABI and retain exact table/context identity.
 */
struct mt6797_dvfsp_vendor_integration_ops {
	u32 abi;
	int (*bind_lifecycle)(void *context, const void *bridge_ops,
		void *bridge_context, const void *runtime_ops,
		void *runtime_context, const void *owner_lifecycle_ops,
		void *owner_lifecycle_context, u64 owner_handle,
		u64 transition_handle);
	int (*unbind_lifecycle)(void *context, const void *bridge_ops,
		void *bridge_context, const void *runtime_ops,
		void *runtime_context, const void *owner_lifecycle_ops,
		void *owner_lifecycle_context, u64 owner_handle,
		u64 transition_handle);
	int (*register_mainline_owner)(void *context, const void *bridge_ops,
		void *bridge_context, const void *runtime_ops,
		void *runtime_context, u64 owner_handle,
		u64 transition_handle);
	int (*unregister_mainline_owner)(void *context, const void *bridge_ops,
		void *bridge_context, const void *runtime_ops,
		void *runtime_context, u64 owner_handle,
		u64 transition_handle);
	const void *source_registration;
};

/* The external vendor-aware caller owns this registration lifetime. */
struct mt6797_dvfsp_vendor_writer_registration_ops {
	u32 abi;
	int (*register_owner)(void *context,
		const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops,
		void *bridge_context,
		const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
		void *runtime_context, u64 owner_handle, u64 transition_handle);
	int (*unregister_owner)(void *context,
		const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops,
		void *bridge_context,
		const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
		void *runtime_context, u64 owner_handle, u64 transition_handle);
};

/*
 * A vendor-aware driver supplies these callbacks while retaining ownership
 * of its existing policy writers and transition lock. The adapter never
 * installs or replaces a vendor callback through a single-slot setter.
 */
struct mt6797_dvfsp_vendor_owner_ops {
	u32 abi;
	const struct mt6797_dvfsp_identity_provenance_ops *identity_ops;
	void *identity_context;
	const struct mt6797_dvfsp_ppm_source_ops *ppm_ops;
	void *ppm_context;
	const struct mt6797_dvfsp_live_source_ops *live_ops;
	void *live_context;
	const struct mt6797_dvfsp_calibrated_provider_source_ops *provider_ops;
	void *provider_context;
	const struct mt6797_dvfsp_vendor_writer_ops *writer_ops;
	void *writer_context;
	const struct mt6797_dvfsp_vendor_writer_registration_ops
		*writer_registration_ops;
	void *writer_registration_context;
	const struct mt6797_dvfsp_vendor_runtime_ops *writer_runtime_ops;
	void *writer_runtime_context;
};

/*
 * This object owns callback composition, provider lifetime, and explicit
 * read-only handoff registration. It performs no state-changing operation.
 */
struct mt6797_dvfsp_vendor_owner {
	struct mt6797_dvfsp_resource_owner *resource_owner;
	struct mt6797_dvfsp_vendor_owner_ops ops;
	struct mt6797_dvfsp_identity_source identity_source;
	struct mt6797_dvfsp_live_source live_source;
	struct mt6797_dvfsp_ppm_source ppm_source;
	struct mt6797_dvfsp_calibrated_provider provider;
	struct mt6797_dvfsp_vendor_writer writer;
	struct mt6797_dvfsp_vendor_writer_bridge writer_bridge;
	struct mt6797_dvfsp_vendor_source source;
	struct mt6797_dvfsp_vendor_provider_bridge provider_bridge;
	struct mt6797_dvfsp_handoff *state_handoff;
	const struct mt6797_dvfsp_vendor_runtime_ops *writer_runtime_ops;
	void *writer_runtime_context;
	const struct mt6797_dvfsp_vendor_integration_ops *writer_integration_ops;
	void *writer_integration_context;
	bool writer_integration_bound;
	u64 owner_handle;
	u64 transition_handle;
	bool bound;
	bool writer_registered;
	bool state_registered;
};

int mt6797_dvfsp_vendor_owner_bind(
	struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_vendor_owner_ops *ops,
	u64 owner_handle, u64 transition_handle);

/* Register the exact bridge table with an external vendor-aware caller. */
int mt6797_dvfsp_vendor_owner_register_writer(
	struct mt6797_dvfsp_vendor_owner *owner);

/* Unregister the external caller before owner teardown. */
int mt6797_dvfsp_vendor_owner_unregister_writer(
	struct mt6797_dvfsp_vendor_owner *owner);

/* Register only a complete, matching read-only state view. */
int mt6797_dvfsp_vendor_owner_register_state(
	struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_handoff *handoff);

/* Remove the state owner before the writer or source lifecycle. */
int mt6797_dvfsp_vendor_owner_unregister_state(
	struct mt6797_dvfsp_vendor_owner *owner);

/* Bind the external vendor lifecycle adapter before its probe can run. */
int mt6797_dvfsp_vendor_owner_bind_writer_integration(
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context);

/* Unbind only after the vendor lifecycle has removed its owner. */
int mt6797_dvfsp_vendor_owner_unbind_writer_integration(
	struct mt6797_dvfsp_vendor_owner *owner);

/* Invalidate the shared source, then forward one vendor lifecycle event. */
int mt6797_dvfsp_vendor_owner_forward_runtime_event(
	struct mt6797_dvfsp_vendor_owner *owner, unsigned int type,
	unsigned int cpu);

int mt6797_dvfsp_vendor_owner_unbind(
	struct mt6797_dvfsp_vendor_owner *owner);

/* Populate dormant handoff callbacks; this does not register the owner. */
int mt6797_dvfsp_vendor_owner_state_ops_init(
	const struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_state_owner_ops *state_ops);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_OWNER_H */
