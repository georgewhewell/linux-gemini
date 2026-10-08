// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>

#include "clk-mt6797-cpu.h"

#define MT6797_ARMPLL_PCW_MASK GENMASK(20, 0)
#define MT6797_ARMPLL_POSDIV_MASK GENMASK(26, 24)
#define MT6797_MP2_ARMPLL_PCW_MASK GENMASK(30, 0)
#define MT6797_MP2_ARMPLL_POSDIV_MASK GENMASK(14, 12)

struct mt6797_cpuclk_domain_data {
	u8 mux_shift;
	u8 divider_shift;
};

struct mt6797_cpuclk_rate_data {
	unsigned long rate;
	u8 postdiv;
};

static const struct mt6797_cpuclk_domain_data mt6797_cpuclk_domains[] = {
	[MT6797_CPUCLK_MP0] = { .mux_shift = 2, .divider_shift = 5 },
	[MT6797_CPUCLK_MP1] = { .mux_shift = 4, .divider_shift = 10 },
	[MT6797_CPUCLK_MP2] = { .mux_shift = 0, .divider_shift = 0 },
	[MT6797_CPUCLK_CCI] = { .mux_shift = 6, .divider_shift = 15 },
};

/* Vendor CPU_LEVEL_1 rates whose path divider remains one. */
static const struct mt6797_cpuclk_rate_data mt6797_mp0_rates[] = {
	{ 624000000, 1 },
	{ 715000000, 1 },
	{ 806000000, 1 },
	{ 897000000, 1 },
	{ 1014000000, 1 },
	{ 1118000000, 0 },
	{ 1222000000, 0 },
	{ 1274000000, 0 },
	{ 1339000000, 0 },
	{ 1391000000, 0 },
	{ 1443000000, 0 },
	{ 1495000000, 0 },
	{ 1547000000, 0 },
};

static const struct mt6797_cpuclk_rate_data mt6797_mp1_rates[] = {
	{ 650000000, 1 },
	{ 832000000, 1 },
	{ 962000000, 1 },
	{ 1092000000, 0 },
	{ 1209000000, 0 },
	{ 1352000000, 0 },
	{ 1495000000, 0 },
	{ 1625000000, 0 },
	{ 1703000000, 0 },
	{ 1755000000, 0 },
	{ 1820000000, 0 },
	{ 1885000000, 0 },
	{ 1950000000, 0 },
	{ 2002000000, 0 },
};

/* Vendor CPU_LEVEL_1 CCI points with a unity path divider. */
static const struct mt6797_cpuclk_rate_data mt6797_cci_rates[] = {
	{ 611000000, 1 },
	{ 676000000, 1 },
	{ 754000000, 1 },
	{ 819000000, 1 },
	{ 845000000, 1 },
	{ 884000000, 1 },
	{ 910000000, 1 },
	{ 936000000, 1 },
	{ 975000000, 1 },
	{ 988000000, 1 },
};

static bool mt6797_cpuclk_domain_valid(enum mt6797_cpuclk_domain domain)
{
	return domain >= MT6797_CPUCLK_MP0 && domain < MT6797_CPUCLK_NR_DOMAINS;
}

bool mt6797_cpuclk_secure_read_allowed(unsigned long fid, u32 address)
{
	if (fid != MT6797_CPUCLK_SMC_REG_READ)
		return false;

	return address == MT6797_CPUCLK_MP2_PLL_CON0 ||
	       address == MT6797_CPUCLK_MP2_PLL_CON1 ||
	       address == MT6797_CPUCLK_MP2_SYNC_DCM;
}

bool mt6797_cpuclk_snapshots_equal(const struct mt6797_cpuclk_snapshot *first,
				   const struct mt6797_cpuclk_snapshot *second)
{
	return first && second && !memcmp(first, second, sizeof(*first));
}

static bool mt6797_cpuclk_divider_valid(u8 divider)
{
	switch (divider) {
	case 0:
	case 8:
	case 9:
	case 10:
	case 11:
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
	case 24:
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
		return true;
	default:
		return false;
	}
}

