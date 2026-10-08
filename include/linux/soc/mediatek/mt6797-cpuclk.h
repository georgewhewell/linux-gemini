/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CPUCLK_H
#define __LINUX_SOC_MEDIATEK_MT6797_CPUCLK_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct clk;

#if IS_ENABLED(CONFIG_COMMON_CLK_MT6797_CPU)
/* May sleep. Validate the consumer's named MP2 clock using a fresh snapshot. */
int mt6797_cpuclk_check_mp2_boot(struct device *consumer, struct clk *clk);
/* A72 power owner only; serialize against CPU_ON/OFF in the caller. */
int mt6797_cpuclk_set_mp2_sync_dcm(struct device *consumer, struct clk *clk,
				 bool enable);
#else
static inline int mt6797_cpuclk_check_mp2_boot(struct device *consumer,
					    struct clk *clk)
{
	return -EOPNOTSUPP;
}
static inline int mt6797_cpuclk_set_mp2_sync_dcm(struct device *consumer,
					      struct clk *clk, bool enable)
{
	return -EOPNOTSUPP;
}
#endif
#endif
