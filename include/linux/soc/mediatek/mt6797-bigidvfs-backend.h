/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_BIGIDVFS_BACKEND_H
#define __LINUX_SOC_MEDIATEK_MT6797_BIGIDVFS_BACKEND_H

#include <linux/types.h>

struct device;

#define MT6797_BIGIDVFS_BACKEND_ABI		1
#define MT6797_BIGIDVFS_FID_READ			0xc200035fUL
#define MT6797_BIGIDVFS_PLL_PCW			0x102224a4U
#define MT6797_BIGIDVFS_PLL_ENABLE_POSDIV	0x102224a0U
#define MT6797_BIGIDVFS_SRAM_SELECTOR		0x102222b0U
#define MT6797_BIGIDVFS_CONTROL			0x10222470U

struct mt6797_bigidvfs_readback {
	u32 abi;
	u32 reserved;
	u64 sample_generation;
	u32 pll_pcw;
	u32 pll_enable_posdiv;
	u32 sram_selector;
	u32 control;
};

int mt6797_bigidvfs_backend_read(
	struct device *dev, struct mt6797_bigidvfs_readback *readback);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_BIGIDVFS_BACKEND_H */
