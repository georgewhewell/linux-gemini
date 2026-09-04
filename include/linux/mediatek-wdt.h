/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MEDIATEK_WDT_H
#define _LINUX_MEDIATEK_WDT_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device;

#if IS_REACHABLE(CONFIG_MEDIATEK_WATCHDOG)
int mtk_wdt_require_boot_running(struct device *consumer);
#else
static inline int mtk_wdt_require_boot_running(struct device *consumer)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_MEDIATEK_WDT_H */
