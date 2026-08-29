// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pure conversion of the disabled MT6797 protected clock readbacks.
 *
 * The vendor formula is intentionally kept here as a conversion boundary:
 * 26 MHz parent, PCW bits 20:0, POSDIV bits 26:24, and the documented
 * ARMPLLDIV_CKDIV encodings. No hardware access is performed.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-state.h>

struct mt6797_dvfsp_clock_divider {
	u32 numerator;
	u32 denominator;
};

static int mt6797_dvfsp_clock_divider_decode(
	u32 selector, struct mt6797_dvfsp_clock_divider *divider)
{
	if (!divider)
		return -EINVAL;

	switch (selector) {
	case 8:
		*divider = (struct mt6797_dvfsp_clock_divider){ 1, 1 };
		break;
	case 9:
		*divider = (struct mt6797_dvfsp_clock_divider){ 3, 4 };
		break;
	case 10:
		*divider = (struct mt6797_dvfsp_clock_divider){ 1, 2 };
		break;
	case 11:
		*divider = (struct mt6797_dvfsp_clock_divider){ 1, 4 };
		break;
	case 17:
		*divider = (struct mt6797_dvfsp_clock_divider){ 4, 5 };
		break;
	case 18:
		*divider = (struct mt6797_dvfsp_clock_divider){ 3, 5 };
		break;
	case 19:
		*divider = (struct mt6797_dvfsp_clock_divider){ 2, 5 };
		break;
	case 20:
		*divider = (struct mt6797_dvfsp_clock_divider){ 1, 5 };
		break;
	case 25:
		*divider = (struct mt6797_dvfsp_clock_divider){ 5, 6 };
		break;
	case 26:
		*divider = (struct mt6797_dvfsp_clock_divider){ 4, 6 };
		break;
	case 27:
		*divider = (struct mt6797_dvfsp_clock_divider){ 3, 6 };
		break;
	case 28:
		*divider = (struct mt6797_dvfsp_clock_divider){ 2, 6 };
		break;
	case 29:
		*divider = (struct mt6797_dvfsp_clock_divider){ 1, 6 };
		break;
	default:
		return -EPROTO;
	}

	return 0;
}

static int mt6797_dvfsp_clock_frequency_decode(
	u32 pll_pcw, u32 posdiv, u32 divider_selector, u32 *frequency_khz)
{
	struct mt6797_dvfsp_clock_divider divider;
	u64 frequency;
	int ret;

	if (!frequency_khz || !pll_pcw || posdiv > 2)
		return -EPROTO;

	ret = mt6797_dvfsp_clock_divider_decode(divider_selector, &divider);
	if (ret)
		return ret;

	/* Match the vendor integer order: ((PCW * 26) >> 14) * 1000. */
	frequency = ((u64)pll_pcw * MT6797_DVFSP_CLOCK_STATE_PARENT_MHZ) >> 14;
	frequency *= 1000;
	if (posdiv == 1)
		frequency = div_u64(frequency, 2);
	else if (posdiv == 2)
		frequency = div_u64(frequency, 4);
	frequency = div_u64(frequency * divider.numerator,
				divider.denominator);
	if (!frequency || frequency > U32_MAX)
		return -ERANGE;

	*frequency_khz = (u32)frequency;
	return 0;
}

static int mt6797_dvfsp_clock_cluster_decode(
	u32 con1, u32 mux, u32 divider_selector,
	struct mt6797_dvfsp_clock_state_cluster *cluster)
{
	int ret;

	if (!cluster || con1 & MT6797_DVFSP_CLOCK_STATE_PLL_CHANGE)
		return -EAGAIN;

	cluster->mux_selector = mux;
	cluster->divider_selector = divider_selector;
	cluster->pll_pcw = con1 & MT6797_DVFSP_CLOCK_STATE_PCW_MASK;
	cluster->posdiv = FIELD_GET(MT6797_DVFSP_CLOCK_STATE_POSDIV_MASK,
					con1);
	ret = mt6797_dvfsp_clock_frequency_decode(
		cluster->pll_pcw, cluster->posdiv, divider_selector,
		&cluster->frequency_khz);
	return ret;
}

int mt6797_dvfsp_clock_state_decode(
	const struct mt6797_dvfsp_clock_readback *clock,
	const struct mt6797_bigidvfs_readback *big,
	struct mt6797_dvfsp_clock_state *state)
{
	int ret;

	if (!clock || !big || !state ||
	    clock->abi != MT6797_DVFSP_CLOCK_BACKEND_ABI ||
	    big->abi != MT6797_BIGIDVFS_BACKEND_ABI ||
	    clock->reserved || big->reserved ||
	    !clock->sample_generation || !big->sample_generation)
		return -EPROTO;

	memset(state, 0, sizeof(*state));
	state->abi = MT6797_DVFSP_CLOCK_STATE_ABI;
	state->clock_sample_generation = clock->sample_generation;
	state->big_sample_generation = big->sample_generation;

	ret = mt6797_dvfsp_clock_cluster_decode(
		clock->pll_ll[1], FIELD_GET(GENMASK(3, 2),
				clock->armplldiv_muxsel), FIELD_GET(GENMASK(9, 5),
				clock->armplldiv_ckdiv), &state->cluster[
				MT6797_DVFSP_CLOCK_STATE_CLUSTER_LL]);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_clock_cluster_decode(
		clock->pll_l[1], FIELD_GET(GENMASK(5, 4),
				clock->armplldiv_muxsel), FIELD_GET(GENMASK(14, 10),
				clock->armplldiv_ckdiv), &state->cluster[
				MT6797_DVFSP_CLOCK_STATE_CLUSTER_L]);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_clock_cluster_decode(
		(big->pll_pcw & MT6797_DVFSP_CLOCK_STATE_PCW_MASK) |
			(big->pll_enable_posdiv & MT6797_DVFSP_CLOCK_STATE_POSDIV_MASK),
		FIELD_GET(GENMASK(1, 0),
				clock->armplldiv_muxsel), FIELD_GET(GENMASK(4, 0),
				clock->armplldiv_ckdiv), &state->cluster[
				MT6797_DVFSP_CLOCK_STATE_CLUSTER_B]);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_clock_cluster_decode(
		clock->pll_cci[1], FIELD_GET(GENMASK(7, 6),
				clock->armplldiv_muxsel), FIELD_GET(GENMASK(19, 15),
				clock->armplldiv_ckdiv), &state->cluster[
				MT6797_DVFSP_CLOCK_STATE_CLUSTER_CCI]);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_clock_state_decode);
