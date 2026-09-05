// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed MT6797 MD1 pre-release platform adapter.
 *
 * Stage 1 consumes no active resource. Stage 2 snapshots, changes, and restores
 * every MD1 rail field touched by the vendor, then stops. Stage 3 snapshots,
 * changes and restores only the three SPM clock-buffer command fields used by
 * the exact vendor clk_buf_set_by_flightmode(false) path, then stops. Stage 4
 * holds the PMIC and clock-buffer changes while it clears MD_EXT_BUCK_ISO and
 * obtains the two PWR_STATUS acknowledgements, then unwinds in reverse. Stage
 * 5 additionally releases internal clock/isolation/SRAM while reset remains
 * held and bus protection remains set, then unwinds in reverse. No stage
 * reaches reset release, boot vector, CLDMA, CCIF, SIM or RF.
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
			 "allow explicitly staged MT6797 MD1 pre-release diagnostics");

static bool enable_experimental_md1_stage4_power_ack;
module_param_named(enable_experimental_md1_stage4_power_ack,
		   enable_experimental_md1_stage4_power_ack, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_stage4_power_ack,
		 "allow the reversible MT6797 MD1 stage-4 power ack transaction");

static bool enable_experimental_md1_stage5_reset_held;
module_param_named(enable_experimental_md1_stage5_reset_held,
		   enable_experimental_md1_stage5_reset_held, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_stage5_reset_held,
		 "allow the reversible MT6797 MD1 stage-5 reset-held transaction");

enum mt6797_md1_prerelease_stage {
	MT6797_MD1_STAGE_INVENTORY = 1,
	MT6797_MD1_STAGE_PMIC_ROLLBACK,
	MT6797_MD1_STAGE_CLOCK_BUFFER_GATE,
	MT6797_MD1_STAGE_POWER_ACK,
	MT6797_MD1_STAGE_RESET_HELD,
};

static ushort experimental_md1_prerelease_stage;
module_param_named(experimental_md1_prerelease_stage,
			   experimental_md1_prerelease_stage, ushort, 0400);
MODULE_PARM_DESC(experimental_md1_prerelease_stage,
		 "MD1 stage: 1=inventory, 2=PMIC, 3=clock-buffer, 4=power ack, 5=reset-held, 6+=refuse");

struct mt6797_md1_stage4_context {
	struct regmap *pmic;
	struct regmap *spm;
	struct mt6797_md1_pmic_snapshot pmic_snapshot;
	struct mt6797_md1_clkbuf_snapshot clkbuf_snapshot;
	struct mt6797_md1_spm_snapshot spm_snapshot;
};

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

static int mt6797_md1_stage4_pmic_on(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;
	int ret;

	ret = mt6797_md1_pmic_snapshot(stage4->pmic, &stage4->pmic_snapshot);
	if (ret)
		return ret;
	return mt6797_md1_pmic_apply_vendor_values(stage4->pmic,
					     &stage4->pmic_snapshot);
}

static int mt6797_md1_stage4_pmic_off(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;

	return stage4->pmic_snapshot.valid ?
		mt6797_md1_pmic_restore(stage4->pmic, &stage4->pmic_snapshot) : 0;
}

static int mt6797_md1_stage4_clock_on(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;
	int ret;

	ret = mt6797_md1_clkbuf_snapshot(stage4->spm, &stage4->clkbuf_snapshot);
	if (ret)
		return ret;
	return mt6797_md1_clkbuf_set_modem_on(stage4->spm,
					  &stage4->clkbuf_snapshot);
}

static int mt6797_md1_stage4_clock_off(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;

	return stage4->clkbuf_snapshot.valid ?
		mt6797_md1_clkbuf_restore(stage4->spm, &stage4->clkbuf_snapshot) : 0;
}

static int mt6797_md1_stage4_domain_on(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;
	int ret;

	ret = mt6797_md1_spm_snapshot(stage4->spm, &stage4->spm_snapshot);
	if (ret)
		return ret;
	return mt6797_md1_spm_power_on(stage4->spm, &stage4->spm_snapshot);
}

static int mt6797_md1_stage4_domain_off(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;

	return stage4->spm_snapshot.valid ?
		mt6797_md1_spm_restore(stage4->spm, &stage4->spm_snapshot) : 0;
}

static int mt6797_md1_stage5_domain_on(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;
	int ret;

	ret = mt6797_md1_stage4_domain_on(context);
	if (ret)
		return ret;
	return mt6797_md1_spm_release_internal_reset_held(stage4->spm,
							  &stage4->spm_snapshot);
}

