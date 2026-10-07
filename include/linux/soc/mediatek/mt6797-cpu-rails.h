/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MT6797_CPU_RAILS_H
#define _LINUX_MT6797_CPU_RAILS_H

#include <linux/types.h>

/*
 * Vendor mt_cpufreq.c uses mV * 100 (10 uV units): MIN/MAX_VSRAM_VOLT
 * are 100000/120000, NORMAL_DIFF_VRSAM_VPROC is 10000 and
 * MAX_DIFF_VSRAM_VPROC is 30000. Convert each quantity to microvolts.
 * The enforced lower bound is Vsram >= Vproc, not the nominal offset:
 * the SRAM ceiling permits equal rails at Vproc = 1.2 V.
 */
#define MT6797_CPU_VSRAM_MIN_UV		1000000U
#define MT6797_CPU_VSRAM_MAX_UV		1200000U
#define MT6797_CPU_VSRAM_NORMAL_DELTA_UV	100000U
#define MT6797_CPU_VSRAM_MAX_DELTA_UV	300000U

static inline bool mt6797_cpu_rails_valid(u32 vproc_uv, u32 vsram_uv)
{
	return vproc_uv && vsram_uv >= MT6797_CPU_VSRAM_MIN_UV &&
		vsram_uv <= MT6797_CPU_VSRAM_MAX_UV && vsram_uv >= vproc_uv &&
		vsram_uv - vproc_uv <= MT6797_CPU_VSRAM_MAX_DELTA_UV;
}

#endif
