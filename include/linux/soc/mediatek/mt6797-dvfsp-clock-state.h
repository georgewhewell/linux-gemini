/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_STATE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_STATE_H

#include <linux/bitops.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

#define MT6797_DVFSP_CLOCK_STATE_ABI		1
#define MT6797_DVFSP_CLOCK_STATE_PARENT_MHZ	26
#define MT6797_DVFSP_CLOCK_STATE_PCW_MASK	GENMASK(20, 0)
#define MT6797_DVFSP_CLOCK_STATE_POSDIV_MASK	GENMASK(26, 24)
#define MT6797_DVFSP_CLOCK_STATE_PLL_CHANGE	BIT(31)
#define MT6797_DVFSP_CLOCK_STATE_CLUSTER_COUNT	4

enum mt6797_dvfsp_clock_state_mux {
	MT6797_DVFSP_CLOCK_STATE_MUX_CLKSQ,
	MT6797_DVFSP_CLOCK_STATE_MUX_ARMPLL,
	MT6797_DVFSP_CLOCK_STATE_MUX_MAINPLL,
	MT6797_DVFSP_CLOCK_STATE_MUX_UNIVPLL,
};

enum mt6797_dvfsp_clock_state_cluster_id {
	MT6797_DVFSP_CLOCK_STATE_CLUSTER_LL,
	MT6797_DVFSP_CLOCK_STATE_CLUSTER_L,
	MT6797_DVFSP_CLOCK_STATE_CLUSTER_B,
	MT6797_DVFSP_CLOCK_STATE_CLUSTER_CCI,
};

struct mt6797_dvfsp_clock_state_cluster {
	u32 mux_selector;
	u32 divider_selector;
	u32 pll_pcw;
	u32 posdiv;
	u32 frequency_khz;
};

struct mt6797_dvfsp_clock_state {
	u32 abi;
	u32 reserved;
	u64 clock_sample_generation;
	u64 big_sample_generation;
	struct mt6797_dvfsp_clock_state_cluster
		cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_COUNT];
};

int mt6797_dvfsp_clock_state_decode(
	const struct mt6797_dvfsp_clock_readback *clock,
	const struct mt6797_bigidvfs_readback *big,
	struct mt6797_dvfsp_clock_state *state);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_STATE_H */
