// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 G2d VCN33 preflight provider.
 *
 * Vendor MT6797 sets CONSYS_BT_WIFI_SHARE_V33 to zero and exposes independent
 * BT and Wi-Fi PALDO controls. Its non-legacy path obtains vcn33_bt and
 * vcn33_wifi, sets each to 3.3 V and enables/disables it in the PALDO paths:
 * gemini-linux-kernel-3.18 .../mt6797/mtk_wcn_consys_hw.c:897-935.
 *
 * This provider deliberately stops one step before that mutation. The same
 * vendor build reads an efuse level after patching and calls the declared but
 * absent wmt_set_pmic_voltage(level) before calibration (wmt_ic_soc.c:
 * 1120-1138). No source proves the level-to-voltage mapping or restoration,
 * so enabling either rail here would be a guess.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

static bool enable_experimental_g2d_vcn33_preflight;
module_param_named(enable_experimental_g2d_vcn33_preflight,
			   enable_experimental_g2d_vcn33_preflight, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2d_vcn33_preflight,
		"allow MT6797 G2d VCN33 handle preflight only (default: false)");

static int mt6797_consys_g2d_vcn33_preflight_probe(struct platform_device *pdev)
{
	struct regulator *bt;
	struct regulator *wifi;

	if (!READ_ONCE(enable_experimental_g2d_vcn33_preflight) ||
	    !device_property_read_bool(&pdev->dev,
			"mediatek,experimental-g2d-vcn33-preflight"))
		return dev_err_probe(&pdev->dev, -EPERM,
			"G2d VCN33 preflight requires explicit DT and parameter opt-in\n");

	bt = devm_regulator_get(&pdev->dev, "vcn33-bt");
	if (IS_ERR(bt))
		return dev_err_probe(&pdev->dev, PTR_ERR(bt),
			"missing vcn33-bt supply\n");
	wifi = devm_regulator_get(&pdev->dev, "vcn33-wifi");
	if (IS_ERR(wifi))
		return dev_err_probe(&pdev->dev, PTR_ERR(wifi),
			"missing vcn33-wifi supply\n");
	if (bt == wifi)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"vendor MT6797 requires distinct BT/Wi-Fi VCN33 handles\n");

	dev_info(&pdev->dev,
		 "G2d VCN33 handles acquired; rails remain untouched pending efuse mapping\n");
	return 0;
}

static const struct of_device_id mt6797_consys_g2d_vcn33_preflight_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-g2d-vcn33-preflight" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_g2d_vcn33_preflight_of_match);

static struct platform_driver mt6797_consys_g2d_vcn33_preflight_driver = {
	.probe = mt6797_consys_g2d_vcn33_preflight_probe,
	.driver = {
		.name = "mt6797-consys-g2d-vcn33-preflight",
		.of_match_table = mt6797_consys_g2d_vcn33_preflight_of_match,
	},
};
module_platform_driver(mt6797_consys_g2d_vcn33_preflight_driver);

MODULE_DESCRIPTION("Gated MT6797 G2d VCN33 resource preflight");
MODULE_LICENSE("GPL");
