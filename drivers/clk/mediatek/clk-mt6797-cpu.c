// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 George Whewell
 */

#include <linux/clk-provider.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/clock/mt6797-clk.h>

#include "clk-mt6797-cpu.h"

struct mt6797_cpuclk;

struct mt6797_cpuclk_pll_hw {
	struct clk_hw hw;
	struct mt6797_cpuclk *provider;
	enum mt6797_cpuclk_domain domain;
};

struct mt6797_cpuclk_path_hw {
	struct clk_hw hw;
	struct mt6797_cpuclk *provider;
	enum mt6797_cpuclk_domain domain;
	struct clk_parent_data parents[MT6797_CPUCLK_NR_SOURCES];
};

struct mt6797_cpuclk {
	struct device *dev;
	struct mt6797_cpuclk_backend backend;
	struct mt6797_cpuclk_pll_hw plls[MT6797_CPUCLK_NR_DOMAINS];
	struct mt6797_cpuclk_path_hw paths[MT6797_CPUCLK_NR_DOMAINS];
	struct clk_hw_onecell_data *clk_data;
};

static const char *const mt6797_cpuclk_pll_names[] = {
	[MT6797_CPUCLK_MP0] = "mt6797_mp0_armpll",
	[MT6797_CPUCLK_MP1] = "mt6797_mp1_armpll",
	[MT6797_CPUCLK_MP2] = "mt6797_mp2_armpll",
	[MT6797_CPUCLK_CCI] = "mt6797_cci_armpll",
};

static const char *const mt6797_cpuclk_path_names[] = {
	[MT6797_CPUCLK_MP0] = "mt6797_mp0",
	[MT6797_CPUCLK_MP1] = "mt6797_mp1",
	[MT6797_CPUCLK_MP2] = "mt6797_mp2",
	[MT6797_CPUCLK_CCI] = "mt6797_cci",
};

static int mt6797_cpuclk_snapshot(struct mt6797_cpuclk *provider,
				  struct mt6797_cpuclk_snapshot *snapshot)
{
	if (!provider->backend.ops || !provider->backend.ops->snapshot)
		return -EOPNOTSUPP;

	return provider->backend.ops->snapshot(provider->backend.context,
					       snapshot);
}

static unsigned long mt6797_cpuclk_pll_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct mt6797_cpuclk_pll_hw *pll =
		container_of(hw, struct mt6797_cpuclk_pll_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;

	if (mt6797_cpuclk_snapshot(pll->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(pll->domain, &snapshot, parent_rate,
				       &state))
		return 0;

	return state.armpll_rate;
}

static const struct clk_ops mt6797_cpuclk_pll_ops = {
	.recalc_rate = mt6797_cpuclk_pll_recalc_rate,
};

static u8 mt6797_cpuclk_path_get_parent(struct clk_hw *hw)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m)
		return MT6797_CPUCLK_CLK26M;

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_snapshot(path->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(path->domain, &snapshot, clk26m_rate,
				       &state))
		return MT6797_CPUCLK_CLK26M;

	return state.source;
}

static unsigned long mt6797_cpuclk_path_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct mt6797_cpuclk_path_hw *path =
		container_of(hw, struct mt6797_cpuclk_path_hw, hw);
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_state state;
	struct clk_hw *clk26m;
	unsigned long clk26m_rate;

	clk26m = clk_hw_get_parent_by_index(hw, MT6797_CPUCLK_CLK26M);
	if (!clk26m)
		return 0;

	clk26m_rate = clk_hw_get_rate(clk26m);
	if (mt6797_cpuclk_snapshot(path->provider, &snapshot) ||
	    mt6797_cpuclk_decode_state(path->domain, &snapshot, clk26m_rate,
				       &state))
		return 0;

	if (state.source == MT6797_CPUCLK_ARMPLL)
		parent_rate = state.armpll_rate;

	return mt6797_cpuclk_apply_divider(parent_rate, state.divider);
}

