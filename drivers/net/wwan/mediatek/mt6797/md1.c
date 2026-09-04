// SPDX-License-Identifier: GPL-2.0-only
/* Single, fail-closed resource owner for the MT6797 MD1 modem. */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "power.h"

#define MT6797_MD1_DMA_BITS	36
#define MT6797_MD1_SPM_PWR_STATUS	0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND	0x184
#define MT6797_MD1_POWER_SAMPLES	3

struct mt6797_md1 {
	void __iomem *regs[6];
	struct clk_bulk_data clocks[2];
	struct reset_control_bulk_data resets[2];
	struct regmap *spm;
	enum mt6797_md1_power_state power_state;
	u32 power_status;
	u32 power_status_2nd;
	int irqs[3];
};

static const char * const mt6797_md1_reg_names[] = {
	"ap-ao",
	"md-ao",
	"ap-pdn",
	"md-pdn",
	"ap-ccif",
	"md-ccif",
};

static const char * const mt6797_md1_irq_names[] = {
	"cldma",
	"ccif",
	"wdt",
};

static int mt6797_md1_validate_memory(struct device *dev)
{
	struct device_node *memory;
	int count;
	unsigned int i;

	count = of_count_phandle_with_args(dev->of_node, "memory-region", NULL);
	if (count != 2)
		return dev_err_probe(dev, count < 0 ? count : -EINVAL,
				     "expected two reserved-memory regions\n");
	for (i = 0; i < 2; i++) {
		memory = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!memory)
			return dev_err_probe(dev, -EINVAL,
					     "missing reserved-memory region %u\n", i);
		of_node_put(memory);
	}

	return 0;
}

static int mt6797_md1_observe_power(struct device *dev,
				    struct mt6797_md1 *md1)
{
	enum mt6797_md1_power_state state;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < MT6797_MD1_POWER_SAMPLES; attempt++) {
		ret = regmap_read(md1->spm, MT6797_MD1_SPM_PWR_STATUS,
				  &md1->power_status);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read SPM power status\n");

		ret = regmap_read(md1->spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
				  &md1->power_status_2nd);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read second SPM power status\n");

		state = mt6797_md1_power_state_decode(md1->power_status,
						      md1->power_status_2nd);
		md1->power_state = state;
		if (md1->power_state != MT6797_MD1_POWER_UNSTABLE)
			return 0;

		cpu_relax();
	}

	return dev_err_probe(dev, -EAGAIN,
			     "MD1 SPM power status did not stabilize\n");
}

static int mt6797_md1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_md1 *md1;
	size_t num_resets;
	unsigned int i;
	int ret;

	md1 = devm_kzalloc(dev, sizeof(*md1), GFP_KERNEL);
	if (!md1)
		return -ENOMEM;

	md1->spm = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "mediatek,spm");
	if (IS_ERR(md1->spm))
		return dev_err_probe(dev, PTR_ERR(md1->spm),
				     "failed to acquire SPM syscon\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_reg_names); i++) {
		const char *name = mt6797_md1_reg_names[i];

		md1->regs[i] = devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(md1->regs[i]))
			return PTR_ERR(md1->regs[i]);
	}

	md1->clocks[0].id = "ccif-ap";
	md1->clocks[1].id = "ccif-md";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(md1->clocks), md1->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire clocks\n");

	md1->resets[0].id = "cldma-pdn";
	md1->resets[1].id = "cldma-ao";
	num_resets = ARRAY_SIZE(md1->resets);
	ret = devm_reset_control_bulk_get_exclusive(dev, num_resets, md1->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire resets\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_irq_names); i++) {
		const char *name = mt6797_md1_irq_names[i];

		md1->irqs[i] = platform_get_irq_byname(pdev, name);
		if (md1->irqs[i] < 0)
			return md1->irqs[i];
	}

	ret = mt6797_md1_validate_memory(dev);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(MT6797_MD1_DMA_BITS));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set 36-bit DMA mask\n");
	ret = mt6797_md1_observe_power(dev, md1);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, md1);
	dev_info(dev, "preserving inherited MD1 power state: %s\n",
		 md1->power_state == MT6797_MD1_POWERED ? "powered" : "off");
	return 0;
}

static const struct of_device_id mt6797_md1_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_of_match);

static struct platform_driver mt6797_md1_driver = {
	.probe = mt6797_md1_probe,
	.driver = {
		.name = "mt6797-md1",
		.of_match_table = mt6797_md1_of_match,
	},
};
module_platform_driver(mt6797_md1_driver);

MODULE_DESCRIPTION("MediaTek MT6797 MD1 resource owner");
MODULE_LICENSE("GPL");
