// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 MD1 passive LK hand-off owner.
 *
 * Probe validates the complete aggregate hardware topology and the two
 * reserved-memory nodes rewritten by LK.  It intentionally maps no register,
 * requests no interrupt, enables no clock or regulator, allocates no DMA, and
 * performs no SMC.  In particular, it cannot release or restart the modem.
 */

#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "mt6797-md1-handoff.h"

static const char *const mt6797_md1_reg_names[] = {
	"ap-cldma-ao",
	"infracfg",
	"md-cldma-ao",
	"ap-cldma-pdn",
	"md-cldma-pdn",
	"ap-ccif",
	"md-ccif",
	"apmixedsys",
	"md-clksw",
	"md-dcm",
	"md-peri-misc",
	"md-l1-a0",
	"md-top-pll",
	"md-sys-clk",
	"md-rgu",
	"l1-rgu",
	"boot-vector",
	"boot-status",
};

static const u32 mt6797_md1_interrupts[] = {
	0, 265, IRQ_TYPE_LEVEL_HIGH,
	0, 147, IRQ_TYPE_LEVEL_LOW,
	0, 266, IRQ_TYPE_EDGE_FALLING,
};

static int mt6797_md1_validate_resources(struct platform_device *pdev)
{
	struct mt6797_md1_resource resources[MT6797_MD1_RESOURCE_COUNT];
	struct resource *resource;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		const char *name = mt6797_md1_reg_names[i];

		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		if (!resource)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "missing %s resource\n",
					     mt6797_md1_reg_names[i]);
		resources[i].start = resource->start;
		resources[i].size = resource_size(resource);
	}

	return mt6797_md1_resources_validate(resources, ARRAY_SIZE(resources));
}

static int mt6797_md1_reserved_region(struct device_node *node, int index,
				      u64 *base, u64 *size)
{
	struct device_node *memory_node;
	struct reserved_mem *memory;

	memory_node = of_parse_phandle(node, "memory-region", index);
	if (!memory_node)
		return -EINVAL;
	memory = of_reserved_mem_lookup(memory_node);
	of_node_put(memory_node);
	if (!memory)
		return -EINVAL;

	*base = memory->base;
	*size = memory->size;
	return 0;
}

static int mt6797_md1_validate_layout(struct platform_device *pdev)
{
	struct mt6797_md1_layout layout;
	struct mt6797_md1_remap_plan remap;
	int ret;

	ret = mt6797_md1_reserved_region(pdev->dev.of_node, 0,
					 &layout.firmware_base,
					 &layout.firmware_size);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing LK MD1 firmware carveout\n");
	ret = mt6797_md1_reserved_region(pdev->dev.of_node, 1,
					 &layout.shared_base,
					 &layout.shared_size);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing LK MD1 shared carveout\n");

	ret = mt6797_md1_remap_plan_build(&layout, &remap);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unsafe LK MD1 carveout geometry\n");

	dev_info(&pdev->dev,
		 "passive LK handoff: firmware %#llx+%#llx, shared %#llx+%#llx, bank4 %#llx\n",
		 layout.firmware_base, layout.firmware_size,
		 layout.shared_base, layout.shared_size, remap.bank4_base);
	return 0;
}

static int mt6797_md1_handoff_probe(struct platform_device *pdev)
{
	u32 interrupts[ARRAY_SIZE(mt6797_md1_interrupts)];
	int ret;

	ret = mt6797_md1_validate_resources(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unexpected MD1 register topology\n");
	ret = of_property_read_u32_array(pdev->dev.of_node, "interrupts",
					 interrupts, ARRAY_SIZE(interrupts));
	if (ret || memcmp(interrupts, mt6797_md1_interrupts,
			  sizeof(interrupts)))
		return dev_err_probe(&pdev->dev, ret ?: -ERANGE,
				     "unexpected MD1 interrupt topology\n");
	ret = mt6797_md1_validate_layout(pdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev,
		 "no-write owner ready; MD1 activation remains blocked\n");
	return 0;
}

static const struct of_device_id mt6797_md1_handoff_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_md1_handoff_of_match);

static struct platform_driver mt6797_md1_handoff_driver = {
	.probe = mt6797_md1_handoff_probe,
	.driver = {
		.name = "mt6797-md1-handoff",
		.of_match_table = mt6797_md1_handoff_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_handoff_driver);

MODULE_DESCRIPTION("MediaTek MT6797 MD1 passive LK handoff owner");
MODULE_LICENSE("GPL");
