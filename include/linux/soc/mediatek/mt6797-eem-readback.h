/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MT6797_EEM_READBACK_H
#define _LINUX_MT6797_EEM_READBACK_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#define MT6797_EEM_READBACK_ABI		2
#define MT6797_EEM_READBACK_ANCHORS	8
#define MT6797_EEM_READBACK_BANK_COUNT	4
#define MT6797_EEM_READBACK_BANK_BIG	0
#define MT6797_EEM_READBACK_BANK_L		3
#define MT6797_EEM_READBACK_BANK_2L	4
#define MT6797_EEM_READBACK_BANK_CCI	5

struct mt6797_eem_readback_bank {
	u32 bank;
	u32 eem_enable;
	u32 interrupt_status;
	u32 interrupt_raw;
	u32 interrupt_enable;
	u32 dc_values;
	u32 age_values;
	u32 temperature;
	u8 frequency_pct[MT6797_EEM_READBACK_ANCHORS];
	u8 vop[MT6797_EEM_READBACK_ANCHORS];
};

struct mt6797_eem_readback {
	u32 abi;
	u32 bank_mask;
	u32 thermal_interrupt_status;
	u32 modulation_interrupt_status;
	u32 selector_before;
	u32 selector_after;
	s32 temperature_millic;
	struct mt6797_eem_readback_bank
		bank[MT6797_EEM_READBACK_BANK_COUNT];
};

#if IS_ENABLED(CONFIG_MTK_SOC_THERMAL)
int mt6797_thermal_eem_readback(struct device *dev,
				 struct mt6797_eem_readback *readback);
#else
static inline int
mt6797_thermal_eem_readback(struct device *dev,
				 struct mt6797_eem_readback *readback)
{
	return -ENODEV;
}
#endif

#endif
