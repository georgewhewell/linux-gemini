/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MTK_WDT_H
#define __LINUX_MTK_WDT_H

#include <linux/errno.h>
#include <linux/types.h>

struct device;

#define MTK_WDT_RECOVERY_TIMEOUT_MS 15000U

struct mtk_wdt_recovery_result {
	u64 identity;
	u32 mode_before;
	u32 mode_after;
	u32 length_after;
	u32 owned;
};

#ifdef CONFIG_MEDIATEK_WATCHDOG_RECOVERY_TAKEOVER
int mtk_wdt_recovery_takeover(struct device *dev, unsigned int timeout_ms,
				      struct mtk_wdt_recovery_result *result);
#else
static inline int mtk_wdt_recovery_takeover(struct device *dev,
					    unsigned int timeout_ms,
					    struct mtk_wdt_recovery_result *result)
{
	(void)dev;
	(void)timeout_ms;
	if (result)
		*result = (struct mtk_wdt_recovery_result){};
	return -EOPNOTSUPP;
}
#endif

#ifdef CONFIG_MEDIATEK_WATCHDOG_RECOVERY_TAKEOVER
struct mtk_wdt_recovery_owner {
	bool owned;
	u64 identity;
};

struct mtk_wdt_recovery_register_ops {
	u32 (*read)(void *context, u32 offset);
	void (*write)(void *context, u32 offset, u32 value);
	u64 (*next_identity)(void *context);
};
#endif

#endif
