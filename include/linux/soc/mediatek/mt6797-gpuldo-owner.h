/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_MT6797_GPULDO_OWNER_H
#define LINUX_MT6797_GPULDO_OWNER_H
#include <linux/errno.h>
#include <linux/kconfig.h>
struct device_node;
#if IS_REACHABLE(CONFIG_MTK_SCPSYS)
int mtk_scpsys_gpuldo_gpu_admit(struct device_node *gpu);
#else
static inline int mtk_scpsys_gpuldo_gpu_admit(struct device_node *gpu)
{
	return -ENODEV;
}
#endif
#endif
