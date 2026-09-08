/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_SRAM_H
#define __LINUX_SOC_MEDIATEK_MT6797_SRAM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct regulator;
struct mt6797_sram_sample {
	u32 control;
	u32 calibration;
};

#if IS_BUILTIN(CONFIG_REGULATOR_MT6797_BIGIDVFS_SRAM)
/* Caller retains the exclusive regulator handle throughout either operation. */
int mt6797_sram_read_boot_sample(struct device *consumer, struct regulator *supply,
			       struct mt6797_sram_sample *sample);
/* CPU hotplug lock must be held; only first CPU8 preparation may call this. */
int mt6797_sram_prepare_cpu8(struct device *consumer, struct regulator *supply,
			   unsigned int cpu, struct mt6797_sram_sample *sample);
#else
static inline int mt6797_sram_read_boot_sample(struct device *consumer,
		struct regulator *supply, struct mt6797_sram_sample *sample)
{
	return -EOPNOTSUPP;
}
static inline int mt6797_sram_prepare_cpu8(struct device *consumer,
		struct regulator *supply, unsigned int cpu,
		struct mt6797_sram_sample *sample)
{
	return -EOPNOTSUPP;
}
#endif
#endif
