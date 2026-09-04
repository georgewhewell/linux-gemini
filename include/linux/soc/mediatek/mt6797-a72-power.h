/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H

#include <linux/errno.h>
#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_POWER)
int mt6797_a72_power_cpu_boot_admit(unsigned int cpu);
#else
static inline int mt6797_a72_power_cpu_boot_admit(unsigned int cpu)
{
	(void)cpu;
	return -EAGAIN;
}
#endif

#endif /* __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H */
