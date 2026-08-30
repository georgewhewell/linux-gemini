// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed MT6797 MD1 pre-release platform adapter.
 *
 * Stage 1 consumes no active resource. Stage 2 snapshots, changes, and restores
 * every MD1 rail field touched by the vendor, then stops. Stage 3 snapshots,
 * changes and restores only the three SPM clock-buffer command fields used by
 * the exact vendor clk_buf_set_by_flightmode(false) path, then stops.  The SPM
 * MD1 power primitive is intentionally not bound here. No stage reaches reset
 * release, boot vector, CLDMA, CCIF, SIM or RF.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>
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

enum mt6797_md1_prerelease_stage {
	MT6797_MD1_STAGE_INVENTORY = 1,
	MT6797_MD1_STAGE_PMIC_ROLLBACK,
	MT6797_MD1_STAGE_CLOCK_BUFFER_GATE,
};

static ushort experimental_md1_prerelease_stage;
module_param_named(experimental_md1_prerelease_stage,
			   experimental_md1_prerelease_stage, ushort, 0400);
MODULE_PARM_DESC(experimental_md1_prerelease_stage,
			 "MD1 stage: 1=inventory, 2=PMIC snapshot/apply/restore, "
			 "3=clock-buffer snapshot/apply/restore, 4+=refuse");

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

static struct regmap *mt6797_md1_get_pmic_regmap(struct device *dev)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct regmap *regmap;

	node = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	/* MT6397's MFD driver obtains this same PMIC map from its pwrap parent. */
	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	return regmap ?: ERR_PTR(-EPROBE_DEFER);
}

static int mt6797_md1_pmic_rollback_stage(struct device *dev)
{
	struct mt6797_md1_pmic_snapshot snapshot;
	struct regmap *pmic;
	int apply_ret, restore_ret, ret;

	if (!device_property_read_bool(dev, "mediatek,stage-pmic-rollback"))
		return dev_err_probe(dev, -EPERM,
			"PMIC stage requires an explicit rollback-only DT gate\n");

	pmic = mt6797_md1_get_pmic_regmap(dev);
	if (IS_ERR(pmic))
		return dev_err_probe(dev, PTR_ERR(pmic), "MT6351 PMIC regmap unavailable\n");

	ret = mt6797_md1_pmic_snapshot(pmic, &snapshot);
	if (ret)
		return dev_err_probe(dev, ret, "could not snapshot MD1 PMIC fields\n");

	apply_ret = mt6797_md1_pmic_apply_vendor_values(pmic, &snapshot);
	/* Restore after a partial write too. A PMIC transaction is never retained. */
	restore_ret = mt6797_md1_pmic_restore(pmic, &snapshot);
	if (apply_ret)
		return dev_err_probe(dev, apply_ret, "MD1 PMIC rollback stage apply failed\n");
	if (restore_ret)
		return dev_err_probe(dev, restore_ret, "MD1 PMIC state restore failed\n");

	dev_info(dev, "MD1 PMIC values restored; stopping before clock-buffer gate\n");
	return 0;
}

static int mt6797_md1_clkbuf_rollback_stage(struct device *dev)
{
	struct mt6797_md1_clkbuf_snapshot snapshot;
	struct regmap *spm;
	int apply_ret, restore_ret, ret;

	if (!device_property_read_bool(dev, "mediatek,stage-clock-buffer-rollback"))
		return dev_err_probe(dev, -EPERM,
			"clock-buffer stage requires an explicit rollback-only DT gate\n");

	spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(spm))
		return dev_err_probe(dev, PTR_ERR(spm), "SPM syscon regmap unavailable\n");

	ret = mt6797_md1_clkbuf_snapshot(spm, &snapshot);
	if (ret)
		return dev_err_probe(dev, ret, "could not snapshot SPM_MDBSI_CON[2:0]\n");

	apply_ret = mt6797_md1_clkbuf_set_modem_on(spm, &snapshot);
	/* Restore after any failed write or readback.  No state is retained. */
	restore_ret = mt6797_md1_clkbuf_restore(spm, &snapshot);
	if (apply_ret)
		return dev_err_probe(dev, apply_ret,
			"clock-buffer command latch failed; state restore was attempted\n");
	if (restore_ret)
		return dev_err_probe(dev, restore_ret, "clock-buffer state restore failed\n");

	dev_info(dev,
		 "SPM_MDBSI_CON[2:0] command latched/restored; stopping before MD1 power\n");
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
	    !experimental_md1_prerelease_stage)
		return dev_err_probe(dev, -EPERM,
			"MD1 pre-release requires explicit enable, DT, and stage gates\n");
	if (experimental_md1_prerelease_stage == MT6797_MD1_STAGE_INVENTORY &&
	    !device_property_present(dev, "mediatek,stage-inventory-only"))
		return dev_err_probe(dev, -EPERM, "missing inventory-only DT gate\n");

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

	switch (experimental_md1_prerelease_stage) {
	case MT6797_MD1_STAGE_INVENTORY:
		dev_info(dev, "MD1 inventory validated; no hardware operation requested\n");
		return 0;
	case MT6797_MD1_STAGE_PMIC_ROLLBACK:
		return mt6797_md1_pmic_rollback_stage(dev);
	case MT6797_MD1_STAGE_CLOCK_BUFFER_GATE:
		return mt6797_md1_clkbuf_rollback_stage(dev);
	default:
		return dev_err_probe(dev, -EOPNOTSUPP,
			"stopping before MD1 power-domain control\n");
	}
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
