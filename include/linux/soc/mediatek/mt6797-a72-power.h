/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_POWER)
int mt6797_a72_power_prepare(unsigned int cpu);
void mt6797_a72_power_complete(unsigned int cpu, int psci_ret);
void mt6797_a72_power_off(unsigned int cpu);
#else
static inline int mt6797_a72_power_prepare(unsigned int cpu)
{
	return -ENODEV;
}

static inline void mt6797_a72_power_complete(unsigned int cpu, int psci_ret)
{
}

static inline void mt6797_a72_power_off(unsigned int cpu)
{
}
#endif

#endif
