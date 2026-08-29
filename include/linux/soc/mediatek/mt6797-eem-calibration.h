/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MT6797_EEM_CALIBRATION_H
#define _LINUX_MT6797_EEM_CALIBRATION_H

#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm-policy.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>
#include <linux/soc/mediatek/mt6797-eem-readback.h>

#define MT6797_EEM_CALIBRATION_ABI		2
#define MT6797_EEM_CALIBRATION_ROWS		16
#define MT6797_EEM_CALIBRATION_BANKS		4
#define MT6797_EEM_CALIBRATION_MON_ENABLE	0x2
#define MT6797_EEM_CALIBRATION_ENABLE_MASK	0x7
#define MT6797_EEM_CALIBRATION_MON_STATUS_MASK	GENMASK(23, 16)
#define MT6797_EEM_CALIBRATION_TEMP_OFFSET_UV	62500
#define MT6797_EEM_CALIBRATION_VSRAM_MIN_UV	1000000
#define MT6797_EEM_CALIBRATION_VSRAM_MAX_UV	1200000
#define MT6797_EEM_CALIBRATION_VSRAM_DELTA_MIN_UV	10000
#define MT6797_EEM_CALIBRATION_VSRAM_DELTA_MAX_UV	30000

/* Arrays use the packed bank order BIG, L, 2L, CCI and EEM row order. */
struct mt6797_eem_calibration_input {
	const struct mt6797_eem_readback *readback;
	const struct mt6797_dvfsp_ptp_state *ptp_state;
	const struct mt6797_dvfsp_ppm_snapshot *ppm_snapshot;
	const struct mt6797_dvfsp_ppm_policy_snapshot *ppm_policy;
	u8 frequency_pct[MT6797_EEM_CALIBRATION_BANKS]
		[MT6797_EEM_CALIBRATION_ROWS];
	u32 frequency_khz[MT6797_EEM_CALIBRATION_BANKS]
		[MT6797_EEM_CALIBRATION_ROWS];
	u32 record_vproc_uv[MT6797_EEM_CALIBRATION_BANKS]
		[MT6797_EEM_CALIBRATION_ROWS];
	u32 vsram_uv[MT6797_EEM_CALIBRATION_BANKS]
		[MT6797_EEM_CALIBRATION_ROWS];
	u32 ppm_limit_khz[MT6797_EEM_CALIBRATION_BANKS]
		[MT6797_EEM_CALIBRATION_ROWS];
	u32 vmin_uv[MT6797_EEM_CALIBRATION_BANKS];
	u32 vmax_uv[MT6797_EEM_CALIBRATION_BANKS];
	s32 temperature_millic;
	u64 thermal_generation;
	u64 clock_owner_generation;
	u64 rail_owner_generation;
	u64 source_generation;
	struct mt6797_dvfsp_state_provenance provenance;
};

#if IS_ENABLED(CONFIG_MTK_SOC_THERMAL)
int mt6797_eem_calibration_build(
	const struct mt6797_eem_calibration_input *input,
	struct mt6797_dvfsp_calibration_state *state);
#else
static inline int mt6797_eem_calibration_build(
	const struct mt6797_eem_calibration_input *input,
	struct mt6797_dvfsp_calibration_state *state)
{
	return -ENODEV;
}
#endif

#endif
