// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Gemini PDA Mainline Project */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mt6797-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs cam_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_CAM(_id, _name, _parent, _shift) \
	GATE_MTK(_id, _name, _parent, &cam_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr)

static const struct mtk_gate cam_clks[] = {
	GATE_CAM(CLK_CAM_CAMSV2, "cam_camsv2", "mm_sel", 11),
	GATE_CAM(CLK_CAM_CAMSV1, "cam_camsv1", "mm_sel", 10),
	GATE_CAM(CLK_CAM_CAMSV0, "cam_camsv0", "mm_sel", 9),
	GATE_CAM(CLK_CAM_SENINF, "cam_seninf", "mm_sel", 8),
	GATE_CAM(CLK_CAM_CAMTG, "cam_camtg", "camtg_sel", 7),
	GATE_CAM(CLK_CAM_CAMSYS, "cam_camsys", "mm_sel", 6),
	GATE_CAM(CLK_CAM_LARB2, "cam_larb2", "mm_sel", 0),
};

static const struct mtk_clk_desc cam_desc = {
	.clks = cam_clks,
	.num_clks = ARRAY_SIZE(cam_clks),
};

static const struct of_device_id of_match_clk_mt6797_cam[] = {
	{ .compatible = "mediatek,mt6797-camsys", .data = &cam_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6797_cam);

static struct platform_driver clk_mt6797_cam_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6797-cam",
		.of_match_table = of_match_clk_mt6797_cam,
	},
};
module_platform_driver(clk_mt6797_cam_drv);

MODULE_DESCRIPTION("MediaTek MT6797 camsys clocks driver");
MODULE_LICENSE("GPL");
