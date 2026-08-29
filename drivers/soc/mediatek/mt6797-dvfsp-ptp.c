// SPDX-License-Identifier: GPL-2.0-only
/*
 * Decode the read-only MT6797 LK M_HW_RES detector handoff.
 *
 * The bit positions mirror the public MT6797 eem_devinfo structure. This
 * helper is deliberately pure: the caller owns the handoff storage and no
 * detector or hardware state is changed here.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>

static void mt6797_dvfsp_ptp_decode_bank(
	struct mt6797_dvfsp_ptp_bank_state *bank, u32 bank_id,
	u32 enable_word, u32 init_shift, u32 mon_shift,
	u32 dvfs_shift, u32 bin_shift)
{
	bank->bank = bank_id;
	bank->init_enable = (enable_word >> init_shift) & 0x1;
	bank->mon_enable = (enable_word >> mon_shift) & 0x1;
	bank->dvfs_level = (enable_word >> dvfs_shift) & 0x3;
	bank->bin_spec = (enable_word >> bin_shift) & 0x7;
}

int mt6797_dvfsp_ptp_decode(
	const struct mt6797_dvfsp_ptp_handoff *handoff,
	struct mt6797_dvfsp_ptp_state *state)
{
	if (!handoff || !state ||
	    handoff->abi != MT6797_DVFSP_PTP_HANDOFF_ABI)
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	state->abi = MT6797_DVFSP_PTP_STATE_ABI;
	state->bank_mask = MT6797_DVFSP_PTP_BANK_MASK_ALL;

	/* M_HW_RES1: BIG_INITEN/MONEN/DVFS_L/BIN_SPEC. */
	mt6797_dvfsp_ptp_decode_bank(&state->bank[0],
		MT6797_DVFSP_PTP_BANK_BIG, handoff->m_hw_res[1], 0, 1, 2, 5);
	/* M_HW_RES7: CPU_L_INITEN/MONEN/DVFS_L/BIN_SPEC. */
	mt6797_dvfsp_ptp_decode_bank(&state->bank[1],
		MT6797_DVFSP_PTP_BANK_L, handoff->m_hw_res[7], 0, 1, 2, 5);
	/* M_HW_RES9 low fields: CPU_2L_INITEN/MONEN/DVFS_L/BIN_SPEC. */
	mt6797_dvfsp_ptp_decode_bank(&state->bank[2],
		MT6797_DVFSP_PTP_BANK_2L, handoff->m_hw_res[9], 0, 1, 2, 5);
	/* M_HW_RES9 high fields: CCI_INITEN/MONEN/DVFS_L/BIN_SPEC. */
	mt6797_dvfsp_ptp_decode_bank(&state->bank[3],
		MT6797_DVFSP_PTP_BANK_CCI, handoff->m_hw_res[9], 8, 9, 10, 13);

	if (!state->bank[0].init_enable || !state->bank[0].mon_enable ||
	    !state->bank[1].init_enable || !state->bank[1].mon_enable ||
	    !state->bank[2].init_enable || !state->bank[2].mon_enable ||
	    !state->bank[3].init_enable || !state->bank[3].mon_enable)
		return -EAGAIN;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ptp_decode);

int mt6797_dvfsp_ptp_identity_decode(
	const struct mt6797_dvfsp_efuse_identity *raw,
	struct mt6797_dvfsp_ptp_identity *identity)
{
	u32 date_nibble;
	u32 function_code;

	if (!raw || !identity ||
	    raw->abi != MT6797_DVFSP_EFUSE_IDENTITY_ABI)
		return -EINVAL;

	date_nibble = (raw->date_word >> 4) & 0xf;
	function_code = raw->function_word & 0xf;

	memset(identity, 0, sizeof(*identity));
	identity->abi = MT6797_DVFSP_PTP_IDENTITY_ABI;
	identity->date_code = date_nibble < 7 ?
		MT6797_DVFSP_PTP_DATE_1221 : MT6797_DVFSP_PTP_DATE_0119;
	identity->function_code = function_code;

	switch (function_code) {
	case 0:
	case 3:
		identity->cpu_level = 0;
		break;
	case 1:
	case 6:
		identity->cpu_level = 1;
		break;
	case 2:
	case 7:
		identity->cpu_level = 2;
		break;
	case 0xf:
		identity->cpu_level = 3;
		break;
	case 4:
		identity->cpu_level = 1;
		identity->tt_segment = 1;
		break;
	default:
		identity->cpu_level = 0;
		break;
	}

	/* Stable local token derived only from the vendor-selected fields. */
	identity->variant_id = 1 + (identity->date_code << 8) +
		(identity->cpu_level << 4) + identity->tt_segment;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ptp_identity_decode);

MODULE_DESCRIPTION("MT6797 read-only PTP handoff decoder");
MODULE_LICENSE("GPL");
