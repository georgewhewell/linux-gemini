// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime MT6351 FGADC qualification bridge for the Gemini PDA.
 *
 * An OF overlay applied after the MT6351 MFD probed cannot cause the MFD to
 * revisit its children.  This module deliberately creates exactly one
 * short-lived fgadc platform child after first proving the expected Gemini
 * PMIC, overlay node and inactive FGADC state.  Loading it with the default
 * activate=0 only performs the register snapshot; it makes no PMIC writes.
 *
 * The active driver is responsible for restoring its own PMIC configuration
 * when this child is unregistered.  Therefore unregistering the child is the
 * only supported teardown path, and it happens before this module can unload.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Present in the FGADC follow-on commit, repeated for pre-integration builds. */
#ifndef MT6351_TOP_CKPDN_CON2
#define MT6351_TOP_CKPDN_CON2	0x0246
#endif
#ifndef MT6351_FGADC_CON15
#define MT6351_FGADC_CON15	0x0cc2
#endif

#define GEMINI_PMIC_DEVICE		"1000d000.pwrap:pmic"
#define GEMINI_PMIC_COMPATIBLE		"mediatek,mt6351"
#define GEMINI_PMIC_DRIVER		"mt6397"
#define GEMINI_FGADC_COMPATIBLE		"mediatek,mt6351-fgadc"
#define GEMINI_FGADC_DRIVER		"mt6351-fgadc"
#define GEMINI_FGADC_READONLY_DRIVER	"mt6351-fg-readonly"
#define GEMINI_FGADC_CLK_PDN_MASK	GENMASK(3, 2)
#define GEMINI_FGADC_CON0_LOW_MASK	GENMASK(7, 0)
#define GEMINI_FGADC_CON0_HIGH_MASK	GENMASK(15, 8)
#define GEMINI_FGADC_OSR_MASK		GENMASK(3, 0)

struct gemini_fgadc_snapshot {
	unsigned int clkpdn;
	unsigned int con0;
	unsigned int osr;
};

static DEFINE_MUTEX(gemini_fgadc_lock);
static struct device *gemini_pmic;
static struct device_node *gemini_fgadc_node;
static struct platform_device *gemini_fgadc_pdev;
static struct gemini_fgadc_snapshot before;
static bool preflight_ok;
static bool activate;
static bool restore_pending;

module_param(activate, bool, 0400);
MODULE_PARM_DESC(activate, "create the FGADC child during module load (default false)");
module_param_named(snapshot_clkpdn, before.clkpdn, uint, 0444);
MODULE_PARM_DESC(snapshot_clkpdn, "read-only preflight TOP_CKPDN_CON2 value");
module_param_named(snapshot_con0, before.con0, uint, 0444);
MODULE_PARM_DESC(snapshot_con0, "read-only preflight FGADC_CON0 value");
module_param_named(snapshot_osr, before.osr, uint, 0444);
MODULE_PARM_DESC(snapshot_osr, "read-only preflight FGADC_CON15 value");
module_param_named(preflight_passed, preflight_ok, bool, 0444);
MODULE_PARM_DESC(preflight_passed, "whether the immutable live identity checks passed");

static struct device *gemini_find_platform_device(const char *name)
{
	return bus_find_device_by_name(&platform_bus_type, NULL, name);
}

static int gemini_reject_existing_owner(const char *name)
{
	struct device *dev;

	dev = gemini_find_platform_device(name);
	if (!dev)
		return 0;

	pr_err("gemini-live-fgadc: refusing while existing %s device %s owns PMIC state\n",
	       name, dev_name(dev));
	put_device(dev);
	return -EBUSY;
}

