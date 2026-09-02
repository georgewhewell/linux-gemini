/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared, architecture-neutral MT6797 GEN3 SDIO-like command encoder. */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_CODEC_H
#define __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_CODEC_H

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define MT6797_WIFI_AHB_CMD_ADDR_MAX	0x1ffffU
#define MT6797_WIFI_AHB_CMD_ADDR_SHIFT	9
#define MT6797_WIFI_AHB_CMD_FUNC_SHIFT	28
#define MT6797_WIFI_AHB_CMD_FUNC_MAX	7U
#define MT6797_WIFI_AHB_CMD53_COUNT_MAX	0x1ffU
#define MT6797_WIFI_AHB_CMD53_WIFI_READ_WCIR	0x10000004U

static inline int mt6797_wifi_ahb_cmd52_pack(unsigned int write,
					     unsigned int function,
					     unsigned int address,
					     unsigned int data,
					     unsigned int *word)
{
	if (!word || write > 1 || function > MT6797_WIFI_AHB_CMD_FUNC_MAX ||
	    address > MT6797_WIFI_AHB_CMD_ADDR_MAX || data > 0xff)
		return -EINVAL;

	*word = data | (address << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) |
		(function << MT6797_WIFI_AHB_CMD_FUNC_SHIFT);
	if (write)
		*word |= 1U << 31;
	return 0;
}

static inline int mt6797_wifi_ahb_cmd53_read_pack(unsigned int address,
						  unsigned int count,
						  unsigned int *word)
{
	if (!word || !count || count > MT6797_WIFI_AHB_CMD53_COUNT_MAX ||
	    address > MT6797_WIFI_AHB_CMD_ADDR_MAX)
		return -EINVAL;

	*word = count | (address << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) |
		(1U << MT6797_WIFI_AHB_CMD_FUNC_SHIFT);
	return 0;
}

/* Fixed-port CMD53 write, as used by the vendor AHB HIF data path. */
static inline int mt6797_wifi_ahb_cmd53_write_pack(unsigned int address,
					    unsigned int count,
					    unsigned int *word)
{
	int ret;

	ret = mt6797_wifi_ahb_cmd53_read_pack(address, count, word);
	if (ret)
		return ret;
	*word |= 1U << 31;
	return 0;
}

#endif /* __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_CODEC_H */
