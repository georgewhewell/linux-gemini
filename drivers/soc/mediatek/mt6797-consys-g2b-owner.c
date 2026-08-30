// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, default-disabled MT6797 G2b chronology owner.
 *
 * This is the missing live bridge between the reviewed resource provider and
 * the reviewed BTIF/STP controller.  It is intentionally a diagnostic, not a
 * radio driver: after the vendor EMI -> identity -> MBIST/AFE -> reset-release
 * prefix it runs only the raw-HCR/HVR/FVR plus mandatory-to-full STP query
 * sequence, then synchronously fences BTIF, reasserts reset, and releases all
 * reversible runtime votes.  The controller is explicitly placed in its
 * no-patch mode; no firmware, Wi-Fi, Bluetooth, GNSS, or FM command is sent.
 *
 * EMI protection/remap/clear and AFE programming have no vendor inverse.  An
 * error after either is therefore terminal for this boot.  This driver does
 * not claim that a userspace or kernel deadline survives a system-wide stall.
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

static bool enable_experimental_g2b_owner;
module_param_named(enable_experimental_g2b_owner, enable_experimental_g2b_owner,
			   bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2b_owner,
		"allow one MT6797 G2b no-firmware chronology diagnostic (default: false)");

static int mt6797_consys_g2b_get_supplier(struct device *dev,
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

static int mt6797_consys_g2b_run(struct device *dev,
		struct device *runtime_dev, struct device *controller_dev)
{
	struct mt6797_consys_runtime_resource_lease resources = { };
	struct mt6797_consys_runtime_provider_lease identity = { };
	struct mt6797_consys_postid_owner postid;
	bool postid_initialized = false;
	bool controller_active = false;
	int ret;
	int cleanup_ret;

	/*
	 * The shared-EMI provider is deliberately claimed first.  Its one-way
	 * lifetime is what makes the following identity/post-ID lease chronology
	 * match vendor hw_init(), rather than inventing a reversible fake setup.
	 */
	dev_info(dev, "G2b before persistent EMI claim\n");
	ret = mt6797_consys_emi_mpu_claim(dev);
	dev_info(dev, "G2b after persistent EMI claim ret=%d\n", ret);
	if (ret)
		return ret;

	dev_info(dev, "G2b before runtime resources\n");
	ret = mt6797_consys_runtime_resource_lease_acquire(runtime_dev, &resources);
	dev_info(dev, "G2b after runtime resources ret=%d\n", ret);
	if (ret)
		return ret;

	dev_info(dev, "G2b before reset-held identity\n");
	ret = mt6797_consys_runtime_platform_identity_acquire(&resources, &identity);
	dev_info(dev, "G2b after reset-held identity ret=%d\n", ret);
	if (ret)
		goto out_resources;

	ret = mt6797_consys_runtime_postid_owner_init(runtime_dev,
			identity.identity, &postid);
	if (ret)
		goto out_identity;
	postid_initialized = true;

	dev_info(dev, "G2b before MBIST/AFE/reset release\n");
	ret = mt6797_consys_postid_start(&postid, identity.identity, true);
	dev_info(dev, "G2b after MBIST/AFE/reset release ret=%d\n", ret);
	if (ret)
		goto out_postid;

	/* This controller node must opt into mediatek,experimental-bootstrap-no-patch. */
	dev_info(dev, "G2b before raw/mandatory/full WMT sequence (no patch)\n");
	ret = mt6797_consys_runtime_controller_start(controller_dev);
	dev_info(dev, "G2b after raw/mandatory/full WMT sequence ret=%d\n", ret);
	if (ret)
		goto out_postid;
	controller_active = true;

	/* A successful query is deliberately bounded: no session or radio survives it. */
	dev_info(dev, "G2b full query passed; fencing BTIF and restoring reset-held state\n");

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
		/* AFE/reset cleanup is uncertain; keep all pins and require recovery. */
		mt6797_consys_emi_mpu_latch_fatal();
		dev_err(dev, "G2b post-ID cleanup is uncertain; reboot required\n");
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
	return ret;
}

static int mt6797_consys_g2b_owner_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *runtime_dev;
	struct device *controller_dev;
	int ret;

	if (!READ_ONCE(enable_experimental_g2b_owner) ||
	    !device_property_read_bool(dev, "mediatek,experimental-g2b-owner"))
		return dev_err_probe(dev, -EPERM,
				     "G2b owner requires explicit DT and parameter opt-in\n");
	if (!device_property_read_bool(dev,
			"mediatek,experimental-g2b-no-firmware"))
		return dev_err_probe(dev, -EPERM,
				     "G2b owner refuses a firmware-capable controller\n");

	ret = mt6797_consys_g2b_get_supplier(dev, "mediatek,runtime-provider",
					    &runtime_dev);
	if (ret)
		return ret;
	ret = mt6797_consys_g2b_get_supplier(dev, "mediatek,controller",
					    &controller_dev);
	if (ret)
		goto put_runtime;
	if (!device_property_read_bool(controller_dev,
			"mediatek,experimental-bootstrap-no-patch")) {
		ret = dev_err_probe(dev, -EPERM,
				    "controller lacks the required no-patch mode\n");
		goto put_controller;
	}

	ret = mt6797_consys_g2b_run(dev, runtime_dev, controller_dev);
put_controller:
	put_device(controller_dev);
put_runtime:
	put_device(runtime_dev);
	if (ret)
		return dev_err_probe(dev, ret, "G2b no-firmware diagnostic failed\n");

	dev_info(dev, "G2b no-firmware diagnostic passed; reset asserted, BTIF fenced\n");
	return 0;
}

static const struct of_device_id mt6797_consys_g2b_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-g2b-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_g2b_owner_of_match);

static struct platform_driver mt6797_consys_g2b_owner_driver = {
	.probe = mt6797_consys_g2b_owner_probe,
	.driver = {
		.name = "mt6797-consys-g2b-owner",
		.of_match_table = mt6797_consys_g2b_owner_of_match,
		/* EMI is one-way: do not offer runtime unbind after a live attempt. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_g2b_owner_driver);

MODULE_DESCRIPTION("One-shot gated MT6797 CONSYS G2b chronology owner");
MODULE_LICENSE("GPL");
