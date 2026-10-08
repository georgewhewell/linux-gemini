/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H

#include <linux/errno.h>
#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_POWER)
int mt6797_a72_power_cpu_boot_admit(unsigned int cpu);
int mt6797_a72_power_cpu_disable_admit(unsigned int cpu);
int mt6797_a72_power_cpu_kill(unsigned int cpu, int (*firmware_kill)(unsigned int));
/*
 * Caller holds CPU hotplug serialization; may be called from a CPUHP callback.
 * Supply a qualified OPP and arrange CCI bandwidth before increasing MP2.
 * Last-core shutdown restores the captured startup VPROC with BUCKB disabled.
 * -EUCLEAN permanently fences this owner and retains the current rails.
 */
int mt6797_a72_power_set_opp(unsigned int cpu, unsigned long rate,
			   unsigned int divisor, unsigned int uv);
#else
static inline int mt6797_a72_power_cpu_boot_admit(unsigned int cpu)
{
	(void)cpu;
	return -EAGAIN;
}
static inline int mt6797_a72_power_cpu_disable_admit(unsigned int cpu)
{
	return -EOPNOTSUPP;
}
static inline int mt6797_a72_power_cpu_kill(unsigned int cpu,
					 int (*firmware_kill)(unsigned int))
{
	return -EOPNOTSUPP;
}
static inline int mt6797_a72_power_set_opp(unsigned int cpu, unsigned long rate,
					 unsigned int divisor, unsigned int uv)
{
	return -EOPNOTSUPP;
}

#endif

#endif /* __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H */