static int
mt6797_cpuclk_decode_armpll(enum mt6797_cpuclk_domain domain,
			    const struct mt6797_cpuclk_snapshot *snapshot,
			    unsigned long clk26m_rate, u64 *rate)
{
	u32 con1, pcw, posdiv;

	if (domain == MT6797_CPUCLK_MP2) {
		pcw = FIELD_GET(MT6797_MP2_ARMPLL_PCW_MASK,
				snapshot->mp2_armpll_con1);
		posdiv = FIELD_GET(MT6797_MP2_ARMPLL_POSDIV_MASK,
				   snapshot->mp2_armpll_con0);
		*rate = (u64)clk26m_rate * pcw >> 24;
	} else {
		con1 = snapshot->armpll_con1[domain];
		pcw = FIELD_GET(MT6797_ARMPLL_PCW_MASK, con1);
		posdiv = FIELD_GET(MT6797_ARMPLL_POSDIV_MASK, con1);
		if (posdiv > 2)
			return -ERANGE;

		*rate = (u64)clk26m_rate * pcw >> 14;
	}

	*rate >>= posdiv;
	return *rate ? 0 : -ERANGE;
}

int mt6797_cpuclk_decode_state(enum mt6797_cpuclk_domain domain,
			       const struct mt6797_cpuclk_snapshot *snapshot,
			       unsigned long clk26m_rate,
			       struct mt6797_cpuclk_state *state)
{
	const struct mt6797_cpuclk_domain_data *data;
	int ret;

	if (!mt6797_cpuclk_domain_valid(domain) || !snapshot || !clk26m_rate ||
	    !state)
		return -EINVAL;

	data = &mt6797_cpuclk_domains[domain];
	state->source = (snapshot->muxsel >> data->mux_shift) & 0x3;
	state->divider = (snapshot->clkdiv >> data->divider_shift) & 0x1f;

	if (!mt6797_cpuclk_divider_valid(state->divider))
		return -ERANGE;

	/*
	 * The offline MP2 path uses clk26m with an uninitialized, unselected
	 * PLL. Secure PSCI owns initial PLL/mux programming during CPU_ON.
	 * Preserve the zero PLL observation; it is not a synthesized rate.
	 * Rail removal also clears both PLL words, while the divider survives.
	 * Decode that reference-clock path with any valid divider. Cold/restart
	 * admission separately checks the divider and shutdown ownership; neither
	 * malformed PLL state nor a zero-rate selected PLL is accepted here.
	 */
	if (domain == MT6797_CPUCLK_MP2 &&
	    state->source == MT6797_CPUCLK_CLK26M &&
	    !snapshot->mp2_armpll_con0 && !snapshot->mp2_armpll_con1) {
		state->armpll_rate = 0;
		return 0;
	}

	ret = mt6797_cpuclk_decode_armpll(domain, snapshot, clk26m_rate,
					  &state->armpll_rate);
	if (ret)
		return ret;

	return 0;
}

/* A pre-CPU_ON contract, not a statement about clocks after firmware starts MP2. */
int mt6797_cpuclk_validate_cold_mp2(const struct mt6797_cpuclk_snapshot *snapshot,
				 unsigned long reference_rate)
{
	struct mt6797_cpuclk_state state;
	int ret;

	if (reference_rate != 26000000)
		return -ERANGE;
	ret = mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2, snapshot,
					 reference_rate, &state);
	if (ret)
		return ret;
	if (state.source != MT6797_CPUCLK_CLK26M || state.divider ||
	    state.armpll_rate || snapshot->mp2_armpll_con0 ||
	    snapshot->mp2_armpll_con1)
		return -EBUSY;
	return 0;
}

/*
 * ATF power_off_cl3() clears MP2 mux bit 0 and PLL enable bit 0, retaining
 * the PLL programming and divider. Compare against the owner's pre-off
 * snapshot, not an assumed power-on default. Removing BUCKB then clears
 * both PLL words on Gemini, while the mux and divider survive. Accept that
 * complete reset pair as well as exact retention, never a partial reset.
 * Other clusters can change.
 * This proves only the clock transition; power/SRAM state and ownership of
 * the shutdown receipt must be checked separately before authorizing CPU_ON.
 */
int mt6797_cpuclk_validate_stopped_mp2(
		const struct mt6797_cpuclk_snapshot *running,
		const struct mt6797_cpuclk_snapshot *stopped,
		unsigned long reference_rate)
{
	struct mt6797_cpuclk_state state;
	int ret;

	if (!running || !stopped)
		return -EINVAL;
	if (reference_rate != 26000000)
		return -ERANGE;
	ret = mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2, running,
					 reference_rate, &state);
	if (ret)
		return ret;
	if (state.source != MT6797_CPUCLK_ARMPLL ||
	    !(running->mp2_armpll_con0 & BIT(0)))
		return -EBUSY;
	if ((stopped->muxsel & GENMASK(1, 0)) != MT6797_CPUCLK_CLK26M ||
	    (stopped->clkdiv & GENMASK(4, 0)) != state.divider)
		return -EBUSY;
	if (!stopped->mp2_armpll_con0 && !stopped->mp2_armpll_con1)
		return 0;
	if (stopped->mp2_armpll_con0 != (running->mp2_armpll_con0 & ~BIT(0)) ||
	    stopped->mp2_armpll_con1 != running->mp2_armpll_con1)
		return -EBUSY;
	return 0;
}

