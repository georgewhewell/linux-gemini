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
/*
 * CPU hotplug lock must be held. First CPU8 preparation, or one restart
 * authorized by record_cpu8_off after the power owner verified BUCKB off.
 */
int mt6797_sram_prepare_cpu8(struct device *consumer, struct regulator *supply,
			   unsigned int cpu, struct mt6797_sram_sample *sample);
int mt6797_sram_record_cpu8_off(struct device *consumer, struct regulator *supply);
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
static inline int mt6797_sram_record_cpu8_off(struct device *consumer,
					   struct regulator *supply)
{
	return -EOPNOTSUPP;
}
#endif
#endif
