// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 WLAN GEN3 W4 HIF platform ownership boundary. */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w3-session.h>

struct mt6797_wifi_w4 {
	struct device *controller_dev;
	void __iomem *portal;
	int irq;
	struct mt6797_consys_runtime_controller_wifi_lease runtime;
	const struct mt6797_consys_stp_session_lease *stp_session;
	struct mt6797_wifi_ahb_w3_session session;
};

static bool mt6797_wifi_w4_resource_valid(const struct resource *res)
{
	return res && resource_size(res) == MT6797_WIFI_AHB_HIF_PORTAL_SIZE &&
		res->start == MT6797_WIFI_AHB_HIF_BASE;
}

static irqreturn_t mt6797_wifi_w4_irq_thread(int irq, void *data)
{
	/* No RX dispatcher exists; IRQF_NO_AUTOEN keeps this line masked. */
	return IRQ_NONE;
}

static int mt6797_wifi_w4_probe(struct platform_device *pdev)
{
	struct mt6797_wifi_w4 *w4;
	struct resource *res;
	struct device_node *node;
	struct platform_device *controller;
	int ret;

	if (!device_property_read_bool(&pdev->dev,
		"mediatek,experimental-wifi-w4-platform"))
		return dev_err_probe(&pdev->dev, -EPERM,
			"W4 requires explicit DT opt-in; session fenced\n");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mt6797_wifi_w4_resource_valid(res))
		return dev_err_probe(&pdev->dev, -EINVAL,
			"WLAN HIF must be 0x%08x/0x%x\n", (u32)MT6797_WIFI_AHB_HIF_BASE,
			(u32)MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
	w4 = devm_kzalloc(&pdev->dev, sizeof(*w4), GFP_KERNEL);
	if (!w4)
		return -ENOMEM;
	w4->portal = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(w4->portal))
		return PTR_ERR(w4->portal);
	w4->irq = platform_get_irq_byname(pdev, "wifi");
	if (w4->irq < 0)
		return w4->irq;
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing runtime controller\n");
	controller = of_find_device_by_node(node);
	of_node_put(node);
	if (!controller)
		return -EPROBE_DEFER;
	w4->controller_dev = &controller->dev;
	ret = devm_request_threaded_irq(&pdev->dev, w4->irq, NULL,
		mt6797_wifi_w4_irq_thread, IRQF_ONESHOT | IRQF_NO_AUTOEN,
		dev_name(&pdev->dev), w4);
	if (ret) {
		put_device(w4->controller_dev);
		return ret;
	}
	platform_set_drvdata(pdev, w4);
	dev_info(&pdev->dev, "W4 portal/IRQ reserved; W1 handoff absent, session fenced\n");
	return 0;
}

static void mt6797_wifi_w4_remove(struct platform_device *pdev)
{
	struct mt6797_wifi_w4 *w4 = platform_get_drvdata(pdev);

	if (!w4)
		return;
	mt6797_wifi_ahb_w3_session_stop(&w4->session);
	mt6797_consys_runtime_controller_wifi_session_put(&w4->runtime);
	put_device(w4->controller_dev);
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id mt6797_wifi_w4_of_match[] = {
	{ .compatible = "mediatek,mt6797-wifi-ahb-w4-platform" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_wifi_w4_of_match);

static struct platform_driver mt6797_wifi_w4_driver = {
	.probe = mt6797_wifi_w4_probe, .remove = mt6797_wifi_w4_remove,
	.driver = { .name = "mt6797-wifi-ahb-w4-platform",
		.of_match_table = mt6797_wifi_w4_of_match, .suppress_bind_attrs = true },
};
module_platform_driver(mt6797_wifi_w4_driver);
MODULE_DESCRIPTION("MT6797 WLAN GEN3 W4 dormant HIF platform owner");
MODULE_LICENSE("GPL");
