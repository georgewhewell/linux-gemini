/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CSPM_STATE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CSPM_STATE_H

#include <linux/bitops.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

#define MT6797_DVFSP_CSPM_STATE_ABI		1
#define MT6797_DVFSP_CSPM_STATE_CLUSTER_COUNT	4
#define MT6797_DVFSP_CSPM_STATE_PHYSICAL_COUNT	3
#define MT6797_DVFSP_CSPM_STATE_OPP_COUNT	16

#define MT6797_DVFSP_CSPM_STATE_VALID		BIT(0)
#define MT6797_DVFSP_CSPM_STATE_ON		BIT(1)
#define MT6797_DVFSP_CSPM_STATE_PAUSED		BIT(2)
#define MT6797_DVFSP_CSPM_STATE_LIMITS_VALID	BIT(3)

/* PMIC values are raw vendor codes until a proven regulator conversion exists. */
struct mt6797_dvfsp_cspm_state_cluster {
	u32 raw_swctrl;
	u32 raw_hwsta;
	u32 firmware_opp;
	u32 software_opp;
	u32 voltage_code;
	u32 vsram_code;
	u32 ceiling_opp;
	u32 floor_opp;
	u32 state_flags;
};

struct mt6797_dvfsp_cspm_state {
	u32 abi;
	u32 reserved;
	u64 sample_generation;
	struct mt6797_dvfsp_cspm_state_cluster
		cluster[MT6797_DVFSP_CSPM_STATE_CLUSTER_COUNT];
};

int mt6797_dvfsp_cspm_vproc_code_to_uv(u32 code, u32 *voltage_uv);
int mt6797_dvfsp_cspm_vsram_code_to_uv(u32 code, u32 *voltage_uv);

int mt6797_dvfsp_cspm_state_decode(
	const struct mt6797_dvfsp_clock_readback *readback,
	struct mt6797_dvfsp_cspm_state *state);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CSPM_STATE_H */
