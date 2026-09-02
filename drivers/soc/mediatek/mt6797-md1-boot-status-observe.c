// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, explicitly gated MT6797 MD1 boot-status observation.
 *
 * The MT6797 vendor CCCI driver reads MD1_CFG_BOOT_STATS0/1 from the AP
 * infrastructure view when reporting modem boot status.  It maps the two
 * words at 0x10201300, makes two dummy reads, and reports the third:
 *
 * drivers/misc/mediatek/eccci/mt6797/modem_reg_base.h:70-72
 * drivers/misc/mediatek/eccci/mt6797/cldma_platform.c:360-378
 *
 * This is deliberately narrower than a CCCI/CLDMA driver.  It neither maps
 * CLDMA, CCIF, MD-side registers, nor the CCCI carve-outs, and it cannot
 * create a modem transport.  There are no writes, clock/power/reset/IRQ/DMA
 * requests, firmware operations, or shared-memory accesses here.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MT6797_MD1_BOOT_STATUS_BASE	0x10201300
#define MT6797_MD1_BOOT_STATUS_SIZE	0x8
#define MT6797_MD1_BOOT_STATUS0		0x0
#define MT6797_MD1_BOOT_STATUS1		0x4
#define MT6797_MD1_BOOT_STATUS_SAMPLES	3

static bool enable_experimental_boot_status;
module_param_named(enable_experimental_boot_status,
			   enable_experimental_boot_status, bool, 0400);
MODULE_PARM_DESC(enable_experimental_boot_status,
		 "allow the one-shot MT6797 MD1 boot-status observation (default: false)");

static int mt6797_md1_boot_status_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct resource *res;
	void __iomem *base;
	u32 stats0[MT6797_MD1_BOOT_STATUS_SAMPLES];
	u32 stats1[MT6797_MD1_BOOT_STATUS_SAMPLES];
	unsigned int i;

	if (!READ_ONCE(enable_experimental_boot_status) ||
	    !device_property_read_bool(dev,
				       "mediatek,experimental-md1-boot-status"))
		return dev_err_probe(dev, -EPERM,
			"refusing MD1 boot-status observation without both explicit gates\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "md1-boot-status");
	if (!res)
		return dev_err_probe(dev, -EINVAL,
			"missing md1-boot-status resource\n");

	if (res->start != MT6797_MD1_BOOT_STATUS_BASE ||
	    resource_size(res) != MT6797_MD1_BOOT_STATUS_SIZE)
		return dev_err_probe(dev, -EINVAL,
			"md1-boot-status must be %08x/%#x\n",
			MT6797_MD1_BOOT_STATUS_BASE, MT6797_MD1_BOOT_STATUS_SIZE);

	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/*
	 * This fixed sequence matches the vendor's two dummy reads and one report
	 * read per word.  It has no delay, poll, retry, or write path.  A printk
	 * immediately before every read leaves a serial/log boundary if a read
	 * itself wedges the fabric; software cannot recover from such a wedge.
	 */
	for (i = 0; i < MT6797_MD1_BOOT_STATUS_SAMPLES; i++) {
		dev_info(dev, "before MD1_BOOT_STATS0 sample %u/%u\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES);
		stats0[i] = readl(base + MT6797_MD1_BOOT_STATUS0);
		dev_info(dev, "after MD1_BOOT_STATS0 sample %u/%u = %#08x\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES, stats0[i]);
	}

	for (i = 0; i < MT6797_MD1_BOOT_STATUS_SAMPLES; i++) {
		dev_info(dev, "before MD1_BOOT_STATS1 sample %u/%u\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES);
		stats1[i] = readl(base + MT6797_MD1_BOOT_STATUS1);
		dev_info(dev, "after MD1_BOOT_STATS1 sample %u/%u = %#08x\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES, stats1[i]);
	}

	dev_info(dev,
		 "MD1 boot-status complete: stats0=%#08x/%#08x/%#08x stats1=%#08x/%#08x/%#08x; no writes\n",
		 stats0[0], stats0[1], stats0[2], stats1[0], stats1[1], stats1[2]);
	return 0;
}

static const struct of_device_id mt6797_md1_boot_status_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-boot-status-observe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_boot_status_of_match);

static struct platform_driver mt6797_md1_boot_status_driver = {
	.probe = mt6797_md1_boot_status_probe,
	.driver = {
		.name = "mt6797-md1-boot-status-observe",
		.of_match_table = mt6797_md1_boot_status_of_match,
	},
};
module_platform_driver(mt6797_md1_boot_status_driver);

MODULE_DESCRIPTION("Explicitly gated MT6797 MD1 boot-status observer");
MODULE_LICENSE("GPL");
