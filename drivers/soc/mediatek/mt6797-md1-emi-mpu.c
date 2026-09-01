// SPDX-License-Identifier: GPL-2.0-only
/*
 * Default-disabled MT6797 MD1 secure EMI-MPU provider.
 *
 * The vendor's mt_emi_mpu_set_region_protection() issues SIP SET 0x82000209
 * with (start, end, permission | region << 27).  Vendor CCCI and public LK
 * discard the return register; this provider instead captures it and turns
 * every nonzero result into an irreversible boot-fatal latch.  There is no
 * GET/readback or inverse operation because neither is source-backed for MD1.
 *
 * LK programs and locks the MD1 regions before hand-off; this device's ATF
 * then answers any re-SET of a locked region with PERMISSION_DENY, and vendor
 * CCCI bypasses every one of these secure calls once LK did the setup
 * (modem_run_env_ready()).  A tuple that exactly matches the attested
 * LK-locked plan is already committed, so its SET is skipped.  Any mismatch
 * keeps the strict SET-or-fatal rule above unchanged.
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>

static bool enable_experimental_md1_emi_mpu;
module_param_named(enable_experimental_md1_emi_mpu,
		   enable_experimental_md1_emi_mpu, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_emi_mpu,
	"allow the default-disabled one-way MT6797 MD1 EMI-MPU provider");

struct mt6797_md1_emi_mpu {
	struct device *dev;
};

/* This deliberately outlives DT unbind/rebind attempts during one boot. */
static struct mt6797_md1_emi_mpu_core mt6797_md1_emi_mpu_boot_core =
	MT6797_MD1_EMI_MPU_CORE_INITIALIZER(mt6797_md1_emi_mpu_boot_core);
static bool mt6797_md1_emi_mpu_boot_module_pinned;
static DEFINE_MUTEX(mt6797_md1_emi_mpu_boot_pin_lock);

static int mt6797_md1_emi_mpu_begin(void *context)
{
	int ret = 0;

	mutex_lock(&mt6797_md1_emi_mpu_boot_pin_lock);
	if (!mt6797_md1_emi_mpu_boot_module_pinned) {
		if (!try_module_get(THIS_MODULE))
			ret = -ESHUTDOWN;
		else
			mt6797_md1_emi_mpu_boot_module_pinned = true;
	}
	mutex_unlock(&mt6797_md1_emi_mpu_boot_pin_lock);
	return ret;
}

/*
 * Skip authority: the published LK plan must carry identical values for this
 * region AND the separate read-only observation must attest that LK locked
 * the remap/MPU setup.  Missing or rejected evidence means no skip; the
 * caller then performs the secure SET with its unchanged error contract.
 */
static bool mt6797_md1_emi_mpu_lk_locked_match(u64 start, u64 end,
					       u32 region_permission)
{
	struct mt6797_md1_remap_observation observation;
	struct mt6797_md1_mpu_remap_plan plan;

	if (mt6797_md1_lk_handoff_plan_get(&plan) ||
	    mt6797_md1_remap_observation_get(&observation))
		return false;
	if (observation.classification ||
	    !observation.attestation.lk_lock_observed ||
	    !observation.attestation.secure_mpu_unobservable ||
	    observation.attestation.mode == MT6797_MD1_REMAP_ATTEST_NONE)
		return false;
	return mt6797_md1_emi_mpu_plan_entry_matches(&plan, start, end,
						     region_permission);
}

static int mt6797_md1_emi_mpu_set(void *context, u64 start, u64 end,
				  u32 region_permission)
{
	struct mt6797_md1_emi_mpu *mpu = context;
	struct arm_smccc_res res;
	long status;

	if (mt6797_md1_emi_mpu_lk_locked_match(start, end, region_permission)) {
		dev_info(mpu->dev,
			 "MD1 EMI-MPU region %u [%#llx-%#llx] permission %#07x already locked by LK with identical values; skipping secure SET, mirroring vendor bypass\n",
			 region_permission >> MT6797_MD1_EMI_MPU_REGION_SHIFT,
			 start, end,
			 region_permission & MT6797_MD1_EMI_MPU_PERMISSION_MASK);
		return 0;
	}

	arm_smccc_smc(MT6797_MD1_EMI_MPU_SMC_SET, start, end, region_permission,
		      0, 0, 0, 0, &res);
	status = (long)res.a0;
	if (!status)
		return 0;
	if (status < 0 && status >= -MAX_ERRNO)
		return status;
	dev_crit(mpu->dev,
		 "MD1 EMI-MPU SIP status %#lx for region-permission %#x; reboot required\n",
		 status, region_permission);
	return -EIO;
}

