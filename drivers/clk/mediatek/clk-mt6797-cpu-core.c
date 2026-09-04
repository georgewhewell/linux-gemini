// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/kernel.h>
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

static const struct mt6797_cpuclk_domain_data mt6797_cpuclk_domains[] = {
	[MT6797_CPUCLK_MP0] = { .mux_shift = 2, .divider_shift = 5 },
	[MT6797_CPUCLK_MP1] = { .mux_shift = 4, .divider_shift = 10 },
	[MT6797_CPUCLK_MP2] = { .mux_shift = 0, .divider_shift = 0 },
	[MT6797_CPUCLK_CCI] = { .mux_shift = 6, .divider_shift = 15 },
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

	ret = mt6797_cpuclk_decode_armpll(domain, snapshot, clk26m_rate,
					  &state->armpll_rate);
	if (ret)
		return ret;

	return 0;
}

unsigned long mt6797_cpuclk_apply_divider(unsigned long parent_rate, u8 divider)
{
	u8 numerator, denominator;

	switch (divider) {
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
