/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SNAPSHOT_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SNAPSHOT_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

#define MT6797_DVFSP_STATE_SNAPSHOT_ABI		1

/*
 * This is a private source-to-owner conversion object. Every field is
 * supplied by the future transition owner; no default or board guess is
 * permitted here.
 */
struct mt6797_dvfsp_state_snapshot_input {
	u32 abi;
	u32 reserved;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	const struct mt6797_dvfsp_clock_state *clock;
	const struct mt6797_dvfsp_calibration_state *calibration;
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

int mt6797_dvfsp_state_snapshot_assemble(
	const struct mt6797_dvfsp_state_snapshot_input *input,
	struct mt6797_dvfsp_state_snapshot *snapshot);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SNAPSHOT_H */