unsigned long mt6797_cpuclk_apply_divider(unsigned long parent_rate, u8 divider)
{
	u8 numerator, denominator;

	switch (divider) {
	case 0:
	case 8:
	case 16:
	case 24:
		return parent_rate;
	case 9:
		numerator = 3;
		denominator = 4;
		break;
	case 10:
		numerator = 1;
		denominator = 2;
		break;
	case 11:
		numerator = 1;
		denominator = 4;
		break;
	case 17:
	case 18:
	case 19:
	case 20:
		numerator = 21 - divider;
		denominator = 5;
		break;
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
		numerator = 30 - divider;
		denominator = 6;
		break;
	default:
		return 0;
	}

	return mult_frac(parent_rate, numerator, denominator);
}

static const struct mt6797_cpuclk_rate_data *
mt6797_cpuclk_find_rate(enum mt6797_cpuclk_domain domain, unsigned long rate)
{
	const struct mt6797_cpuclk_rate_data *rates;
	unsigned int count, i;

	switch (domain) {
	case MT6797_CPUCLK_MP0:
		rates = mt6797_mp0_rates;
		count = ARRAY_SIZE(mt6797_mp0_rates);
		break;
	case MT6797_CPUCLK_MP1:
		rates = mt6797_mp1_rates;
		count = ARRAY_SIZE(mt6797_mp1_rates);
		break;
	case MT6797_CPUCLK_CCI:
		rates = mt6797_cci_rates;
		count = ARRAY_SIZE(mt6797_cci_rates);
		break;
	default:
		return NULL;
	}

	for (i = 0; i < count; i++)
		if (rates[i].rate == rate)
			return &rates[i];

	return NULL;
}

static int mt6797_cpuclk_encode_pll_rate(unsigned long rate,
		unsigned long parent_rate, u8 postdiv, u32 initial_con1,
		u32 *target_con1, unsigned long *actual_rate)
{
	u64 pcw;

	if (!parent_rate || !target_con1 || !actual_rate)
		return -EINVAL;

	pcw = (u64)rate << (14 + postdiv);
	pcw = div_u64(pcw, parent_rate);
	if (!pcw || pcw > MT6797_ARMPLL_PCW_MASK)
		return -ERANGE;

	*target_con1 = initial_con1 &
		~(MT6797_ARMPLL_PCW_MASK | MT6797_ARMPLL_POSDIV_MASK);
	*target_con1 |= FIELD_PREP(MT6797_ARMPLL_POSDIV_MASK, postdiv) |
		       (u32)pcw | BIT(31);
	*actual_rate = (u64)parent_rate * pcw >> 14;
	*actual_rate >>= postdiv;

	return 0;
}

int mt6797_cpuclk_plan_pll_rate(enum mt6797_cpuclk_domain domain,
				unsigned long rate,
				unsigned long parent_rate, u32 initial_con1,
				u32 *target_con1, unsigned long *actual_rate)
{
	const struct mt6797_cpuclk_rate_data *rate_data;

	rate_data = mt6797_cpuclk_find_rate(domain, rate);
	if (!rate_data)
		return -EINVAL;

	return mt6797_cpuclk_encode_pll_rate(rate, parent_rate,
					   rate_data->postdiv, initial_con1,
					   target_con1, actual_rate);
}

int mt6797_cpuclk_plan_mux(enum mt6797_cpuclk_domain domain, u8 source,
			   u32 initial_mux, u32 *target_mux)
{
	const struct mt6797_cpuclk_domain_data *data;
	u32 mask;

	if (!mt6797_cpuclk_domain_valid(domain) ||
	    source >= MT6797_CPUCLK_NR_SOURCES || !target_mux)
		return -EINVAL;

	data = &mt6797_cpuclk_domains[domain];
	mask = GENMASK(data->mux_shift + 1, data->mux_shift);
	*target_mux = (initial_mux & ~mask) | (source << data->mux_shift);

	return 0;
}

