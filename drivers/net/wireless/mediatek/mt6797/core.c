// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 integrated GEN3 WLAN endpoint. */

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#include "firmware.h"
#include "hif.h"

#define MT6797_WIFI_HIF_BASE	0x180f0000
#define MT6797_WIFI_HIF_SPI	283

struct mt6797_wifi {
	struct mt6797_wifi_ahb_hif hif;
	struct mt6797_wifi_firmware firmware;
	int irq;
};

static void mt6797_wifi_release_firmware(void *data)
{
	struct mt6797_wifi *wifi = data;

	mt6797_wifi_firmware_release(&wifi->firmware);
}

static int mt6797_wifi_validate_irq(struct platform_device *pdev)
{
	struct of_phandle_args spec;
	int ret;

	if (platform_irq_count(pdev) != 1)
		return -EINVAL;
	ret = of_irq_parse_one(pdev->dev.of_node, 0, &spec);
	if (ret)
		return ret;
	ret = spec.args_count == 3 && spec.args[0] == GIC_SPI &&
		spec.args[1] == MT6797_WIFI_HIF_SPI &&
		spec.args[2] == IRQ_TYPE_LEVEL_LOW ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
}

static int mt6797_wifi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_wifi *wifi;
	struct resource *resource;
	void __iomem *base;
	int ret;

	wifi = devm_kzalloc(dev, sizeof(*wifi), GFP_KERNEL);
	if (!wifi)
		return -ENOMEM;
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != MT6797_WIFI_HIF_BASE ||
	    resource_size(resource) != MT6797_WIFI_AHB_HIF_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid HIF resource\n");
	base = devm_ioremap_resource(dev, resource);
	if (IS_ERR(base))
		return PTR_ERR(base);
	ret = mt6797_wifi_ahb_hif_init(&wifi->hif, base);
	if (ret)
		return ret;

	ret = mt6797_wifi_validate_irq(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "invalid HIF interrupt\n");
	wifi->irq = platform_get_irq_byname(pdev, "hif");
	if (wifi->irq < 0)
		return wifi->irq;
	if ((irq_get_trigger_type(wifi->irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW)
		return dev_err_probe(dev, -EINVAL,
				     "HIF interrupt must be level-low\n");

	ret = mt6797_wifi_firmware_request(dev, &wifi->firmware);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to acquire verified WLAN firmware\n");
	ret = devm_add_action_or_reset(dev, mt6797_wifi_release_firmware, wifi);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, wifi);
	dev_info(dev, "verified WLAN firmware and HIF acquired; radio remains off\n");
	return 0;
}

static const struct of_device_id mt6797_wifi_of_match[] = {
	{ .compatible = "mediatek,mt6797-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_wifi_of_match);

static struct platform_driver mt6797_wifi_driver = {
	.probe = mt6797_wifi_probe,
	.driver = {
		.name = "mt6797-wifi",
		.of_match_table = mt6797_wifi_of_match,
	},
};
module_platform_driver(mt6797_wifi_driver);

MODULE_DESCRIPTION("MediaTek MT6797 integrated GEN3 WLAN endpoint");
MODULE_LICENSE("GPL");
