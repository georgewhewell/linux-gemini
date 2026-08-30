// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot MT6797 G2d post-patch VCN33 experiment.  It runs only inside the
 * bootstrap's post-full WMT callback: efuse read, both initially-off VCN33
 * rails at the fused selector, RF calibration, exact reverse rail restore,
 * then the vendor's WMT coexistence command.  Any WMT or restoration failure
 * lets the surrounding one-shot controller fence BTIF and requires reboot.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-runtime.h>
#include <linux/soc/mediatek/mt6797-consys-vcn33.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_G2D_TIMEOUT msecs_to_jiffies(3000)

static bool enable_experimental_g2d_postpatch_owner;
module_param_named(enable_experimental_g2d_postpatch_owner,
		enable_experimental_g2d_postpatch_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_g2d_postpatch_owner,
	"allow one MT6797 G2d fused-VCN33 post-patch experiment (default: false)");

struct mt6797_g2d_rail {
	struct regulator *reg;
	int original_uv;
	bool enabled;
	bool voltage_changed;
};
struct mt6797_g2d_owner { struct device *dev; struct mt6797_g2d_rail bt, wifi; };

static const u8 g2d_calibrate[] = { 1 };
static const u8 g2d_calibrate_reply[] = { 0, 1 };
static const u8 g2d_coex[] = { 1, 1 };
static const u8 g2d_ok[] = { 0 };
static const struct mtk_wmt_match g2d_calibrate_match = {
	.opcode = 0x14, .data = g2d_calibrate_reply, .data_len = sizeof(g2d_calibrate_reply),
};
static const struct mtk_wmt_match g2d_coex_match = {
	.opcode = 0x10, .data = g2d_ok, .data_len = sizeof(g2d_ok),
};

static int g2d_prepare_rail(struct mt6797_g2d_rail *rail, int uv)
{
	int ret;

	rail->original_uv = regulator_get_voltage(rail->reg);
	if (rail->original_uv < 0)
		return rail->original_uv;
	ret = regulator_is_enabled(rail->reg);
	if (ret < 0)
		return ret;
	/* A pre-existing consumer makes a transient selector change non-reversible. */
	if (ret)
		return -EBUSY;
	rail->enabled = false;
	ret = regulator_set_voltage(rail->reg, uv, uv);
	if (ret)
		return ret;
	rail->voltage_changed = true;
	ret = regulator_enable(rail->reg);
	if (ret)
		return ret;
	rail->enabled = true;
	return 0;
}

static int g2d_restore_rail(struct mt6797_g2d_rail *rail)
{
	int ret = 0, tmp;

	if (rail->enabled) {
		tmp = regulator_disable(rail->reg);
		if (tmp)
			ret = tmp;
		rail->enabled = false;
	}
	if (rail->voltage_changed) {
		tmp = regulator_set_voltage(rail->reg, rail->original_uv,
					  rail->original_uv);
		if (!ret && tmp)
			ret = tmp;
		rail->voltage_changed = false;
	}
	return ret;
}

static int mt6797_g2d_postpatch_stage(void *context,
		const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_g2d_owner *owner = context;
	u8 command[] = { 4, 0 }, reply[2], level;
	int uv, ret, cleanup;

	ret = mt6797_btif_stp_bootstrap_full_wmt_transaction_capture(lease, 0x12,
		command, sizeof(command), 0x12, sizeof(reply), reply, sizeof(reply),
		MT6797_G2D_TIMEOUT);
	if (ret)
		return ret;
	if (reply[0] != 4)
		return -EPROTO;
	level = reply[1] & 3;
	ret = mt6797_consys_vcn33_level_to_uv(level, &uv);
	if (ret)
		return ret;
	dev_info(owner->dev, "G2d efuse VCN33 level %u -> %duV\n", level, uv);
	ret = g2d_prepare_rail(&owner->bt, uv);
	if (ret)
		goto out;
	ret = g2d_prepare_rail(&owner->wifi, uv);
	if (ret)
		goto out;
	ret = mt6797_btif_stp_bootstrap_full_wmt_transaction(lease, 0x14,
		g2d_calibrate, sizeof(g2d_calibrate), &g2d_calibrate_match,
		MT6797_G2D_TIMEOUT);
out:
	cleanup = g2d_restore_rail(&owner->wifi);
	if (!ret)
		ret = cleanup;
	cleanup = g2d_restore_rail(&owner->bt);
	if (!ret)
		ret = cleanup;
	if (ret)
		return ret;
	return mt6797_btif_stp_bootstrap_full_wmt_transaction(lease, 0x10,
		g2d_coex, sizeof(g2d_coex), &g2d_coex_match, MT6797_G2D_TIMEOUT);
}

