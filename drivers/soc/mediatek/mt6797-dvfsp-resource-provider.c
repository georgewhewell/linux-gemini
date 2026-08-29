// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 DVFSP resource-only provider.
 *
 * This provider binds only the resource-lifetime owner. It resolves the
 * reviewed backend devices, retains references through the owner's explicit
 * attach/detach lifecycle, and performs no readback, register access,
 * firmware operation, secure call, clock change, rail change, or owner
 * registration.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/soc/mediatek/mt6797-dvfsp-resource-owner.h>

#define MT6797_DVFSP_RESOURCE_OWNER_LIFECYCLE_HANDLE	1

struct mt6797_dvfsp_resource_provider {
	struct mt6797_dvfsp_resource_owner owner;
};

static int mt6797_dvfsp_resource_provider_resolve(
	struct device_node *provider,
	const char *property, struct device **device)
{
	struct device_node *resource_node;
	struct platform_device *resource_pdev;

	resource_node = of_parse_phandle(provider, property, 0);
	if (!resource_node)
		return -EINVAL;

	resource_pdev = of_find_device_by_node(resource_node);
	of_node_put(resource_node);
	if (!resource_pdev)
		return -EPROBE_DEFER;

	*device = &resource_pdev->dev;
	return 0;
}

static void mt6797_dvfsp_resource_provider_put_devices(
	struct mt6797_dvfsp_state_source_devices *devices)
{
	if (!devices)
		return;
	if (devices->calibration_source)
		put_device(devices->calibration_source);
	if (devices->thermal)
		put_device(devices->thermal);
	if (devices->bigidvfs_backend)
		put_device(devices->bigidvfs_backend);
	if (devices->clock_backend)
		put_device(devices->clock_backend);
}

static int mt6797_dvfsp_resource_provider_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_resource_provider *provider;
	struct mt6797_dvfsp_state_source_devices devices = { };
	int ret;

	provider = devm_kzalloc(&pdev->dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;

	ret = mt6797_dvfsp_resource_provider_resolve(
		pdev->dev.of_node, "mediatek,clock-backend",
		&devices.clock_backend);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "clock backend is not available\n");
	ret = mt6797_dvfsp_resource_provider_resolve(
		pdev->dev.of_node, "mediatek,bigidvfs-backend",
		&devices.bigidvfs_backend);
	if (ret)
		goto err_put_devices;
	ret = mt6797_dvfsp_resource_provider_resolve(
		pdev->dev.of_node, "mediatek,thermal", &devices.thermal);
	if (ret)
		goto err_put_devices;
	ret = mt6797_dvfsp_resource_provider_resolve(
		pdev->dev.of_node, "mediatek,calibration-source",
		&devices.calibration_source);
	if (ret)
		goto err_put_devices;

	ret = mt6797_dvfsp_resource_owner_init(&provider->owner,
			MT6797_DVFSP_RESOURCE_OWNER_LIFECYCLE_HANDLE);
	if (ret)
		goto err_put_devices;
	ret = mt6797_dvfsp_resource_owner_attach(&provider->owner, &devices);
	mt6797_dvfsp_resource_provider_put_devices(&devices);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot attach resource owner\n");

	platform_set_drvdata(pdev, provider);
	dev_info(&pdev->dev,
		 "resource-only owner bound; writes disabled; provider unregistered\n");
	return 0;

err_put_devices:
	mt6797_dvfsp_resource_provider_put_devices(&devices);
	return dev_err_probe(&pdev->dev, ret,
				     "resource dependency is not available\n");
}

static void mt6797_dvfsp_resource_provider_remove(struct platform_device *pdev)
{
	struct mt6797_dvfsp_resource_provider *provider =
		platform_get_drvdata(pdev);
	int ret;

	if (!provider)
		return;
	ret = mt6797_dvfsp_resource_owner_detach(&provider->owner);
	if (ret && ret != -ENOENT)
		dev_warn(&pdev->dev, "resource owner detach failed: %d\n", ret);
}

static const struct of_device_id mt6797_dvfsp_resource_provider_of_match[] = {
	{ .compatible = "mediatek,mt6797-dvfsp-resource-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_dvfsp_resource_provider_of_match);

static struct platform_driver mt6797_dvfsp_resource_provider_driver = {
	.probe = mt6797_dvfsp_resource_provider_probe,
	.remove = mt6797_dvfsp_resource_provider_remove,
	.driver = {
		.name = "mt6797-dvfsp-resource-owner",
		.of_match_table = mt6797_dvfsp_resource_provider_of_match,
	},
};
module_platform_driver(mt6797_dvfsp_resource_provider_driver);

MODULE_DESCRIPTION("MT6797 DVFSP resource-only provider");
MODULE_LICENSE("GPL");
