// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, default-disabled MT6797 G2c patch-only chronology owner.
 *
 * This is the one bounded successor to the G2b no-firmware diagnostic.  It
 * repeats the reviewed vendor EMI -> identity -> MBIST/AFE -> reset-release
 * prefix, then asks the normal runtime controller to execute its hash-pinned
 * full-STP patch stage.  It immediately fences BTIF, reasserts reset, and
 * releases every reversible vote.  It does not issue WMT function control,
 * register HCI or netdev, associate, or expose a radio client.
 *
 * The vendor WMT reset issued after each patch has firmware-internal effects
 * which are not known from source.  Shared EMI and AFE also lack an inverse.
 * Therefore any uncertain post-ID cleanup is terminal for the boot.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>

static bool enable_experimental_g2c_patch_owner;
module_param_named(enable_experimental_g2c_patch_owner,
			   enable_experimental_g2c_patch_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2c_patch_owner,
		"allow one MT6797 G2c patch-only chronology diagnostic (default: false)");

static int mt6797_consys_g2c_get_supplier(struct device *dev,
		const char *property, struct device **supplier)
{
	struct device_node *node;
	struct platform_device *pdev;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return dev_err_probe(dev, -EINVAL, "missing %s provider\n", property);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for %s provider\n", property);
	*supplier = &pdev->dev;
	return 0;
}

static int mt6797_consys_g2c_run(struct device *dev,
		struct device *runtime_dev, struct device *controller_dev)
{
	struct mt6797_consys_runtime_resource_lease resources = { };
	struct mt6797_consys_runtime_provider_lease identity = { };
	struct mt6797_consys_postid_owner postid;
	bool postid_initialized = false;
	bool controller_active = false;
	bool emi_claimed = false;
	int ret;
	int cleanup_ret;

	/* Pin every reversible supplier before consuming the one-way EMI claim. */
	dev_info(dev, "G2c before reversible supplier preflight\n");
	ret = mt6797_consys_runtime_resource_lease_acquire(runtime_dev, &resources);
	if (ret == -ENODEV)
		ret = -EPROBE_DEFER;
	dev_info(dev, "G2c after runtime resource preflight ret=%d\n", ret);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_controller_preflight(controller_dev);
	dev_info(dev, "G2c after controller/BTIF preflight ret=%d\n", ret);
	if (ret)
		goto out_resources;

	dev_info(dev, "G2c before persistent EMI claim\n");
	ret = mt6797_consys_emi_mpu_claim(dev);
	dev_info(dev, "G2c after persistent EMI claim ret=%d\n", ret);
	if (ret)
		goto out_resources;
	emi_claimed = true;

	dev_info(dev, "G2c before reset-held identity\n");
	ret = mt6797_consys_runtime_platform_identity_acquire(&resources, &identity);
	dev_info(dev, "G2c after reset-held identity ret=%d\n", ret);
	if (ret)
		goto out_resources;

	ret = mt6797_consys_runtime_postid_owner_init(runtime_dev,
			identity.identity, &postid);
	if (ret)
		goto out_identity;
	postid_initialized = true;

	dev_info(dev, "G2c before MBIST/AFE/reset release\n");
	ret = mt6797_consys_postid_start(&postid, identity.identity, true);
	dev_info(dev, "G2c after MBIST/AFE/reset release ret=%d\n", ret);
	if (ret)
		goto out_postid;

	/* The controller's no-patch property is rejected before this call. */
	dev_info(dev, "G2c before full WMT patch manifest (no function control)\n");
	ret = mt6797_consys_runtime_controller_start(controller_dev);
	dev_info(dev, "G2c after full WMT patch manifest ret=%d\n", ret);
	if (ret)
		goto out_postid;
	controller_active = true;

	dev_info(dev, "G2c patch manifest passed; fencing BTIF before any radio function\n");

out_postid:
	if (controller_active)
		mt6797_consys_runtime_controller_stop(controller_dev);
	if (postid_initialized &&
	    mt6797_consys_postid_state(&postid) == MT6797_CONSYS_POSTID_ACTIVE) {
		cleanup_ret = mt6797_consys_postid_stop(&postid);
		if (!ret)
			ret = cleanup_ret;
	}
	if (postid_initialized &&
	    mt6797_consys_postid_state(&postid) != MT6797_CONSYS_POSTID_IDLE) {
		mt6797_consys_emi_mpu_latch_fatal();
		dev_err(dev, "G2c post-ID cleanup is uncertain; reboot required\n");
		return ret ? ret : -EIO;
	}
out_identity:
	cleanup_ret = mt6797_consys_runtime_platform_identity_release(&identity);
	if (!ret)
		ret = cleanup_ret;
out_resources:
	cleanup_ret = mt6797_consys_runtime_resource_lease_release(&resources);
	if (!ret)
		ret = cleanup_ret;
	if (emi_claimed && ret) {
		mt6797_consys_emi_mpu_latch_fatal();
		dev_err(dev, "G2c failed after persistent EMI claim; reboot required\n");
	}
	return ret;
}

static int mt6797_consys_g2c_patch_owner_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *runtime_dev;
	struct device *controller_dev;
	int ret;

	if (!READ_ONCE(enable_experimental_g2c_patch_owner) ||
	    !device_property_read_bool(dev,
			"mediatek,experimental-g2c-patch-owner"))
		return dev_err_probe(dev, -EPERM,
				     "G2c owner requires explicit DT and parameter opt-in\n");

	ret = mt6797_consys_g2c_get_supplier(dev, "mediatek,runtime-provider",
					    &runtime_dev);
	if (ret)
		return ret;
	ret = mt6797_consys_g2c_get_supplier(dev, "mediatek,controller",
					    &controller_dev);
	if (ret)
		goto put_runtime;
	if (device_property_read_bool(controller_dev,
			"mediatek,experimental-bootstrap-no-patch")) {
		ret = dev_err_probe(dev, -EPERM,
				    "controller lacks the required patch stage\n");
		goto put_controller;
	}

	ret = mt6797_consys_g2c_run(dev, runtime_dev, controller_dev);
put_controller:
	put_device(controller_dev);
put_runtime:
	put_device(runtime_dev);
	if (ret)
		return dev_err_probe(dev, ret, "G2c patch-only diagnostic failed\n");

	dev_info(dev, "G2c patch-only diagnostic passed; reset asserted, BTIF fenced\n");
	return 0;
}

static const struct of_device_id mt6797_consys_g2c_patch_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-g2c-patch-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_g2c_patch_owner_of_match);

static struct platform_driver mt6797_consys_g2c_patch_owner_driver = {
	.probe = mt6797_consys_g2c_patch_owner_probe,
	.driver = {
		.name = "mt6797-consys-g2c-patch-owner",
		.of_match_table = mt6797_consys_g2c_patch_owner_of_match,
		/* EMI is one-way: do not offer runtime unbind after a live attempt. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_g2c_patch_owner_driver);

MODULE_DESCRIPTION("One-shot gated MT6797 CONSYS G2c patch-only owner");
MODULE_LICENSE("GPL");