static int g2d_supplier(struct device *dev, const char *prop, struct device **out)
{
	struct device_node *node = of_parse_phandle(dev->of_node, prop, 0);
	struct platform_device *pdev;
	if (!node)
		return -EINVAL;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	*out = &pdev->dev;
	return 0;
}

static int mt6797_g2d_postpatch_owner_probe(struct platform_device *pdev)
{
	struct mt6797_g2d_owner *owner;
	struct device *runtime, *controller;
	struct mt6797_consys_runtime_resource_lease resources = { };
	struct mt6797_consys_runtime_provider_lease identity = { };
	struct mt6797_consys_postid_owner postid;
	bool postid_init = false;
	bool controller_active = false;
	int ret, cleanup;

	if (!READ_ONCE(enable_experimental_g2d_postpatch_owner) ||
	    !device_property_read_bool(&pdev->dev, "mediatek,experimental-g2d-postpatch-owner"))
		return -EPERM;
	owner = devm_kzalloc(&pdev->dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	owner->dev = &pdev->dev;
	owner->bt.reg = devm_regulator_get(&pdev->dev, "vcn33-bt");
	if (IS_ERR(owner->bt.reg))
		return PTR_ERR(owner->bt.reg);
	owner->wifi.reg = devm_regulator_get(&pdev->dev, "vcn33-wifi");
	if (IS_ERR(owner->wifi.reg))
		return PTR_ERR(owner->wifi.reg);
	if (owner->bt.reg == owner->wifi.reg)
		return -EINVAL;
	ret = g2d_supplier(&pdev->dev, "mediatek,runtime-provider", &runtime);
	if (ret)
		return ret;
	ret = g2d_supplier(&pdev->dev, "mediatek,controller", &controller);
	if (ret)
		goto put_runtime;
	ret = mt6797_consys_emi_mpu_claim(&pdev->dev);
	if (ret)
		goto put_controller;
	ret = mt6797_consys_runtime_resource_lease_acquire(runtime, &resources);
	if (ret)
		goto put_controller;
	ret = mt6797_consys_runtime_platform_identity_acquire(&resources, &identity);
	if (ret)
		goto resources;
	ret = mt6797_consys_runtime_postid_owner_init(runtime, identity.identity, &postid);
	if (ret)
		goto identity;
	postid_init = true;
	ret = mt6797_consys_postid_start(&postid, identity.identity, true);
	if (ret)
		goto postid;
	ret = mt6797_consys_runtime_controller_start_with_postpatch_stage(controller,
		mt6797_g2d_postpatch_stage, owner);
	if (!ret)
		controller_active = true;
postid:
	if (controller_active)
		mt6797_consys_runtime_controller_stop(controller);
	if (postid_init && mt6797_consys_postid_state(&postid) == MT6797_CONSYS_POSTID_ACTIVE) {
		cleanup = mt6797_consys_postid_stop(&postid);
		if (!ret)
			ret = cleanup;
	}
	if (postid_init && mt6797_consys_postid_state(&postid) != MT6797_CONSYS_POSTID_IDLE) {
		mt6797_consys_emi_mpu_latch_fatal();
		if (!ret)
			ret = -EIO;
	}
identity:
	cleanup = mt6797_consys_runtime_platform_identity_release(&identity);
	if (!ret)
		ret = cleanup;
resources:
	cleanup = mt6797_consys_runtime_resource_lease_release(&resources);
	if (!ret)
		ret = cleanup;
put_controller:
	put_device(controller);
put_runtime:
	put_device(runtime);
	return ret;
}

static const struct of_device_id mt6797_g2d_postpatch_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-g2d-postpatch-owner" }, {}
};
static struct platform_driver mt6797_g2d_postpatch_owner_driver = {
	.probe = mt6797_g2d_postpatch_owner_probe,
	.driver = { .name = "mt6797-consys-g2d-postpatch-owner", .of_match_table = mt6797_g2d_postpatch_owner_of_match, .suppress_bind_attrs = true },
};
module_platform_driver(mt6797_g2d_postpatch_owner_driver);
MODULE_LICENSE("GPL");