int mt6797_cpuclk_plan_dcm(enum mt6797_cpuclk_domain domain,
			   unsigned long rate, u32 initial_dcm,
			   struct mt6797_cpuclk_dcm_plan *plan)
{
	u32 divider, mask, toggle, shift;

	if (!rate || !plan)
		return -EINVAL;

	switch (domain) {
	case MT6797_CPUCLK_MP0:
		shift = 9;
		toggle = BIT(14);
		break;
	case MT6797_CPUCLK_MP1:
		shift = 17;
		toggle = BIT(22);
		break;
	case MT6797_CPUCLK_CCI:
		shift = 1;
		toggle = BIT(6);
		break;
	default:
		return -EOPNOTSUPP;
	}

	divider = rate / 70000000;
	divider = min(divider, 31U);
	mask = GENMASK(shift + 4, shift);
	plan->value[0] = (initial_dcm & ~mask) | (divider << shift);
	plan->value[1] = plan->value[0] & ~toggle;
	plan->value[2] = plan->value[1] | toggle;

	return 0;
}

static bool mt6797_cpuclk_divider_is_unity(u8 divider)
{
	return !(divider & 0x7);
}

/*
 * Vendor LL CPU_LEVEL_1 low points keep POS=2 and use CKDIV=2 or 4.
 * Increasing division precedes FHCTL; decreasing division follows it.
 * Unlike a unity-divider hop, the intermediate CPU rate can fall below both
 * endpoints. Prepare DCM for that minimum, then program the final selector.
 * For a postdivider change, use CKDIV=4 while on MAINPLL and programming
 * the disconnected ARMPLL. Even the fallback CPU rate must stay below the
 * larger endpoint, independently of the current regulator voltage. DCM
 * covers both divided ARMPLL endpoints and the temporary MAINPLL rate.
 */
int mt6797_cpuclk_plan_transition_mainpll(enum mt6797_cpuclk_domain domain,
			   const struct mt6797_cpuclk_snapshot *snapshot,
			   unsigned long clk26m_rate, unsigned long rate,
			   unsigned long mainpll_rate,
			   struct mt6797_cpuclk_transition_plan *plan)
{
	struct mt6797_cpuclk_state initial;
	unsigned long pll_rate, minimum, before_hop, after_hop;
	u32 con1, target_con1, shift, dcm_shift, dcm_limit;
	u8 old_div, new_div = 1, selector;
	bool cci_down;
	int ret;

	if (!snapshot || !plan || !clk26m_rate)
		return -EINVAL;
	if (domain != MT6797_CPUCLK_MP0 && domain != MT6797_CPUCLK_MP1 &&
	    domain != MT6797_CPUCLK_CCI)
		return -EOPNOTSUPP;
	ret = mt6797_cpuclk_decode_state(domain, snapshot, clk26m_rate,
					 &initial);
	if (ret)
		return ret;
	if (initial.source != MT6797_CPUCLK_ARMPLL)
		return -EOPNOTSUPP;
	if (mt6797_cpuclk_divider_is_unity(initial.divider))
		old_div = 1;
	else if (initial.divider == 10)
		old_div = 2;
	else if (initial.divider == 11)
		old_div = 4;
	else
		return -EOPNOTSUPP;

	con1 = snapshot->armpll_con1[domain];
	if (domain == MT6797_CPUCLK_MP0 &&
	    (rate == 221000000 || rate == 338000000 || rate == 481000000)) {
		new_div = rate == 221000000 ? 4 : 2;
		ret = mt6797_cpuclk_encode_pll_rate(rate * new_div,
				clk26m_rate, 1, con1, &target_con1, &pll_rate);
	} else if (domain == MT6797_CPUCLK_MP1 &&
		   (rate == 325000000 || rate == 468000000)) {
		new_div = 2;
		ret = mt6797_cpuclk_encode_pll_rate(rate * new_div,
				clk26m_rate, 1, con1, &target_con1, &pll_rate);
	} else if (domain == MT6797_CPUCLK_CCI &&
		   (rate == 169000000 || rate == 234000000 ||
		    rate == 325000000 || rate == 416000000 ||
		    rate == 481000000 || rate == 533000000)) {
		new_div = rate <= 234000000 ? 4 : 2;
		ret = mt6797_cpuclk_encode_pll_rate(rate * new_div,
				clk26m_rate, 1, con1, &target_con1, &pll_rate);
	} else {
		ret = mt6797_cpuclk_plan_pll_rate(domain, rate, clk26m_rate,
						con1, &target_con1, &pll_rate);
	}
	if (ret)
		return ret;
	plan->changes_postdiv = !!((con1 ^ target_con1) & MT6797_ARMPLL_POSDIV_MASK);
	if (plan->changes_postdiv && !mainpll_rate)
		return -EOPNOTSUPP;
	plan->target_con1 = target_con1;
	plan->mainpll_rate = mainpll_rate;
	plan->temporary_clkdiv = snapshot->clkdiv;
	plan->mainpll_mux = snapshot->muxsel;
	plan->mux_mask = 0;

