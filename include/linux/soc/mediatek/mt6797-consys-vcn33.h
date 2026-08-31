/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_VCN33_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_VCN33_H

#include <linux/errno.h>
#include <linux/types.h>

/* MT6351 VCN33 selectors are 3.3, 3.4, 3.5 and 3.6 V in that order. */
static inline int mt6797_consys_vcn33_level_to_uv(u8 level, int *uv)
{
	if (!uv || level > 3)
		return -ERANGE;
	*uv = 3300000 + level * 100000;
	return 0;
}

static inline int mt6797_consys_vcn33_uv_to_level(int uv, u8 *level)
{
	if (!level || uv < 3300000 || uv > 3600000 ||
	    (uv - 3300000) % 100000)
		return -ERANGE;
	*level = (uv - 3300000) / 100000;
	return 0;
}

#endif
