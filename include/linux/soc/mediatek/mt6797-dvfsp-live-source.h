/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_LIVE_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_LIVE_SOURCE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-cspm-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-source.h>

#define MT6797_DVFSP_LIVE_SOURCE_ADAPTER_ABI	1
#define MT6797_DVFSP_LIVE_SOURCE_CLUSTER_COUNT	4

/* One callback result for a cluster, with no default or inferred field. */
struct mt6797_dvfsp_live_source_cluster {
	u32 state_flags;
	u32 opp_id;
	u32 frequency_khz;
	u32 vproc_code;
	u32 vsram_code;
	u32 vproc_uv;
	u32 vsram_uv;
	u32 floor_khz;
	u32 ceiling_khz;
	u32 clock_state;
	u32 rail_state;
};

struct mt6797_dvfsp_live_source_ops {
	u32 abi;

	/* Called while the state owner's outer transition lock is held. */
	int (*read_generation)(void *context,
				       const struct mt6797_dvfsp_clock_state *clock,
				       const struct mt6797_dvfsp_calibration_state *calibration,
				       const struct mt6797_dvfsp_cspm_state *cspm,
				       const struct mt6797_dvfsp_ppm_snapshot *ppm,
				       u64 *generation, u64 *owner_handle,
				       u64 *transition_handle,
				       struct mt6797_dvfsp_state_provenance *provenance);
	int (*read_cluster)(void *context, unsigned int cluster,
				   const struct mt6797_dvfsp_clock_state *clock,
				   const struct mt6797_dvfsp_calibration_state *calibration,
				   const struct mt6797_dvfsp_cspm_state *cspm,
				   const struct mt6797_dvfsp_ppm_snapshot *ppm,
				   struct mt6797_dvfsp_live_source_cluster *state);
};

/* Device and provider references remain borrowed by the caller. */
struct mt6797_dvfsp_live_source {
	const struct mt6797_dvfsp_live_source_ops *ops;
	void *context;
};

/* Populate the existing state-owner fill_live callback; never registers it. */
int mt6797_dvfsp_live_source_fill(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_state_source_live *live);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_LIVE_SOURCE_H */
