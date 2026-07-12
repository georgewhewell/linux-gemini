// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Gemini PDA Mainline Project */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mt6797-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs mjc_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_MJC(_id, _name, _shift) \
	GATE_MTK(_id, _name, "mjc_sel", &mjc_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mjc_clks[] = {
	GATE_MJC(CLK_MJC_SMI_LARB, "mjc_smi_larb", 0),
	GATE_MJC(CLK_MJC_TOP_CLK_0, "mjc_top_clk_0", 1),
	GATE_MJC(CLK_MJC_TOP_CLK_1, "mjc_top_clk_1", 2),
	GATE_MJC(CLK_MJC_TOP_CLK_2, "mjc_top_clk_2", 3),
	GATE_MJC(CLK_MJC_FAKE_ENGINE, "mjc_fake_engine", 4),
	GATE_MJC(CLK_MJC_LARB4_ASIF, "mjc_larb4_asif", 5),
};

static const struct mtk_clk_desc mjc_desc = {
	.clks = mjc_clks,
	.num_clks = ARRAY_SIZE(mjc_clks),
};

static const struct of_device_id of_match_clk_mt6797_mjc[] = {
	{ .compatible = "mediatek,mt6797-mjcsys", .data = &mjc_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6797_mjc);

static struct platform_driver clk_mt6797_mjc_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6797-mjc",
		.of_match_table = of_match_clk_mt6797_mjc,
	},
};
module_platform_driver(clk_mt6797_mjc_drv);

MODULE_DESCRIPTION("MediaTek MT6797 mjcsys clocks driver");
MODULE_LICENSE("GPL");