/* Deliberately no set_parent, determine_rate, round_rate, or set_rate. */
static const struct clk_ops mt6797_cpuclk_path_ops = {
	.get_parent = mt6797_cpuclk_path_get_parent,
	.recalc_rate = mt6797_cpuclk_path_recalc_rate,
};

static int mt6797_cpuclk_register_plls(struct mt6797_cpuclk *provider)
{
	struct clk_parent_data parent = { .fw_name = "clk26m" };
	struct clk_init_data init = {
		.ops = &mt6797_cpuclk_pll_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_CPUCLK_NR_DOMAINS; i++) {
		provider->plls[i].provider = provider;
		provider->plls[i].domain = i;
		provider->plls[i].hw.init = &init;
		init.name = mt6797_cpuclk_pll_names[i];

		ret = devm_clk_hw_register(provider->dev,
					   &provider->plls[i].hw);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt6797_cpuclk_register_paths(struct mt6797_cpuclk *provider)
{
	struct clk_init_data init = {
		.ops = &mt6797_cpuclk_path_ops,
		.num_parents = MT6797_CPUCLK_NR_SOURCES,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_CPUCLK_NR_DOMAINS; i++) {
		struct mt6797_cpuclk_path_hw *path = &provider->paths[i];

		path->provider = provider;
		path->domain = i;
		path->parents[MT6797_CPUCLK_CLK26M].fw_name = "clk26m";
		path->parents[MT6797_CPUCLK_ARMPLL].hw = &provider->plls[i].hw;
		path->parents[MT6797_CPUCLK_MAINPLL].fw_name = "mainpll";
		path->parents[MT6797_CPUCLK_UNIVPLL].fw_name = "univpll";
		path->hw.init = &init;
		init.name = mt6797_cpuclk_path_names[i];
		init.parent_data = path->parents;

		ret = devm_clk_hw_register(provider->dev, &path->hw);
		if (ret)
			return ret;

		provider->clk_data->hws[i] = &path->hw;
	}

	return 0;
}

static int mt6797_cpuclk_register(struct mt6797_cpuclk *provider)
{
	int ret;

	provider->clk_data = devm_kzalloc(provider->dev,
					  struct_size(provider->clk_data, hws,
						      MT6797_CPUCLK_NR_DOMAINS),
					  GFP_KERNEL);
	if (!provider->clk_data)
		return -ENOMEM;

	provider->clk_data->num = MT6797_CPUCLK_NR_DOMAINS;

	ret = mt6797_cpuclk_register_plls(provider);
	if (ret)
		return ret;

	ret = mt6797_cpuclk_register_paths(provider);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(provider->dev, of_clk_hw_onecell_get,
					   provider->clk_data);
}

static int mt6797_cpuclk_backend_init(struct platform_device *pdev,
				      struct mt6797_cpuclk_backend *backend)
{
	/*
	 * A backend must take one serialized snapshot and own every future
	 * MCUMIXEDSYS/secure-firmware transition. No such backend is proved yet.
	 */
	return -EOPNOTSUPP;
}

static int mt6797_cpuclk_probe(struct platform_device *pdev)
{
	struct mt6797_cpuclk *provider;
	int ret;

	provider = devm_kzalloc(&pdev->dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;

	provider->dev = &pdev->dev;
	ret = mt6797_cpuclk_backend_init(pdev, &provider->backend);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "serialized clock backend is unavailable\n");
		return ret;
	}

	return mt6797_cpuclk_register(provider);
}

static const struct of_device_id mt6797_cpuclk_of_match[] = {
	{ .compatible = "mediatek,mt6797-mcumixedsys" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_cpuclk_of_match);

static struct platform_driver mt6797_cpuclk_driver = {
	.probe = mt6797_cpuclk_probe,
	.driver = {
		.name = "clk-mt6797-cpu",
		.of_match_table = mt6797_cpuclk_of_match,
	},
};
module_platform_driver(mt6797_cpuclk_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CPU clock provider foundation");
MODULE_LICENSE("GPL");
