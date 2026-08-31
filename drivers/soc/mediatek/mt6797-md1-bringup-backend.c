// SPDX-License-Identifier: GPL-2.0-only
/*
 * Default-disabled MT6797 MD1 bring-up backend inventory.
 *
 * This driver deliberately has no control to prepare or run the bring-up
 * fence.  An explicit module gate plus a disabled DT node may only validate
 * and map the vendor-sourced resource inventory.  The MD1 EMI-MPU owner is
 * absent, so authorization remains false and no register helper is called.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-backend.h>

static bool enable_experimental_md1_bringup_backend;
module_param_named(enable_experimental_md1_bringup_backend,
			   enable_experimental_md1_bringup_backend, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_bringup_backend,
	"map the default-disabled MT6797 MD1 pre-release backend inventory only");

struct mt6797_md1_bringup_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

static const struct mt6797_md1_bringup_resource mt6797_md1_bringup_resources[] = {
	{ "infracfg", MT6797_MD1_INFRACFG_BASE, MT6797_MD1_INFRACFG_SIZE },
	{ "ap-ccif", MT6797_MD1_AP_CCIF_BASE, MT6797_MD1_CCIF_SIZE },
	{ "md-ccif", MT6797_MD1_MD_CCIF_BASE, MT6797_MD1_CCIF_SIZE },
	{ "md-clksw", MT6797_MD1_CLKSW_BASE, MT6797_MD1_CLKSW_SIZE },
};

static int mt6797_md1_bringup_map_resource(struct platform_device *pdev,
	const struct mt6797_md1_bringup_resource *expected, void __iomem **base)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing %s resource\n",
				     expected->name);
	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid %s resource\n",
				     expected->name);
	*base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(*base))
		return PTR_ERR(*base);
	return 0;
}

static struct regmap *mt6797_md1_bringup_pmic_regmap(struct device *dev)
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
	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	return regmap ?: ERR_PTR(-EPROBE_DEFER);
}

static bool mt6797_md1_bringup_emi_mpu_provider_ready(struct device *dev)
{
	struct device_node *node;
	struct platform_device *pdev;
	bool ready;

	node = of_parse_phandle(dev->of_node, "mediatek,emi-mpu", 0);
	if (!node)
		return false;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return false;
	device_lock(&pdev->dev);
	ready = platform_get_drvdata(pdev) && !mt6797_md1_emi_mpu_is_fatal();
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ready;
}

static int mt6797_md1_bringup_backend_probe(struct platform_device *pdev)
{
	struct mt6797_md1_bringup_backend *backend;
	struct mt6797_md1_bringup_authorization authorization;
	struct mt6797_md1_bringup_fence *fence;
	void __iomem *base[ARRAY_SIZE(mt6797_md1_bringup_resources)];
	unsigned int i;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_bringup_backend) ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,experimental-md1-bringup-backend"))
		return dev_err_probe(&pdev->dev, -EPERM,
			"MD1 backend inventory needs explicit module and DT gates\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_bringup_resources); i++) {
		ret = mt6797_md1_bringup_map_resource(pdev,
					      &mt6797_md1_bringup_resources[i], &base[i]);
		if (ret)
			return ret;
	}
	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	fence = devm_kzalloc(&pdev->dev, sizeof(*fence), GFP_KERNEL);
	if (!backend || !fence)
		return -ENOMEM;
	ret = mt6797_md1_bringup_backend_init(backend, &pdev->dev, base[0], base[1],
						 base[2], base[3]);
	if (ret)
		return ret;
	backend->emi_mpu_provider_available =
		mt6797_md1_bringup_emi_mpu_provider_ready(&pdev->dev);
	backend->pmic = mt6797_md1_bringup_pmic_regmap(&pdev->dev);
	if (IS_ERR(backend->pmic))
		return dev_err_probe(&pdev->dev, PTR_ERR(backend->pmic),
			"MT6351 PMIC regmap unavailable\n");
	backend->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "mediatek,spm");
	if (IS_ERR(backend->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(backend->spm),
			"SPM syscon regmap unavailable\n");
	ret = mt6797_md1_bringup_fence_init(fence,
					    &mt6797_md1_bringup_backend_ops, backend);
	if (ret)
		return ret;
	mt6797_md1_bringup_backend_authorization(backend, &authorization);
	platform_set_drvdata(pdev, backend);
	dev_info(&pdev->dev,
		 "MD1 source-backed resource inventory mapped; no LK plan prepared and no fence run path exists\n");
	return 0;
}

static const struct of_device_id mt6797_md1_bringup_backend_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-bringup-backend" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_bringup_backend_of_match);

static struct platform_driver mt6797_md1_bringup_backend_driver = {
	.probe = mt6797_md1_bringup_backend_probe,
	.driver = {
		.name = "mt6797-md1-bringup-backend",
		.of_match_table = mt6797_md1_bringup_backend_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_bringup_backend_driver);

MODULE_DESCRIPTION("Default-disabled MT6797 MD1 pre-release backend inventory");
MODULE_LICENSE("GPL");
