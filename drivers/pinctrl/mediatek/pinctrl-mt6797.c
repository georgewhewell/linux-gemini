// SPDX-License-Identifier: GPL-2.0
/*
 * Based on pinctrl-mt6765.c
 *
 * Copyright (C) 2018 MediaTek Inc.
 *
 * Author: ZH Chen <zh.chen@mediatek.com>
 *
 * Copyright (C) Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 *
 */

#include "pinctrl-mtk-mt6797.h"
#include "pinctrl-paris.h"

/*
 * MT6797 have multiple bases to program pin configuration listed as the below:
 * gpio:0x10005000, iocfg[l]:0x10002000, iocfg[b]:0x10002400,
 * iocfg[r]:0x10002800, iocfg[t]:0x10002C00.
 * _i_base could be used to indicate what base the pin should be mapped into.
 */

static const struct mtk_pin_field_calc mt6797_pin_mode_range[] = {
	PIN_FIELD(0, 261, 0x300, 0x10, 0, 4),
};

static const struct mtk_pin_field_calc mt6797_pin_dir_range[] = {
	PIN_FIELD(0, 261, 0x0, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_di_range[] = {
	PIN_FIELD(0, 261, 0x200, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_do_range[] = {
	PIN_FIELD(0, 261, 0x100, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_smt_range[] = {
	/* MicroSD insertion input in IOCFG_R. */
	PIN_FIELD_CALC(67, 67, 3, 0x030, 0x10, 27, 1, 32, 0),
	/* SIM hot-plug inputs have independent IOCFG_B Schmitt fields. */
	PIN_FIELD_CALC(112, 112, 2, 0x060, 0x10, 13, 1, 32, 0),
	PIN_FIELD_CALC(113, 113, 2, 0x060, 0x10, 16, 1, 32, 0),
	/* MSDC0 shared DAT0-3, DAT4-7, CMD/DSL/RST and CLK fields. */
	PIN_FIELD_CALC(114, 117, 2, 0x060, 0x10, 0, 1, 32, 1),
	PIN_FIELD_CALC(118, 121, 2, 0x060, 0x10, 1, 1, 32, 1),
	PIN_FIELD_CALC(122, 122, 2, 0x060, 0x10, 2, 1, 32, 0),
	PIN_FIELD_CALC(123, 123, 2, 0x060, 0x10, 3, 1, 32, 0),
	PIN_FIELD_CALC(124, 125, 2, 0x060, 0x10, 2, 1, 32, 1),
	/* SIM1/2 reset and data pins share one Schmitt field per interface. */
	PIN_FIELD_CALC(126, 126, 2, 0x060, 0x10, 11, 1, 32, 0),
	PIN_FIELD_CALC(127, 128, 2, 0x060, 0x10, 12, 1, 32, 1),
	PIN_FIELD_CALC(155, 155, 2, 0x060, 0x10, 14, 1, 32, 0),
	PIN_FIELD_CALC(156, 157, 2, 0x060, 0x10, 15, 1, 32, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_tdsel_range[] = {
	PIN_FIELD_CALC(114, 117, 2, 0x0d0, 0x10, 0, 4, 32, 1),
	PIN_FIELD_CALC(118, 121, 2, 0x0d0, 0x10, 4, 4, 32, 1),
	PIN_FIELD_CALC(122, 122, 2, 0x0d0, 0x10, 8, 4, 32, 0),
	PIN_FIELD_CALC(123, 123, 2, 0x0d0, 0x10, 12, 4, 32, 0),
	PIN_FIELD_CALC(124, 125, 2, 0x0d0, 0x10, 8, 4, 32, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_rdsel_range[] = {
	PIN_FIELD_CALC(114, 117, 2, 0x080, 0x10, 0, 6, 32, 1),
	PIN_FIELD_CALC(118, 121, 2, 0x080, 0x10, 6, 6, 32, 1),
	PIN_FIELD_CALC(122, 125, 2, 0x080, 0x10, 12, 6, 32, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_ies_range[] = {
	PIN_FIELD_CALC(67, 67, 3, 0x010, 0x10, 14, 1, 32, 0),
	PIN_FIELD_CALC(112, 112, 2, 0x020, 0x10, 20, 1, 32, 0),
	PIN_FIELD_CALC(113, 113, 2, 0x020, 0x10, 24, 1, 32, 0),
	PIN_FIELD_CALC(114, 125, 2, 0x020, 0x10, 0, 1, 32, 0),
	PIN_FIELD_CALC(126, 128, 2, 0x020, 0x10, 17, 1, 32, 0),
	/* MSDC1 CMD, DAT0-3 and CLK input buffers in IOCFG_B. */
	PIN_FIELD_CALC(129, 134, 2, 0x020, 0x10, 25, 1, 32, 0),
	PIN_FIELD_CALC(155, 157, 2, 0x020, 0x10, 21, 1, 32, 0),
};

static const struct mtk_pin_field_calc mt6797_pin_drv_range[] = {
	PIN_FIELD_CALC(114, 117, 2, 0x1a0, 0x10, 0, 3, 32, 1),
	PIN_FIELD_CALC(118, 121, 2, 0x1a0, 0x10, 3, 3, 32, 1),
	PIN_FIELD_CALC(122, 122, 2, 0x1a0, 0x10, 6, 3, 32, 0),
	PIN_FIELD_CALC(123, 123, 2, 0x1a0, 0x10, 9, 3, 32, 0),
	PIN_FIELD_CALC(124, 125, 2, 0x1a0, 0x10, 6, 3, 32, 1),
	PIN_FIELD_CALC(129, 129, 2, 0x1b0, 0x10, 21, 3, 32, 0),
	/* DAT0 through DAT3 share one drive-strength selector. */
	PIN_FIELD_CALC(130, 133, 2, 0x1b0, 0x10, 24, 3, 32, 1),
	PIN_FIELD_CALC(134, 134, 2, 0x1b0, 0x10, 27, 3, 32, 0),
};

/* Detect pads use separate PU/PD enables, unlike the six SIM signals. */
static const struct mtk_pin_field_calc mt6797_pin_pu_range[] = {
	PIN_FIELD_CALC(67, 67, 3, 0x0e0, 0x10, 14, 1, 32, 0),
	PIN_FIELD_CALC(112, 112, 2, 0x180, 0x10, 3, 1, 32, 0),
	PIN_FIELD_CALC(113, 113, 2, 0x180, 0x10, 7, 1, 32, 0),
};

static const struct mtk_pin_field_calc mt6797_pin_pd_range[] = {
	PIN_FIELD_CALC(67, 67, 3, 0x0c0, 0x10, 14, 1, 32, 0),
	PIN_FIELD_CALC(112, 113, 2, 0x150, 0x10, 0, 1, 32, 0),
};

static const struct mtk_pin_field_calc mt6797_pin_pupd_range[] = {
	PIN_FIELD_CALC(114, 125, 2, 0x100, 0x10, 6, 1, 32, 0),
	PIN_FIELD_CALC(126, 128, 2, 0x100, 0x10, 24, 1, 32, 0),
	PIN_FIELD_CALC(129, 134, 2, 0x100, 0x10, 18, 1, 32, 0),
	PIN_FIELD_CALC(155, 157, 2, 0x100, 0x10, 27, 1, 32, 0),
};

static const struct mtk_pin_field_calc mt6797_pin_r0_range[] = {
	PIN_FIELD_CALC(114, 125, 2, 0x110, 0x10, 6, 1, 32, 0),
	PIN_FIELD_CALC(126, 128, 2, 0x110, 0x10, 24, 1, 32, 0),
	PIN_FIELD_CALC(129, 134, 2, 0x110, 0x10, 18, 1, 32, 0),
	PIN_FIELD_CALC(155, 157, 2, 0x110, 0x10, 27, 1, 32, 0),
};

static const struct mtk_pin_field_calc mt6797_pin_r1_range[] = {
	PIN_FIELD_CALC(114, 125, 2, 0x120, 0x10, 6, 1, 32, 0),
	PIN_FIELD_CALC(126, 128, 2, 0x120, 0x10, 24, 1, 32, 0),
	PIN_FIELD_CALC(129, 134, 2, 0x120, 0x10, 18, 1, 32, 0),
	PIN_FIELD_CALC(155, 157, 2, 0x120, 0x10, 27, 1, 32, 0),
};

static const struct mtk_pin_reg_calc mt6797_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6797_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6797_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6797_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6797_pin_do_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6797_pin_smt_range),
	[PINCTRL_PIN_REG_TDSEL] = MTK_RANGE(mt6797_pin_tdsel_range),
	[PINCTRL_PIN_REG_RDSEL] = MTK_RANGE(mt6797_pin_rdsel_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6797_pin_ies_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6797_pin_drv_range),
	[PINCTRL_PIN_REG_PU] = MTK_RANGE(mt6797_pin_pu_range),
	[PINCTRL_PIN_REG_PD] = MTK_RANGE(mt6797_pin_pd_range),
	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(mt6797_pin_pupd_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6797_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt6797_pin_r1_range),
};

static const char * const mt6797_pinctrl_register_base_names[] = {
	"gpio", "iocfgl", "iocfgb", "iocfgr", "iocfgt",
};

static const unsigned int mt6797_pull_type[ARRAY_SIZE(mtk_pins_mt6797)] = {
	[67] = MTK_PULL_PU_PD_TYPE,
	[112 ... 113] = MTK_PULL_PU_PD_TYPE,
	[114 ... 125] = MTK_PULL_PUPD_R1R0_TYPE,
	[126 ... 128] = MTK_PULL_PUPD_R1R0_TYPE,
	[129 ... 134] = MTK_PULL_PUPD_R1R0_TYPE,
	[155 ... 157] = MTK_PULL_PUPD_R1R0_TYPE,
};

static const unsigned int mt6797_debounce_time[] = {
	128, 256, 512, 1024, 16384, 32768, 65536, 131072, 262144, 524288, 0
};

static const struct mtk_eint_hw mt6797_eint_hw = {
	.port_mask = 7,
	.ports = 6,
	.ap_num = 192,
	.db_cnt = 16,
	.db_time = mt6797_debounce_time,
};

static const struct mtk_pin_soc mt6797_data = {
	.reg_cal = mt6797_reg_cals,
	.pins = mtk_pins_mt6797,
	.npins = ARRAY_SIZE(mtk_pins_mt6797),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6797),
	.eint_hw = &mt6797_eint_hw,
	.gpio_m = 0,
	.base_names = mt6797_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6797_pinctrl_register_base_names),
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
	.pull_type = mt6797_pull_type,
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.adv_pull_set = mtk_pinconf_adv_pull_set,
	.adv_pull_get = mtk_pinconf_adv_pull_get,
};

static const struct of_device_id mt6797_pinctrl_of_match[] = {
	{ .compatible = "mediatek,mt6797-pinctrl", .data = &mt6797_data },
	{ }
};

static struct platform_driver mt6797_pinctrl_driver = {
	.driver = {
		.name = "mt6797-pinctrl",
		.of_match_table = mt6797_pinctrl_of_match,
		.pm = pm_sleep_ptr(&mtk_paris_pinctrl_pm_ops),
	},
	.probe = mtk_paris_pinctrl_probe,
};

static int __init mt6797_pinctrl_init(void)
{
	return platform_driver_register(&mt6797_pinctrl_driver);
}
arch_initcall(mt6797_pinctrl_init);