	selector = new_div == 4 ? 11 : new_div == 2 ? 10 : 8;
	/* Preserve the firmware's equivalent unity encoding if unchanged. */
	if (new_div == 1 && old_div == 1)
		selector = initial.divider;
	shift = mt6797_cpuclk_domains[domain].divider_shift;
	plan->clkdiv_mask = GENMASK(shift + 4, shift);
	plan->clkdiv = (snapshot->clkdiv & ~plan->clkdiv_mask) |
		      ((u32)selector << shift);
	plan->divider_before_hop = new_div > old_div;
	plan->initial_rate = initial.armpll_rate / old_div;
	plan->rate = pll_rate / new_div;
	plan->pll_rate = pll_rate;
	plan->dds = FIELD_GET(MT6797_ARMPLL_PCW_MASK, target_con1);
	before_hop = initial.armpll_rate / (new_div > old_div ? new_div : old_div);
	after_hop = pll_rate / (new_div > old_div ? new_div : old_div);
	minimum = min(before_hop, after_hop);
	if (plan->changes_postdiv) {
		unsigned long ceiling = plan->initial_rate > plan->rate ?
					plan->initial_rate : plan->rate;

		if (mainpll_rate / 4 > ceiling || mainpll_rate < 4)
			return -ERANGE;
		plan->temporary_clkdiv = (snapshot->clkdiv & ~plan->clkdiv_mask) |
					(11U << shift);
		ret = mt6797_cpuclk_plan_mux(domain, MT6797_CPUCLK_MAINPLL,
					    snapshot->muxsel, &plan->mainpll_mux);
		if (ret)
			return ret;
		shift = mt6797_cpuclk_domains[domain].mux_shift;
		plan->mux_mask = GENMASK(shift + 1, shift);
		before_hop = initial.armpll_rate / 4;
		after_hop = pll_rate / 4;
		minimum = min(before_hop, after_hop);
		minimum = min(minimum, mainpll_rate / 4);
	}
	plan->minimum_rate = minimum;

	/* Never raise the initial DCM selector before changing the CPU clock. */
	dcm_shift = domain == MT6797_CPUCLK_CCI ? 1 :
		    domain == MT6797_CPUCLK_MP0 ? 9 : 17;
	/* Vendor CCI downshifts disable synchronous division first. */
	cci_down = domain == MT6797_CPUCLK_CCI && plan->rate < plan->initial_rate;
	if (cci_down)
		minimum = 1;
	dcm_limit = (snapshot->sync_dcm >> dcm_shift) & 0x1f;
	ret = mt6797_cpuclk_plan_dcm(domain, minimum, snapshot->sync_dcm,
				     &plan->before);
	if (ret)
		return ret;
	if (dcm_limit < ((plan->before.value[0] >> dcm_shift) & 0x1f)) {
		/* Any positive rate below70 MHz encodes selector zero. */
		ret = mt6797_cpuclk_plan_dcm(domain,
				 dcm_limit ? dcm_limit * 70000000UL : 1,
				 snapshot->sync_dcm, &plan->before);
		if (ret)
			return ret;
	}
	ret = mt6797_cpuclk_plan_dcm(domain, cci_down ? 1 : plan->rate,
				     plan->before.value[2], &plan->after);
	if (ret)
		return ret;
	plan->domain = domain;
	plan->clk26m_rate = clk26m_rate;
	plan->snapshot = *snapshot;
	return 0;
}

/* Existing callers cannot accidentally request an unowned MAINPLL handoff. */
int mt6797_cpuclk_plan_transition(enum mt6797_cpuclk_domain domain,
			   const struct mt6797_cpuclk_snapshot *snapshot,
			   unsigned long clk26m_rate, unsigned long rate,
			   struct mt6797_cpuclk_transition_plan *plan)
{
	return mt6797_cpuclk_plan_transition_mainpll(domain, snapshot,
					 clk26m_rate, rate, 0, plan);
}
