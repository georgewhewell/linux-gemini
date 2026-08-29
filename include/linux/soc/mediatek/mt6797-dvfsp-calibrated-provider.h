/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CALIBRATED_PROVIDER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CALIBRATED_PROVIDER_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm-owner.h>
#include <linux/soc/mediatek/mt6797-dvfsp-resource-owner.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-owner-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-identity-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-live-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm-source.h>
#include <linux/soc/mediatek/mt6797-dvfsp-runtime.h>

#define MT6797_DVFSP_CALIBRATED_PROVIDER_ABI	1
#define MT6797_DVFSP_CALIBRATED_SOURCE_ABI	1

struct mt6797_dvfsp_calibrated_provider_source_ops {
	u32 abi;
	int (*fill_calibration)(void *context,
				const struct mt6797_dvfsp_ptp_handoff *handoff,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				const struct mt6797_dvfsp_ppm_snapshot *ppm,
				struct mt6797_dvfsp_ppm_policy_snapshot *policy,
				const struct mt6797_eem_readback *readback,
				const struct mt6797_dvfsp_state_owner_source_identity *identity,
				struct mt6797_eem_calibration_input *input);
	void (*invalidate)(void *context,
				enum mt6797_dvfsp_state_invalidation_reason reason);
};

/*
 * This object binds already-reviewed callback contracts to the resource
 * owner. It is not a platform driver and never registers the handoff owner.
 */
struct mt6797_dvfsp_calibrated_provider {
	struct mt6797_dvfsp_resource_owner *resource_owner;
	struct mt6797_dvfsp_state_owner_source source;
	struct mt6797_dvfsp_state_owner_arbitration arbitration;
	struct mt6797_dvfsp_state_owner_arbitration_ops arbitration_ops;
	struct mt6797_dvfsp_identity_source identity_source;
	struct mt6797_dvfsp_live_source live_source;
	struct mt6797_dvfsp_ppm_source ppm_source;
	struct mt6797_dvfsp_state_owner_source_ops bound_source_ops;
	struct mt6797_dvfsp_ppm_owner_ops bound_ppm_ops;
	const struct mt6797_dvfsp_calibrated_provider_source_ops *source_ops;
	void *source_context;
	bool initialized;
};

int mt6797_dvfsp_calibrated_provider_init(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_state_owner_source_ops *source_ops,
	void *source_context,
	const struct mt6797_dvfsp_ppm_owner_ops *ppm_owner_ops,
	void *ppm_owner_context, u64 owner_handle, u64 transition_handle);

int mt6797_dvfsp_calibrated_provider_exit(
	struct mt6797_dvfsp_calibrated_provider *provider);

int mt6797_dvfsp_calibrated_provider_snapshot(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_calibrated_provider_validate(
	struct mt6797_dvfsp_calibrated_provider *provider,
	const struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_calibrated_provider_identify(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_owner_identity *identity);

/* Populate dormant handoff callbacks; this function never registers them. */
int mt6797_dvfsp_calibrated_provider_owner_ops_init(
	const struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_owner_ops *ops);

void mt6797_dvfsp_calibrated_provider_invalidate(
	struct mt6797_dvfsp_calibrated_provider *provider,
	enum mt6797_dvfsp_state_invalidation_reason reason);

/* Compose the explicit source adapters, but do not register an owner. */
int mt6797_dvfsp_calibrated_provider_bind_sources(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_identity_source *identity_source,
	const struct mt6797_dvfsp_live_source *live_source,
	const struct mt6797_dvfsp_ppm_source *ppm_source,
	const struct mt6797_dvfsp_calibrated_provider_source_ops *source_ops,
	void *source_context, u64 owner_handle, u64 transition_handle);

int mt6797_dvfsp_calibrated_provider_apply_runtime_event(
	struct mt6797_dvfsp_calibrated_provider *provider,
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CALIBRATED_PROVIDER_H */