static int gemini_find_fgadc_node(struct device_node **out)
{
	struct device_node *np;
	struct device_node *next;

	np = of_find_compatible_node(NULL, NULL, GEMINI_FGADC_COMPATIBLE);
	if (!np) {
		pr_err("gemini-live-fgadc: %s overlay node is absent\n",
		       GEMINI_FGADC_COMPATIBLE);
		return -ENODEV;
	}

	/* The iterator drops its starting-node reference; retain our first match. */
	next = of_find_compatible_node(of_node_get(np), NULL,
				       GEMINI_FGADC_COMPATIBLE);
	if (next) {
		pr_err("gemini-live-fgadc: multiple %s nodes; refusing ambiguous target\n",
		       GEMINI_FGADC_COMPATIBLE);
		of_node_put(next);
		of_node_put(np);
		return -EINVAL;
	}

	if (!of_device_is_available(np)) {
		pr_err("gemini-live-fgadc: overlay node %pOF is disabled\n", np);
		of_node_put(np);
		return -ENODEV;
	}

	*out = np;
	return 0;
}

static int gemini_fgadc_preflight_locked(void)
{
	struct mt6397_chip *mt6351;
	struct device_node *parent;
	int ret;

	if (preflight_ok)
		return 0;

	gemini_pmic = gemini_find_platform_device(GEMINI_PMIC_DEVICE);
	if (!gemini_pmic) {
		pr_err("gemini-live-fgadc: expected PMIC %s is absent\n",
		       GEMINI_PMIC_DEVICE);
		return -ENODEV;
	}

	if (!gemini_pmic->driver ||
	    strcmp(gemini_pmic->driver->name, GEMINI_PMIC_DRIVER) ||
	    !of_device_is_compatible(gemini_pmic->of_node, GEMINI_PMIC_COMPATIBLE)) {
		pr_err("gemini-live-fgadc: PMIC identity differs (device=%s driver=%s)\n",
		       dev_name(gemini_pmic), gemini_pmic->driver ?
		       gemini_pmic->driver->name : "(none)");
		ret = -ENODEV;
		goto out_put_pmic;
	}

	ret = gemini_find_fgadc_node(&gemini_fgadc_node);
	if (ret)
		goto out_put_pmic;

	parent = of_get_parent(gemini_fgadc_node);
	if (parent != gemini_pmic->of_node) {
		pr_err("gemini-live-fgadc: node %pOF parent differs from PMIC %pOF\n",
		       gemini_fgadc_node, gemini_pmic->of_node);
		of_node_put(parent);
		ret = -EINVAL;
		goto out_put_node;
	}
	of_node_put(parent);

	ret = gemini_reject_existing_owner(GEMINI_FGADC_DRIVER);
	if (ret)
		goto out_put_node;
	ret = gemini_reject_existing_owner(GEMINI_FGADC_READONLY_DRIVER);
	if (ret)
		goto out_put_node;

	mt6351 = dev_get_drvdata(gemini_pmic);
	if (!mt6351 || !mt6351->regmap) {
		pr_err("gemini-live-fgadc: MT6351 regmap is unavailable\n");
		ret = -ENODEV;
		goto out_put_node;
	}

	ret = regmap_read(mt6351->regmap, MT6351_TOP_CKPDN_CON2,
			  &before.clkpdn);
	if (ret)
		goto out_put_node;
	ret = regmap_read(mt6351->regmap, MT6351_FGADC_CON0, &before.con0);
	if (ret)
		goto out_put_node;
	ret = regmap_read(mt6351->regmap, MT6351_FGADC_CON15, &before.osr);
	if (ret)
		goto out_put_node;

	/* Any live FGADC state could belong to firmware or another kernel owner. */
	if ((before.clkpdn & GEMINI_FGADC_CLK_PDN_MASK) !=
	    GEMINI_FGADC_CLK_PDN_MASK ||
	    before.con0 & (GEMINI_FGADC_CON0_LOW_MASK | GEMINI_FGADC_CON0_HIGH_MASK)) {
		pr_err("gemini-live-fgadc: FGADC is not quiescent (ckpdn=%#x con0=%#x); refusing\n",
		       before.clkpdn, before.con0);
		ret = -EBUSY;
		goto out_put_node;
	}

	preflight_ok = true;
	pr_info("gemini-live-fgadc: read-only preflight passed: ckpdn=%#x con0=%#x osr=%#x\n",
		before.clkpdn, before.con0, before.osr);
	return 0;

out_put_node:
	of_node_put(gemini_fgadc_node);
	gemini_fgadc_node = NULL;
out_put_pmic:
	put_device(gemini_pmic);
	gemini_pmic = NULL;
	return ret;
}

