// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed MT6797 MD1 pre-release platform adapter.
 *
 * This consumes no active resource. It validates the vendor DT inventory,
 * then stops before PMIC rails. The vendor on path changes VMODEM/VMD1/VSRAM
 * voltage selectors and hardware-control bits, while its off path only clears
 * enable/sleep bits (eccci/mt6797/cldma_platform.c:542-576). Current mainline
 * also has no MD1 SCPSYS power-domain provider. Neither contract has a safe
 * rollback, so this adapter must not bind the transaction core to hardware.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/string.h>

struct mt6797_md1_prerelease_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

static const struct mt6797_md1_prerelease_resource mt6797_md1_resources[] = {
	{ "ap-cldma-ao", 0x10014000, 0x1e00 },
	{ "md-cldma-ao", 0x10015000, 0x1e00 },
	{ "ap-cldma-pdn", 0x10219000, 0x1e00 },
	{ "md-cldma-pdn", 0x1021a000, 0x1e00 },
	{ "ap-ccif", 0x10209000, SZ_4K },
	{ "md-ccif", 0x1020a000, SZ_4K },
};

static const u32 mt6797_md1_irqs[] = {
	0, 265, 4, /* GIC_SPI, level high: CLDMA */
	0, 147, 8, /* GIC_SPI, level low: CCIF */
	0, 266, 2, /* GIC_SPI, edge falling: MD watchdog */
};

static const char * const mt6797_md1_clock_names[] = {
	"scp-sys-md1-main", "infra-ccif-ap", "infra-ccif-md",
	"infra-ap-c2k-ccif-0", "infra-ap-c2k-ccif-1",
	"infra-md2md-ccif-0", "infra-md2md-ccif-1", "infra-md2md-ccif-2",
	"infra-md2md-ccif-3", "infra-md2md-ccif-4", "infra-md2md-ccif-5",
};

static bool enable_experimental_md1_prerelease;
module_param_named(enable_experimental_md1_prerelease,
			   enable_experimental_md1_prerelease, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_prerelease,
		 "allow MT6797 MD1 pre-release inventory only (default: false)");

static int mt6797_md1_validate_resource(struct platform_device *pdev,
		const struct mt6797_md1_prerelease_resource *expected)
{
	const struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing %s resource\n",
				     expected->name);
	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid %s resource\n",
				     expected->name);
	return 0;
}

static int mt6797_md1_validate_clock_names(struct device *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_clock_names); i++) {
		if (device_property_match_string(dev, "mediatek,vendor-clock-names",
					 mt6797_md1_clock_names[i]) < 0)
			return dev_err_probe(dev, -EINVAL, "missing vendor clock %s\n",
					 mt6797_md1_clock_names[i]);
	}
	return 0;
}

static int mt6797_md1_validate_carveout(struct device *dev, const char *compat,
					u64 expected_size)
{
	struct device_node *node;
	u64 size;

	node = of_find_compatible_node(NULL, NULL, compat);
	if (!node)
		return dev_err_probe(dev, -EINVAL, "missing %s carve-out\n", compat);
	if (!of_property_read_bool(node, "no-map") ||
	    of_property_read_u64(node, "size", &size) || size != expected_size) {
		of_node_put(node);
		return dev_err_probe(dev, -EINVAL, "invalid %s carve-out\n", compat);
	}
	of_node_put(node);
	return 0;
}

static int mt6797_md1_prerelease_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	u32 irqs[ARRAY_SIZE(mt6797_md1_irqs)];
	unsigned int i;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_prerelease) ||
	    !device_property_read_bool(dev, "mediatek,experimental-md1-prerelease") ||
	    !device_property_present(dev, "mediatek,stage-inventory-only"))
		return dev_err_probe(dev, -EPERM,
			"MD1 pre-release requires explicit inventory-only gates\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_resources); i++) {
		ret = mt6797_md1_validate_resource(pdev, &mt6797_md1_resources[i]);
		if (ret)
			return ret;
	}
	if (of_property_read_u32_array(dev->of_node, "interrupts", irqs,
				       ARRAY_SIZE(irqs)) ||
	    memcmp(irqs, mt6797_md1_irqs, sizeof(irqs)))
		return dev_err_probe(dev, -EINVAL, "invalid CLDMA/CCIF IRQ inventory\n");
	ret = mt6797_md1_validate_clock_names(dev);
	if (ret)
		return ret;
	ret = mt6797_md1_validate_carveout(dev,
			"mediatek,reserve-memory-ccci_md1", 0xa100000);
	if (ret)
		return ret;
	ret = mt6797_md1_validate_carveout(dev,
			"mediatek,reserve-memory-ccci_share", 0x600000);
	if (ret)
		return ret;

	return dev_err_probe(dev, -EOPNOTSUPP,
		"inventory validated; stopping before unproven PMIC/MD1-power rollback\n");
}

static const struct of_device_id mt6797_md1_prerelease_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-prerelease" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_prerelease_of_match);

static struct platform_driver mt6797_md1_prerelease_driver = {
	.probe = mt6797_md1_prerelease_probe,
	.driver = {
		.name = "mt6797-md1-prerelease",
		.of_match_table = mt6797_md1_prerelease_of_match,
	},
};
module_platform_driver(mt6797_md1_prerelease_driver);

MODULE_DESCRIPTION("Fail-closed MT6797 MD1 pre-release inventory adapter");
MODULE_LICENSE("GPL");