static int mt6797_md1_emi_mpu_apply_provider(
	struct mt6797_md1_emi_mpu *mpu,
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	int ret;

	ret = mt6797_md1_emi_mpu_core_apply(&mt6797_md1_emi_mpu_boot_core,
		mt6797_md1_emi_mpu_begin, mt6797_md1_emi_mpu_set, mpu, plan, phase,
		entries, count);
	if (ret)
		dev_crit(mpu->dev,
			 "MD1 secure MPU %s failed (%d), state %u; reset/reboot required before any MD1 release\n",
			 phase == MT6797_MD1_EMI_MPU_INITIAL ? "initial" : "stage-one",
			 ret, mt6797_md1_emi_mpu_core_state(&mt6797_md1_emi_mpu_boot_core));
	return ret;
}

int mt6797_md1_emi_mpu_apply(struct device *consumer,
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct mt6797_md1_emi_mpu *mpu;
	int ret;

	if (!consumer || !consumer->of_node)
		return -EINVAL;
	node = of_parse_phandle(consumer->of_node, "mediatek,emi-mpu", 0);
	if (!node)
		return -EINVAL;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;

	/* Hold the provider's devres-backed state across this one secure call. */
	device_lock(&pdev->dev);
	mpu = platform_get_drvdata(pdev);
	if (!mpu)
		ret = -EPROBE_DEFER;
	else
		ret = mt6797_md1_emi_mpu_apply_provider(mpu, plan, phase, entries,
						  count);
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_emi_mpu_apply);

void mt6797_md1_emi_mpu_latch_fatal(void)
{
	mt6797_md1_emi_mpu_core_latch_fatal(&mt6797_md1_emi_mpu_boot_core);
}
EXPORT_SYMBOL_GPL(mt6797_md1_emi_mpu_latch_fatal);

bool mt6797_md1_emi_mpu_is_fatal(void)
{
	return mt6797_md1_emi_mpu_core_state(&mt6797_md1_emi_mpu_boot_core) ==
		MT6797_MD1_EMI_MPU_FATAL;
}
EXPORT_SYMBOL_GPL(mt6797_md1_emi_mpu_is_fatal);

static int mt6797_md1_emi_mpu_probe(struct platform_device *pdev)
{
	struct mt6797_md1_emi_mpu *mpu;

	if (!READ_ONCE(enable_experimental_md1_emi_mpu) ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,experimental-md1-emi-mpu"))
		return dev_err_probe(&pdev->dev, -EPERM,
			"MD1 EMI-MPU needs explicit module and DT gates\n");
	if (mt6797_md1_emi_mpu_is_fatal())
		return dev_err_probe(&pdev->dev, -EUCLEAN,
			"MD1 EMI-MPU boot latch is fatal; reboot required\n");
	mpu = devm_kzalloc(&pdev->dev, sizeof(*mpu), GFP_KERNEL);
	if (!mpu)
		return -ENOMEM;
	mpu->dev = &pdev->dev;
	platform_set_drvdata(pdev, mpu);
	dev_info(&pdev->dev,
		 "MD1 secure EMI-MPU provider armed; no transaction is issued by probe\n");
	return 0;
}

static const struct of_device_id mt6797_md1_emi_mpu_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-emi-mpu" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_emi_mpu_of_match);

static struct platform_driver mt6797_md1_emi_mpu_driver = {
	.probe = mt6797_md1_emi_mpu_probe,
	.driver = {
		.name = "mt6797-md1-emi-mpu",
		.of_match_table = mt6797_md1_emi_mpu_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_emi_mpu_driver);

MODULE_DESCRIPTION("Default-disabled one-way MT6797 MD1 secure EMI-MPU provider");
MODULE_LICENSE("GPL");
