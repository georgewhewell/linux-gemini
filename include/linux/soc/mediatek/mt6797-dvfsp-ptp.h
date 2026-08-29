/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PTP_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PTP_H

#include <linux/types.h>

#include <linux/bitops.h>

#define MT6797_DVFSP_PTP_HANDOFF_ABI		1
#define MT6797_DVFSP_PTP_HANDOFF_WORDS		19
#define MT6797_DVFSP_PTP_STATE_ABI		1
#define MT6797_DVFSP_PTP_BANK_MASK_BIG		BIT(0)
#define MT6797_DVFSP_PTP_BANK_MASK_L		BIT(1)
#define MT6797_DVFSP_PTP_BANK_MASK_2L		BIT(2)
#define MT6797_DVFSP_PTP_BANK_MASK_CCI		BIT(3)
#define MT6797_DVFSP_PTP_BANK_MASK_ALL		GENMASK(3, 0)
#define MT6797_DVFSP_PTP_BANK_BIG		0
#define MT6797_DVFSP_PTP_BANK_L		3
#define MT6797_DVFSP_PTP_BANK_2L		4
#define MT6797_DVFSP_PTP_BANK_CCI		5
#define MT6797_DVFSP_EFUSE_IDENTITY_ABI		1
#define MT6797_DVFSP_EFUSE_FUNCTION_WORD_INDEX	22
#define MT6797_DVFSP_EFUSE_DATE_WORD_INDEX	61

#define MT6797_DVFSP_PTP_IDENTITY_ABI		1
#define MT6797_DVFSP_PTP_DATE_1221		0
#define MT6797_DVFSP_PTP_DATE_0119		1

struct mt6797_dvfsp_efuse_identity {
	u32 abi;
	u32 function_word;
	u32 date_word;
};

struct mt6797_dvfsp_ptp_identity {
	u32 abi;
	u32 variant_id;
	u32 date_code;
	u32 cpu_level;
	u32 function_code;
	u32 tt_segment;
};

struct mt6797_dvfsp_ptp_handoff {
	u32 abi;
	u32 m_hw_res[MT6797_DVFSP_PTP_HANDOFF_WORDS];
};

struct mt6797_dvfsp_ptp_bank_state {
	u32 bank;
	u32 init_enable;
	u32 mon_enable;
	u32 dvfs_level;
	u32 bin_spec;
};

struct mt6797_dvfsp_ptp_state {
	u32 abi;
	u32 bank_mask;
	struct mt6797_dvfsp_ptp_bank_state
		bank[4];
};

int mt6797_dvfsp_ptp_decode(
	const struct mt6797_dvfsp_ptp_handoff *handoff,
	struct mt6797_dvfsp_ptp_state *state);

int mt6797_dvfsp_ptp_identity_decode(
	const struct mt6797_dvfsp_efuse_identity *raw,
	struct mt6797_dvfsp_ptp_identity *identity);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PTP_H */
