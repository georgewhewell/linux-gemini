/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_TOPCKGEN_H
#define __LINUX_SOC_MEDIATEK_MT6797_TOPCKGEN_H

#include <linux/types.h>

struct device;
struct mt6797_topckgen;

/* Optional supplier; -ENOENT means the consumer DT has no phandle. */
struct mt6797_topckgen *mt6797_topckgen_get(struct device *consumer);
int mt6797_topckgen_cpu_read(struct mt6797_topckgen *top, u8 *value);
/*
 * Compare/update only CLK_MISC_CFG_0[5:4], under the TOPCKGEN mux lock.
 * This does not acquire the CPU/firmware semaphore or switch a CPU clock.
 * The caller must hold the CPU transaction lease and retain the setting
 * whenever a CPU can still be using MAINPLL/UNIVPLL, including on failure.
 */
int mt6797_topckgen_cpu_update(struct mt6797_topckgen *top,
			      u8 expected, u8 value);

#endif