static int mt6797_md1_stage5_domain_off(void *context)
{
	struct mt6797_md1_stage4_context *stage4 = context;
	struct mt6797_md1_spm_snapshot *snapshot = &stage4->spm_snapshot;
	int internal_ret = 0, power_ret;

	if (snapshot->internal_valid)
		internal_ret = mt6797_md1_spm_restore_internal(stage4->spm, snapshot);
	power_ret = mt6797_md1_stage4_domain_off(context);
	return internal_ret ?: power_ret;
}

static int mt6797_md1_stage4_poll(void *context)
{
	(void)context;
	/* mt6797_md1_spm_power_on() already performs the bounded dual-status poll. */
	return 0;
}

static const struct mt6797_md1_prerelease_ops mt6797_md1_stage4_ops = {
	.abi = MT6797_MD1_PRERELEASE_ABI,
	.pmic_on = mt6797_md1_stage4_pmic_on,
	.pmic_off = mt6797_md1_stage4_pmic_off,
	.clock_on = mt6797_md1_stage4_clock_on,
	.clock_off = mt6797_md1_stage4_clock_off,
	.domain_on = mt6797_md1_stage4_domain_on,
	.domain_off = mt6797_md1_stage4_domain_off,
	.poll = mt6797_md1_stage4_poll,
};

static const struct mt6797_md1_prerelease_ops mt6797_md1_stage5_ops = {
	.abi = MT6797_MD1_PRERELEASE_ABI,
	.pmic_on = mt6797_md1_stage4_pmic_on,
	.pmic_off = mt6797_md1_stage4_pmic_off,
	.clock_on = mt6797_md1_stage4_clock_on,
	.clock_off = mt6797_md1_stage4_clock_off,
	.domain_on = mt6797_md1_stage5_domain_on,
	.domain_off = mt6797_md1_stage5_domain_off,
	.poll = mt6797_md1_stage4_poll,
};

static int mt6797_md1_power_ack_stage(struct device *dev)
{
	struct mt6797_md1_stage4_context context = { };
	struct mt6797_md1_prerelease prerelease;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_stage4_power_ack) ||
	    !device_property_read_bool(dev, "mediatek,stage4-power-ack"))
		return dev_err_probe(dev, -EPERM,
			"stage 4 requires independent module and DT power-ack gates\n");
	context.pmic = mt6797_md1_get_pmic_regmap(dev);
	if (IS_ERR(context.pmic))
		return dev_err_probe(dev, PTR_ERR(context.pmic),
			"MT6351 PMIC regmap unavailable\n");
	context.spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(context.spm))
		return dev_err_probe(dev, PTR_ERR(context.spm), "SPM syscon regmap unavailable\n");
	ret = mt6797_md1_prerelease_init(&prerelease, &mt6797_md1_stage4_ops,
					 &context);
	if (ret)
		return ret;
	ret = mt6797_md1_prerelease_run(&prerelease);
	if (ret)
		return dev_err_probe(dev, ret,
			"stage-4 PMIC/clock/MD1 power-ack transaction failed\n");
	dev_info(dev, "MD1 stage-4 power ack succeeded and all state was restored\n");
	return 0;
}

static int mt6797_md1_reset_held_stage(struct device *dev)
{
	struct mt6797_md1_stage4_context context = { };
	struct mt6797_md1_prerelease prerelease;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_stage5_reset_held) ||
	    !device_property_read_bool(dev, "mediatek,stage5-reset-held"))
		return dev_err_probe(dev, -EPERM,
			"stage 5 requires independent module and DT reset-held gates\n");
	context.pmic = mt6797_md1_get_pmic_regmap(dev);
	if (IS_ERR(context.pmic))
		return dev_err_probe(dev, PTR_ERR(context.pmic),
			"MT6351 PMIC regmap unavailable\n");
	context.spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(context.spm))
		return dev_err_probe(dev, PTR_ERR(context.spm), "SPM syscon regmap unavailable\n");
	ret = mt6797_md1_prerelease_init(&prerelease, &mt6797_md1_stage5_ops,
					 &context);
	if (ret)
		return ret;
	ret = mt6797_md1_prerelease_run(&prerelease);
	if (ret)
		return dev_err_probe(dev, ret,
			"stage-5 reset-held MD1 transaction failed\n");
	dev_info(dev,
		 "MD1 stage-5 reset-held transaction succeeded and all state was restored\n");
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
	case MT6797_MD1_STAGE_POWER_ACK:
		return mt6797_md1_power_ack_stage(dev);
	case MT6797_MD1_STAGE_RESET_HELD:
		return mt6797_md1_reset_held_stage(dev);
	default:
		return dev_err_probe(dev, -EOPNOTSUPP,
			"stopping after the reversible MD1 reset-held transaction\n");
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

MODULE_DESCRIPTION("Fail-closed MT6797 MD1 pre-release power-ack adapter");
MODULE_LICENSE("GPL");
