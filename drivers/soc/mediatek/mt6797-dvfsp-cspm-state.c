// SPDX-License-Identifier: GPL-2.0-only
/*
 * Decode the raw MT6797 CSPM live-state words recovered from the vendor
 * hybrid-DVFS owner. No register or regulator access occurs here.
 */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-dvfsp-cspm-state.h>

#define MT6797_CSPM_SW_F_MIN_MASK	GENMASK(3, 0)
#define MT6797_CSPM_SW_F_MAX_MASK	GENMASK(7, 4)
#define MT6797_CSPM_SW_PAUSE		BIT(13)
#define MT6797_CSPM_CLUSTER_EN		BIT(14)
#define MT6797_CSPM_F_CURR_MASK		GENMASK(3, 0)
#define MT6797_CSPM_V_CURR_MASK		GENMASK(14, 8)
#define MT6797_CSPM_VS_CURR_MASK	GENMASK(22, 16)

static int mt6797_dvfsp_cspm_decode_physical(
	u32 raw_swctrl, u32 raw_hwsta,
	struct mt6797_dvfsp_cspm_state_cluster *cluster)
{
	u32 firmware_opp;

	if (!cluster)
		return -EINVAL;

	firmware_opp = FIELD_GET(MT6797_CSPM_F_CURR_MASK, raw_hwsta);
	if (firmware_opp >= MT6797_DVFSP_CSPM_STATE_OPP_COUNT)
		return -EPROTO;

	cluster->raw_swctrl = raw_swctrl;
	cluster->raw_hwsta = raw_hwsta;
	cluster->firmware_opp = firmware_opp;
	cluster->software_opp =
		MT6797_DVFSP_CSPM_STATE_OPP_COUNT - 1 - firmware_opp;
	cluster->voltage_code = FIELD_GET(MT6797_CSPM_V_CURR_MASK,
						  raw_hwsta);
	cluster->vsram_code = FIELD_GET(MT6797_CSPM_VS_CURR_MASK,
						   raw_hwsta);
	cluster->ceiling_opp = FIELD_GET(MT6797_CSPM_SW_F_MAX_MASK,
						 raw_swctrl);
	cluster->floor_opp = FIELD_GET(MT6797_CSPM_SW_F_MIN_MASK,
						 raw_swctrl);
	cluster->state_flags = MT6797_DVFSP_CSPM_STATE_VALID |
		MT6797_DVFSP_CSPM_STATE_LIMITS_VALID;
	if (raw_swctrl & MT6797_CSPM_CLUSTER_EN)
		cluster->state_flags |= MT6797_DVFSP_CSPM_STATE_ON;
	if (raw_swctrl & MT6797_CSPM_SW_PAUSE)
		cluster->state_flags |= MT6797_DVFSP_CSPM_STATE_PAUSED;

	return 0;
}

int mt6797_dvfsp_cspm_vproc_code_to_uv(u32 code, u32 *voltage_uv)
{
	u32 vendor_voltage;

	if (!voltage_uv || code > 0x7f)
		return -EINVAL;

	/* EXTBUCK_VAL_TO_VOLT(), converted from mV*100 to microvolts. */
	vendor_voltage = 30000 + (code * 1000);
	*voltage_uv = vendor_voltage * 10;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_cspm_vproc_code_to_uv);

int mt6797_dvfsp_cspm_vsram_code_to_uv(u32 code, u32 *voltage_uv)
{
	u32 vendor_voltage;

	if (!voltage_uv || code > 0xf)
		return -EINVAL;

	/* PMIC_VAL_TO_VOLT(), including the vendor's special codes 0..2. */
	switch (code) {
	case 0:
		vendor_voltage = 105000;
		break;
	case 1:
		vendor_voltage = 60000;
		break;
	case 2:
		vendor_voltage = 70000;
		break;
	default:
		vendor_voltage = 90000 + ((code - 3) * 2500);
		break;
	}

	*voltage_uv = vendor_voltage * 10;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_cspm_vsram_code_to_uv);

int mt6797_dvfsp_cspm_state_decode(
	const struct mt6797_dvfsp_clock_readback *readback,
	struct mt6797_dvfsp_cspm_state *state)
{
	unsigned int i;
	int ret;

	if (!readback || !state ||
	    readback->abi != MT6797_DVFSP_CLOCK_BACKEND_ABI ||
	    readback->reserved || !readback->sample_generation)
		return -EPROTO;

	memset(state, 0, sizeof(*state));
	state->abi = MT6797_DVFSP_CSPM_STATE_ABI;
	state->sample_generation = readback->sample_generation;

	for (i = 0; i < MT6797_DVFSP_CSPM_STATE_PHYSICAL_COUNT; i++) {
		ret = mt6797_dvfsp_cspm_decode_physical(
			readback->cspm_swctrl[i], readback->cspm_hwsta[i],
			&state->cluster[i]);
		if (ret)
			return ret;
	}

	/* CCI has a current word but no physical-cluster limit word. */
	ret = mt6797_dvfsp_cspm_decode_physical(
		0, readback->cspm_hwsta[3], &state->cluster[3]);
	if (ret)
		return ret;
	state->cluster[3].state_flags = MT6797_DVFSP_CSPM_STATE_VALID;
	state->cluster[3].ceiling_opp = 0;
	state->cluster[3].floor_opp = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_cspm_state_decode);

MODULE_DESCRIPTION("MT6797 CSPM live-state readback decoder");
MODULE_LICENSE("GPL");