static int gemini_fgadc_activate_locked(void)
{
	int ret;

	if (gemini_fgadc_pdev)
		return -EALREADY;

	ret = gemini_fgadc_preflight_locked();
	if (ret)
		return ret;

	if (!of_property_read_bool(gemini_fgadc_node,
				   "mediatek,initialize-fgadc")) {
		pr_err("gemini-live-fgadc: ownership permission absent; refusing activation\n");
		return -EPERM;
	}

	gemini_fgadc_pdev =
		platform_device_alloc(GEMINI_FGADC_DRIVER, PLATFORM_DEVID_NONE);
	if (!gemini_fgadc_pdev)
		return -ENOMEM;

	gemini_fgadc_pdev->dev.parent = gemini_pmic;
	device_set_node(&gemini_fgadc_pdev->dev,
			of_fwnode_handle(gemini_fgadc_node));
	ret = platform_device_add(gemini_fgadc_pdev);
	if (ret)
		goto out_put_pdev;

	if (!gemini_fgadc_pdev->dev.driver ||
	    strcmp(gemini_fgadc_pdev->dev.driver->name, GEMINI_FGADC_DRIVER)) {
		pr_err("gemini-live-fgadc: active child did not bind its intended driver\n");
		ret = -ENODEV;
		goto out_unregister;
	}

	/* The child must be unregistered (and its devm restore run) before rmmod. */
	__module_get(THIS_MODULE);
	pr_info("gemini-live-fgadc: activated %s with preflight snapshot retained\n",
		dev_name(&gemini_fgadc_pdev->dev));
	return 0;

out_unregister:
	platform_device_unregister(gemini_fgadc_pdev);
	gemini_fgadc_pdev = NULL;
	return ret;
out_put_pdev:
	platform_device_put(gemini_fgadc_pdev);
	gemini_fgadc_pdev = NULL;
	return ret;
}

static int gemini_fgadc_verify_restore_locked(void)
{
	struct mt6397_chip *mt6351 = dev_get_drvdata(gemini_pmic);
	struct gemini_fgadc_snapshot after;
	int ret;

	ret = regmap_read(mt6351->regmap, MT6351_TOP_CKPDN_CON2,
			  &after.clkpdn);
	if (ret)
		return ret;
	ret = regmap_read(mt6351->regmap, MT6351_FGADC_CON0, &after.con0);
	if (ret)
		return ret;
	ret = regmap_read(mt6351->regmap, MT6351_FGADC_CON15, &after.osr);
	if (ret)
		return ret;

	if ((after.clkpdn & GEMINI_FGADC_CLK_PDN_MASK) !=
	    (before.clkpdn & GEMINI_FGADC_CLK_PDN_MASK) ||
	    (after.con0 & GEMINI_FGADC_CON0_LOW_MASK) !=
	    (before.con0 & GEMINI_FGADC_CON0_LOW_MASK) ||
	    (after.osr & GEMINI_FGADC_OSR_MASK) !=
	    (before.osr & GEMINI_FGADC_OSR_MASK)) {
		pr_err("gemini-live-fgadc: restore mismatch: before=%#x/%#x/%#x after=%#x/%#x/%#x\n",
		       before.clkpdn, before.con0, before.osr,
		       after.clkpdn, after.con0, after.osr);
		return -EUCLEAN;
	}

	pr_info("gemini-live-fgadc: driver restore verified\n");
	return 0;
}

