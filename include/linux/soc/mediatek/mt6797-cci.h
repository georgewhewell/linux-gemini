/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CCI_H
#define __LINUX_SOC_MEDIATEK_MT6797_CCI_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device;

#if IS_ENABLED(CONFIG_ARM_MT6797_CCI_DEVFREQ)
/* Active A72 owner transaction only. Holds the CCI update lock across change. */
int mt6797_cci_run_a72(struct device *consumer, unsigned long rate,
		     int (*change)(void *), void *context);
#else
static inline int mt6797_cci_run_a72(struct device *consumer, unsigned long rate,
				   int (*change)(void *), void *context)
{
	return -EOPNOTSUPP;
}
#endif
#endif
