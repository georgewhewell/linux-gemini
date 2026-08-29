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

static const struct mtk_pin_reg_calc mt6797_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6797_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6797_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6797_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6797_pin_do_range),
};

static const char * const mt6797_pinctrl_register_base_names[] = {
	"gpio", "iocfgl", "iocfgb", "iocfgr", "iocfgt",
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
