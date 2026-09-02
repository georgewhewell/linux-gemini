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

#define PIN_FIELD_BASE(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs,\
		       _s_bit, _x_bits) \
	PIN_FIELD_CALC(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs,\
		       _s_bit, _x_bits, 32, 0)

#define PINS_FIELD_BASE(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs,\
			_s_bit, _x_bits) \
	PIN_FIELD_CALC(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs,\
		       _s_bit, _x_bits, 32, 1)

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

/*
 * Only describe fields whose layout is independently documented by the
 * MT6797 GPIO tables and the MSDC1 pad definitions.  The remaining pins keep
 * their existing pinmux-only support until their pad controls are verified.
 */
static const struct mtk_pin_field_calc mt6797_pin_ies_range[] = {
	PIN_FIELD_BASE(67, 67, 3, 0x10, 0x10, 14, 1),
	PIN_FIELD_BASE(129, 134, 2, 0x20, 0x10, 25, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_smt_range[] = {
	PIN_FIELD_BASE(67, 67, 3, 0x30, 0x10, 27, 1),
	PIN_FIELD_BASE(129, 129, 3, 0x30, 0x10, 0, 1),
	PINS_FIELD_BASE(130, 133, 3, 0x30, 0x10, 1, 1),
	PIN_FIELD_BASE(134, 134, 3, 0x30, 0x10, 2, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_drv_range[] = {
	PIN_FIELD_BASE(129, 129, 2, 0x1b0, 0x10, 21, 3),
	PINS_FIELD_BASE(130, 133, 2, 0x1b0, 0x10, 24, 3),
	PIN_FIELD_BASE(134, 134, 2, 0x1b0, 0x10, 27, 3),
};

static const struct mtk_pin_field_calc mt6797_pin_pupd_range[] = {
	PIN_FIELD_BASE(129, 134, 2, 0x100, 0x10, 18, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_r0_range[] = {
	PIN_FIELD_BASE(129, 134, 2, 0x110, 0x10, 18, 1),
};

static const struct mtk_pin_field_calc mt6797_pin_r1_range[] = {
	PIN_FIELD_BASE(129, 134, 2, 0x120, 0x10, 18, 1),
};

static const struct mtk_pin_reg_calc mt6797_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6797_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6797_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6797_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6797_pin_do_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6797_pin_smt_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6797_pin_drv_range),
	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(mt6797_pin_pupd_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6797_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt6797_pin_r1_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6797_pin_ies_range),
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
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
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
