/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-cspm-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm-policy.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-snapshot.h>
#include <linux/soc/mediatek/mt6797-eem-calibration.h>
#include <linux/soc/mediatek/mt6797-eem-readback.h>

#define MT6797_DVFSP_STATE_SOURCE_ABI		6

/*
 * The caller owns and holds the transition lock while this source is used.
 * These fields are the only live values that the adapter accepts from it;
 * the adapter supplies the decoded clock and calibration object pointers.
 */
struct mt6797_dvfsp_state_source_live {
	u64 cspm_sample_generation;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	struct mt6797_dvfsp_state_provenance provenance;
	u32 state_flags[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 opp_id[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 frequency_khz[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 voltage_uv[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 vsram_uv[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 ceiling_khz[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 floor_khz[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 clock_state[MT6797_DVFSP_STATE_CLUSTER_COUNT];
	u32 rail_state[MT6797_DVFSP_STATE_CLUSTER_COUNT];
};

struct mt6797_dvfsp_state_source_ops {
	u32 abi;
	int (*read_clock)(void *context,
			 struct mt6797_dvfsp_clock_readback *readback);
	int (*read_big)(void *context,
		       struct mt6797_bigidvfs_readback *readback);
	int (*read_eem)(void *context,
		       struct mt6797_eem_readback *readback);
	int (*read_ptp)(void *context,
		       struct mt6797_dvfsp_ptp_handoff *handoff);
	int (*read_ppm)(void *context,
			const struct mt6797_dvfsp_ptp_state *ptp_state,
			struct mt6797_dvfsp_ppm_snapshot *snapshot);
	int (*fill_calibration)(void *context,
				const struct mt6797_dvfsp_ptp_handoff *handoff,
				const struct mt6797_dvfsp_ptp_state *ptp_state,
				const struct mt6797_dvfsp_ppm_snapshot *ppm,
				struct mt6797_dvfsp_ppm_policy_snapshot *policy,
				const struct mt6797_eem_readback *readback,
				struct mt6797_eem_calibration_input *input);
	int (*fill_live)(void *context,
				const struct mt6797_dvfsp_clock_state *clock,
				const struct mt6797_dvfsp_calibration_state *calibration,
				const struct mt6797_dvfsp_cspm_state *cspm,
				const struct mt6797_dvfsp_ppm_snapshot *ppm,
				struct mt6797_dvfsp_state_source_live *live);
};

int mt6797_dvfsp_state_source_snapshot(
	const struct mt6797_dvfsp_state_source_ops *ops, void *context,
	struct mt6797_dvfsp_state_snapshot *snapshot);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_H */
