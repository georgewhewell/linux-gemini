/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_POWER_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_POWER)
int mt6797_a72_power_cpu_boot_admit(unsigned int cpu);
int mt6797_a72_power_cpu_disable_admit(unsigned int cpu);
int mt6797_a72_power_cpu_kill(unsigned int cpu, int (*firmware_kill)(unsigned int));
/*
 * Serializes internally against MP2 power transitions; callable during CPUHP.
 * Supply a qualified OPP and arrange CCI bandwidth before increasing MP2.
 * Last-core shutdown restores the captured startup VPROC with BUCKB disabled.
 * -EUCLEAN permanently fences this owner and retains the current rails.
 */
int mt6797_a72_power_set_opp(unsigned int cpu, unsigned long rate,
			   unsigned int divisor, unsigned int uv);
/* Provider admission: true only inside this task's owner DVFS transaction. */
bool mt6797_a72_power_owns_dvfs(struct device *consumer);
#else
static inline bool mt6797_a72_power_owns_dvfs(struct device *consumer)
{
	return false;
}
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