static int gemini_fgadc_restore_snapshot_locked(void)
{
	struct mt6397_chip *mt6351 = dev_get_drvdata(gemini_pmic);
	int ret;

	ret = regmap_update_bits(mt6351->regmap, MT6351_FGADC_CON15,
				 GEMINI_FGADC_OSR_MASK, before.osr);
	if (ret)
		return ret;
	ret = regmap_update_bits(mt6351->regmap, MT6351_FGADC_CON0,
				 GEMINI_FGADC_CON0_LOW_MASK, before.con0);
	if (ret)
		return ret;

	return regmap_update_bits(mt6351->regmap, MT6351_TOP_CKPDN_CON2,
				  GEMINI_FGADC_CLK_PDN_MASK, before.clkpdn);
}

static int gemini_fgadc_deactivate_locked(void)
{
	int ret;

	if (!gemini_fgadc_pdev && !restore_pending)
		return -ENOENT;

	if (gemini_fgadc_pdev) {
		/* The driver's devm cleanup attempts its exact masked restore first. */
		platform_device_unregister(gemini_fgadc_pdev);
		gemini_fgadc_pdev = NULL;
		restore_pending = true;
	}

	/* Retain the module and an explicit retry path until restoration verifies. */
	ret = gemini_fgadc_verify_restore_locked();
	if (ret) {
		ret = gemini_fgadc_restore_snapshot_locked();
		if (!ret)
			ret = gemini_fgadc_verify_restore_locked();
	}
	if (ret)
		return ret;

	restore_pending = false;
	module_put(THIS_MODULE);
	return 0;
}

static int gemini_fgadc_activate_param(const char *value,
				       const struct kernel_param *kp)
{
	bool requested;
	int ret;

	ret = kstrtobool(value, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gemini_fgadc_lock);
	ret = gemini_fgadc_activate_locked();
	mutex_unlock(&gemini_fgadc_lock);
	return ret;
}

static const struct kernel_param_ops gemini_fgadc_activate_ops = {
	.set = gemini_fgadc_activate_param,
};

module_param_cb(activate_child, &gemini_fgadc_activate_ops, NULL, 0200);
MODULE_PARM_DESC(activate_child, "write 1 after preflight to create the active FGADC child");

static int gemini_fgadc_deactivate_param(const char *value,
					 const struct kernel_param *kp)
{
	bool requested;
	int ret;

	ret = kstrtobool(value, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gemini_fgadc_lock);
	if (!gemini_fgadc_pdev && !restore_pending) {
		mutex_unlock(&gemini_fgadc_lock);
		return -ENOENT;
	}
	ret = gemini_fgadc_deactivate_locked();
	mutex_unlock(&gemini_fgadc_lock);
	return ret;
}

static const struct kernel_param_ops gemini_fgadc_deactivate_ops = {
	.set = gemini_fgadc_deactivate_param,
};

module_param_cb(deactivate_child, &gemini_fgadc_deactivate_ops, NULL, 0200);
MODULE_PARM_DESC(deactivate_child, "write 1 to unregister child and trigger FGADC restore");

static int __init gemini_live_fgadc_init(void)
{
	int ret;

	mutex_lock(&gemini_fgadc_lock);
	ret = gemini_fgadc_preflight_locked();
	if (!ret && activate)
		ret = gemini_fgadc_activate_locked();
	mutex_unlock(&gemini_fgadc_lock);
	return ret;
}

static void __exit gemini_live_fgadc_exit(void)
{
	mutex_lock(&gemini_fgadc_lock);
	WARN_ON_ONCE(gemini_fgadc_pdev || restore_pending);
	if (gemini_fgadc_node)
		of_node_put(gemini_fgadc_node);
	if (gemini_pmic)
		put_device(gemini_pmic);
	mutex_unlock(&gemini_fgadc_lock);
}

module_init(gemini_live_fgadc_init);
module_exit(gemini_live_fgadc_exit);

MODULE_AUTHOR("Gemini PDA mainline bring-up project");
MODULE_DESCRIPTION("Fail-closed live MT6351 FGADC qualification bridge");
MODULE_LICENSE("GPL");
