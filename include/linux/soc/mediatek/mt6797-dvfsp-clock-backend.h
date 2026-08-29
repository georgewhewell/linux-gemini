/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_BACKEND_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_BACKEND_H

#include <linux/types.h>

#define MT6797_DVFSP_CLOCK_BACKEND_ABI	2
#define MT6797_DVFSP_CLOCK_BACKEND_MCUMIXED_BASE	0x1001a000ULL
#define MT6797_DVFSP_CLOCK_BACKEND_MCUMIXED_LENGTH	0x1000
#define MT6797_DVFSP_CLOCK_BACKEND_CSPM_BASE	0x11015000ULL
#define MT6797_DVFSP_CLOCK_BACKEND_CSPM_LENGTH	0x1000
#define MT6797_DVFSP_CLOCK_BACKEND_SEMAPHORE_OFFSET	0x440
#define MT6797_DVFSP_CLOCK_BACKEND_SEMAPHORE_TIMEOUT_US	2000

struct device;

/* Raw words only: no frequency, OPP, voltage, or rail interpretation. */
struct mt6797_dvfsp_clock_readback {
	u32 abi;
	u32 reserved;
	u64 sample_generation;
	u32 armplldiv_muxsel;
	u32 armplldiv_ckdiv;
	u32 pll_ll[3];
	u32 pll_l[3];
	u32 pll_cci[3];
	u32 cspm_swctrl[3];
	u32 cspm_hwsta[4];
};

int mt6797_dvfsp_clock_backend_read(
	struct device *dev, struct mt6797_dvfsp_clock_readback *readback);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_CLOCK_BACKEND_H */
