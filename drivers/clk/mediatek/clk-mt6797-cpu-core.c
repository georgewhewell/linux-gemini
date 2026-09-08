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
	       address == MT6797_CPUCLK_MP2_PLL_CON1;
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

static bool mt6797_cpuclk_state_valid(const struct mt6797_cpuclk_state *state)
{
	return state && state->source < MT6797_CPUCLK_NR_SOURCES &&
	       mt6797_cpuclk_divider_valid(state->divider) &&
	       state->armpll_rate;
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
	 * Admit only the complete cold tuple observed on Gemini, not arbitrary
	 * malformed PLL state or a zero-rate PLL selected by the mux.
	 */
	if (domain == MT6797_CPUCLK_MP2 &&
	    state->source == MT6797_CPUCLK_CLK26M && !state->divider &&
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
	default:
		return NULL;
	}

	for (i = 0; i < count; i++)
		if (rates[i].rate == rate)
			return &rates[i];

	return NULL;
}

int mt6797_cpuclk_plan_pll_rate(enum mt6797_cpuclk_domain domain,
				unsigned long rate,
				unsigned long parent_rate, u32 initial_con1,
				u32 *target_con1, unsigned long *actual_rate)
{
	const struct mt6797_cpuclk_rate_data *rate_data;
	u64 pcw;

	if (!parent_rate || !target_con1 || !actual_rate)
		return -EINVAL;

	rate_data = mt6797_cpuclk_find_rate(domain, rate);
	if (!rate_data)
		return -EINVAL;

	pcw = (u64)rate << (14 + rate_data->postdiv);
	pcw = div_u64(pcw, parent_rate);
	if (!pcw || pcw > MT6797_ARMPLL_PCW_MASK)
		return -ERANGE;

	*target_con1 = initial_con1 &
		~(MT6797_ARMPLL_PCW_MASK | MT6797_ARMPLL_POSDIV_MASK);
	*target_con1 |= FIELD_PREP(MT6797_ARMPLL_POSDIV_MASK,
				  rate_data->postdiv) | (u32)pcw | BIT(31);
	*actual_rate = (u64)parent_rate * pcw >> 14;
	*actual_rate >>= rate_data->postdiv;

	return 0;
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

int mt6797_cpuclk_build_rate_plan(enum mt6797_cpuclk_domain domain,
				  const struct mt6797_cpuclk_snapshot *snapshot,
				  unsigned long clk26m_rate,
				  const struct mt6797_cpuclk_state *target,
				  struct mt6797_cpuclk_rate_plan *plan)
{
	struct mt6797_cpuclk_state initial;
	int ret;

	if (!snapshot || !mt6797_cpuclk_state_valid(target) || !plan)
		return -EINVAL;

	ret = mt6797_cpuclk_decode_state(domain, snapshot, clk26m_rate,
					 &initial);
	if (ret)
		return ret;

	/* Standard mediatek-cpufreq reparents to MAINPLL before set_rate. */
	if (initial.source != MT6797_CPUCLK_MAINPLL || initial.divider != 8 ||
	    target->source != initial.source ||
	    target->divider != initial.divider)
		return -EOPNOTSUPP;

	plan->domain = domain;
	plan->clk26m_rate = clk26m_rate;
	plan->initial = initial;
	plan->target = *target;
	plan->rollback = initial;
	plan->rollback_snapshot = *snapshot;
	plan->intermediate_source = MT6797_CPUCLK_MAINPLL;

	return 0;
}

int mt6797_cpuclk_get_rollback_state(const struct mt6797_cpuclk_rate_plan *plan,
				     struct mt6797_cpuclk_state *rollback)
{
	struct mt6797_cpuclk_state decoded;
	int ret;

	if (!plan || !rollback || !mt6797_cpuclk_domain_valid(plan->domain) ||
	    plan->intermediate_source != MT6797_CPUCLK_MAINPLL ||
	    !mt6797_cpuclk_state_valid(&plan->initial) ||
	    !mt6797_cpuclk_state_valid(&plan->target) ||
	    plan->initial.source != MT6797_CPUCLK_MAINPLL ||
	    plan->initial.divider != 8 ||
	    plan->target.source != plan->initial.source ||
	    plan->target.divider != plan->initial.divider ||
	    plan->rollback.armpll_rate != plan->initial.armpll_rate ||
	    plan->rollback.source != plan->initial.source ||
	    plan->rollback.divider != plan->initial.divider)
		return -EINVAL;

	ret = mt6797_cpuclk_decode_state(plan->domain, &plan->rollback_snapshot,
					 plan->clk26m_rate, &decoded);
	if (ret || decoded.armpll_rate != plan->rollback.armpll_rate ||
	    decoded.source != plan->rollback.source ||
	    decoded.divider != plan->rollback.divider)
		return -EINVAL;

	*rollback = plan->rollback;
	return 0;
}

int mt6797_cpuclk_apply_rate_plan(struct mt6797_cpuclk_backend *backend,
				  const struct mt6797_cpuclk_rate_plan *plan)
{
	struct mt6797_cpuclk_state rollback;
	int ret;

	ret = mt6797_cpuclk_get_rollback_state(plan, &rollback);
	if (ret)
		return ret;

	if (!backend || !backend->ops || !backend->ops->apply)
		return -EOPNOTSUPP;

	return backend->ops->apply(backend->context, plan);
}
