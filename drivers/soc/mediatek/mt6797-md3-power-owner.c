// SPDX-License-Identifier: GPL-2.0-only
/* Optional, explicit C2K domain owner. Keeps TOPRGU reset asserted. Optional
 * AP-only CCIF preparation does not provide running RX/TX, PLL, execution,
 * runtime capability, firmware response or CSS.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mediatek-wdt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md3-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md3-power.h>
#include <linux/soc/mediatek/mt6797-md3-ccif.h>
#include <dt-bindings/reset/mt6797-resets.h>

static bool allow_domain_power;
module_param(allow_domain_power, bool, 0400);
MODULE_PARM_DESC(allow_domain_power,
	"allow one optional C2K domain transition while holding reset; no modem execution");

static bool allow_ccif_prepare;
module_param(allow_ccif_prepare, bool, 0400);
MODULE_PARM_DESC(allow_ccif_prepare, "allow AP3 SRAM/ring setup with CCIF IRQs and C2K reset held");

struct mt6797_md3_power_owner {
	struct mutex lock;
	struct reset_control *reset;
	struct mt6797_md3_ccif *ccif;
	bool ccif_requested;
	struct mt6797_md3_power_result result;
	bool reset_verified;
};

static int mt6797_md3_reset_spec(struct device *dev)
{
	struct of_phandle_args args;
	struct resource res;
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "resets", "#reset-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(dev->of_node, "resets", "#reset-cells", 0, &args);
	if (ret)
		return ret;
	if (args.args_count != 1 || args.args[0] != MT6797_TOPRGU_C2KSYS_SW_RST ||
	    !of_device_is_compatible(args.np, "mediatek,mt6797-wdt")) {
		ret = -EINVAL;
		goto out;
	}
	ret = of_address_to_resource(args.np, 0, &res);
	if (!ret && (res.start != 0x10007000 || resource_size(&res) != 0x100))
		ret = -ERANGE;
out:
	of_node_put(args.np);
	return ret;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6797_md3_power_owner *owner = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&owner->lock);
	len = sysfs_emit(buf, "domain_state=%u error=%d spm_may_have_written=%u initial_reset_verified=%u ccif_requested=%u ccif_status=%d execution_owned=0\n",
		owner->result.state, owner->result.error, owner->result.may_have_written,
		owner->reset_verified, owner->ccif_requested, mt6797_md3_ccif_status(owner->ccif));
	mutex_unlock(&owner->lock);
	return len;
}
static DEVICE_ATTR_RO(state);
static struct attribute *mt6797_md3_attrs[] = { &dev_attr_state.attr, NULL };
static const struct attribute_group mt6797_md3_group = { .attrs = mt6797_md3_attrs };

static int mt6797_md3_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_md3_power_owner *owner;
	struct mt6797_md3_lk_snapshot snapshot;
	int ret;

	if (!allow_domain_power ||
	    !device_property_read_bool(dev, "mediatek,experimental-md3-domain-power") ||
	    device_property_present(dev, "power-domains"))
		return -EPERM;
	ret = mt6797_md3_reset_spec(dev);
	if (ret)
		return ret;
	ret = mtk_wdt_require_boot_recovery(dev);
	if (ret)
		return ret;
	owner = devm_kzalloc(dev, sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	mutex_init(&owner->lock);
	platform_set_drvdata(pdev, owner);
	/* Allocate fallible diagnostic resources before any hardware mutation. */
	ret = devm_device_add_group(dev, &mt6797_md3_group);
	if (ret)
		return ret;
	owner->ccif_requested = device_property_read_bool(dev, "mediatek,prepare-ap-md3-ccif");
	if (owner->ccif_requested && !allow_ccif_prepare)
		return -EPERM;
	owner->reset = devm_reset_control_get_exclusive(dev, "c2k");
	if (IS_ERR(owner->reset))
		return PTR_ERR(owner->reset);
	ret = mt6797_md3_lk_handoff_get(&snapshot);
	if (ret)
		return ret;
	/* Metadata failure is known before any reset/SPM write. Do not substitute
	 * fallback MPU settings for a missing, cleared or mismatched LK table.
	 */
	if (snapshot.mpu_error)
		return snapshot.mpu_error;
	if (owner->ccif_requested) {
		ret = mt6797_md3_ccif_acquire(pdev, &snapshot, owner->reset, &owner->ccif);
		if (ret)
			return ret;
	}
	/* All fallible/deferred provider acquisition precedes the one-boot lease. */
	ret = mt6797_md1_cold_owner_md3_power_ready();
	if (ret)
		return ret;
	ret = mt6797_md1_cold_owner_acquire_md3(dev, &snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_owner_verify_md3_cold(dev);
	if (ret)
		return ret;
	/* Acquiring an exclusive reset handle does not change hardware. Holding
	 * the real reset here precedes the SPM reset release, including failure.
	 * Never deassert or power down automatically on probe/shutdown/removal.
	 */
	mutex_lock(&owner->lock);
	ret = reset_control_assert(owner->reset);
	if (!ret) {
		ret = reset_control_status(owner->reset);
		ret = ret < 0 ? ret : ret == 1 ? 0 : -EIO;
	}
	owner->reset_verified = !ret;
	owner->result.state = MT6797_MD3_POWER_FAILED;
	owner->result.error = ret;
	if (!ret)
		ret = mt6797_md1_cold_owner_power_md3(dev, owner->reset, &owner->result);
	if (!ret && owner->ccif_requested)
		ret = mt6797_md3_ccif_prepare(owner->ccif);
	if (ret) {
		owner->result.error = ret;
		owner->result.state = MT6797_MD3_POWER_FAILED;
	}
	/* Retain the supplier/reset leases even on a partial transition. Returning
	 * failure here would run devres cleanup and misrepresent an unowned domain.
	 * FAILED stays bound with no retry interface until a real cold restart.
	 */
	dev_info(dev, "optional C2K domain state=%u error=%d spm_may_have_written=%u initial_reset_verified=%u; execution unowned\n",
		owner->result.state, owner->result.error, owner->result.may_have_written,
		owner->reset_verified);
	mutex_unlock(&owner->lock);
	return 0;
}

static const struct of_device_id mt6797_md3_power_of_match[] = {
	{ .compatible = "mediatek,mt6797-md3-power-owner" }, {}
};
MODULE_DEVICE_TABLE(of, mt6797_md3_power_of_match);

static struct platform_driver mt6797_md3_power_driver = {
	.probe = mt6797_md3_power_probe,
	.driver = {
		.name = "mt6797-md3-power-owner",
		.of_match_table = mt6797_md3_power_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md3_power_driver);
MODULE_DESCRIPTION("Optional reset-held MT6797 C2K domain owner");
MODULE_LICENSE("GPL");
