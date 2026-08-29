/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_OWNER_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_OWNER_SOURCE_H

#include <linux/mutex.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm-owner.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-source-backends.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-source.h>

struct mt6797_dvfsp_handoff;

#define MT6797_DVFSP_STATE_OWNER_SOURCE_ABI	6

/* This identity is read from the selected silicon/PTP source, never guessed. */
struct mt6797_dvfsp_state_owner_source_identity {
	u32 abi;
	u32 variant_id;
	u64 table_epoch;
	u64 calibration_handle;
};

struct mt6797_dvfsp_state_owner_source_ops {
	u32 abi;
	int (*read_identity)(void *context,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				struct mt6797_dvfsp_state_owner_source_identity *identity);
	int (*fill_calibration)(void *context,
				const struct mt6797_dvfsp_ptp_handoff *handoff,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				const struct mt6797_dvfsp_ppm_snapshot *ppm,
				struct mt6797_dvfsp_ppm_policy_snapshot *policy,
				const struct mt6797_eem_readback *readback,
				const struct mt6797_dvfsp_state_owner_source_identity *identity,
				struct mt6797_eem_calibration_input *input);
	int (*fill_live)(void *context,
				const struct mt6797_dvfsp_clock_state *clock,
				const struct mt6797_dvfsp_calibration_state *calibration,
				const struct mt6797_dvfsp_cspm_state *cspm,
				const struct mt6797_dvfsp_ppm_snapshot *ppm,
				struct mt6797_dvfsp_state_source_live *live);
	void (*invalidate)(void *context,
				enum mt6797_dvfsp_state_invalidation_reason reason);
};

/* Device pointers are borrowed and must remain alive for the owner lifetime. */
struct mt6797_dvfsp_state_owner_source {
	struct mutex transition_lock;
	struct mt6797_dvfsp_state_source_devices devices;
	struct mt6797_dvfsp_state_source_ops adapter_ops;
	const struct mt6797_dvfsp_state_owner_source_ops *source_ops;
	void *source_context;
	const struct mt6797_dvfsp_ppm_owner_ops *ppm_owner_ops;
	void *ppm_owner_context;
	struct mt6797_dvfsp_ppm_policy_snapshot ppm_policy;
	struct mt6797_dvfsp_state_provenance provenance;
	u64 owner_handle;
	u64 transition_handle;
	bool initialized;
	bool invalidated;
	bool ppm_policy_valid;
};

int mt6797_dvfsp_state_owner_source_init(
	struct mt6797_dvfsp_state_owner_source *owner,
	const struct mt6797_dvfsp_state_source_devices *devices,
	const struct mt6797_dvfsp_state_owner_source_ops *source_ops,
	void *source_context,
	const struct mt6797_dvfsp_ppm_owner_ops *ppm_owner_ops,
	void *ppm_owner_context, u64 owner_handle, u64 transition_handle);

int mt6797_dvfsp_state_owner_source_snapshot(
	struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_state_owner_source_validate(
	struct mt6797_dvfsp_state_owner_source *owner,
	const struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_state_owner_source_identify(
	struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_owner_identity *identity);

void mt6797_dvfsp_state_owner_source_invalidate(
	struct mt6797_dvfsp_state_owner_source *owner,
	enum mt6797_dvfsp_state_invalidation_reason reason);

/* Populate the dormant registry callbacks; this does not register the owner. */
int mt6797_dvfsp_state_owner_source_owner_ops_init(
	const struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_owner_ops *ops);

#define MT6797_DVFSP_STATE_OWNER_ARBITRATION_ABI	1

struct mt6797_dvfsp_state_owner_arbitration_ops {
	u32 abi;
	int (*lock)(void *context);
	void (*unlock)(void *context);
	int (*read_generation)(void *context, u64 *generation);
};

struct mt6797_dvfsp_state_owner_arbitration {
	struct mt6797_dvfsp_state_owner_source *source;
	const struct mt6797_dvfsp_state_owner_arbitration_ops *ops;
	void *context;
	u64 last_generation;
	bool faulted;
	bool initialized;
	struct mt6797_dvfsp_state_owner_ops owner_ops;
	struct mt6797_dvfsp_handoff *handoff;
	struct mutex lifecycle_lock;
	bool registered;
	bool hold_active;
	u64 hold_generation;
	u32 hold_cluster_mask;
	u64 hold_owner_handle;
};

int mt6797_dvfsp_state_owner_arbitration_init(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_source *source,
	const struct mt6797_dvfsp_state_owner_arbitration_ops *ops,
	void *context);

int mt6797_dvfsp_state_owner_arbitration_snapshot(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_state_owner_arbitration_validate(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	const struct mt6797_dvfsp_state_snapshot *snapshot);

int mt6797_dvfsp_state_owner_arbitration_identify(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_identity *identity);

void mt6797_dvfsp_state_owner_arbitration_invalidate(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	enum mt6797_dvfsp_state_invalidation_reason reason);

/* Populate dormant callbacks only; no handoff owner is registered. */
int mt6797_dvfsp_state_owner_arbitration_owner_ops_init(
	const struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_ops *ops);

int mt6797_dvfsp_state_owner_arbitration_register(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_arbitration *arbitration);

int mt6797_dvfsp_state_owner_arbitration_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_arbitration *arbitration);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_OWNER_SOURCE_H */
