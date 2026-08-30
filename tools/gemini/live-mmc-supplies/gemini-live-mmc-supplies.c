// SPDX-License-Identifier: GPL-2.0-only
/* Reversible VMCH/VMC ownership for live Gemini PDA MSDC1 probing. */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define GEMINI_MMC1_DEVICE	"11240000.mmc"
#define MT6351_LDO_EN		BIT(1)
#define MT6351_VMCH_VSEL_MASK	BIT(8)
#define MT6351_VMC_VSEL_MASK	GENMASK(10, 8)
#define MT6351_VMCH_3V_SEL	0
#define MT6351_VMC_3V_SEL	(6 << 8)

struct gemini_mmc_supply_snapshot {
	unsigned int vmch_con0;
	unsigned int vmch_vosel;
	unsigned int vmc_con0;
	unsigned int vmc_vosel;
};

static struct gemini_mmc_supply_snapshot saved;
static struct regmap *pmic_regmap;
static bool activate;
static bool supplies_owned;

module_param(activate, bool, 0400);
MODULE_PARM_DESC(activate, "explicitly own VMCH/VMC for live MSDC1 probing");

static int gemini_live_read_snapshot(struct gemini_mmc_supply_snapshot *state)
{
	int ret;

	ret = regmap_read(pmic_regmap, MT6351_LDO_VMCH_CON0,
			  &state->vmch_con0);
	if (ret)
		return ret;
	ret = regmap_read(pmic_regmap, MT6351_LDO_VMCH_VOSEL,
			  &state->vmch_vosel);
	if (ret)
		return ret;
	ret = regmap_read(pmic_regmap, MT6351_LDO_VMC_CON0,
			  &state->vmc_con0);
	if (ret)
		return ret;

	return regmap_read(pmic_regmap, MT6351_LDO_VMC_VOSEL,
			   &state->vmc_vosel);
}

static int gemini_live_restore_supplies(void)
{
	int ret;

	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMCH_VOSEL,
				 MT6351_VMCH_VSEL_MASK,
				 saved.vmch_vosel & MT6351_VMCH_VSEL_MASK);
	if (ret)
		return ret;
	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMC_VOSEL,
				 MT6351_VMC_VSEL_MASK,
				 saved.vmc_vosel & MT6351_VMC_VSEL_MASK);
	if (ret)
		return ret;
	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMC_CON0,
				 MT6351_LDO_EN,
				 saved.vmc_con0 & MT6351_LDO_EN);
	if (ret)
		return ret;

	return regmap_update_bits(pmic_regmap, MT6351_LDO_VMCH_CON0,
				  MT6351_LDO_EN,
				  saved.vmch_con0 & MT6351_LDO_EN);
}

static int gemini_live_activate_supplies(void)
{
	struct gemini_mmc_supply_snapshot state;
	int ret;

	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMCH_VOSEL,
				 MT6351_VMCH_VSEL_MASK, MT6351_VMCH_3V_SEL);
	if (ret)
		return ret;
	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMC_VOSEL,
				 MT6351_VMC_VSEL_MASK, MT6351_VMC_3V_SEL);
	if (ret)
		return ret;
	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMC_CON0,
				 MT6351_LDO_EN, MT6351_LDO_EN);
	if (ret)
		return ret;
	ret = regmap_update_bits(pmic_regmap, MT6351_LDO_VMCH_CON0,
				 MT6351_LDO_EN, MT6351_LDO_EN);
	if (ret)
		return ret;

	ret = gemini_live_read_snapshot(&state);
	if (ret)
		return ret;
	if (!(state.vmch_con0 & MT6351_LDO_EN) ||
	    !(state.vmc_con0 & MT6351_LDO_EN) ||
	    (state.vmch_vosel & MT6351_VMCH_VSEL_MASK) !=
		MT6351_VMCH_3V_SEL ||
	    (state.vmc_vosel & MT6351_VMC_VSEL_MASK) != MT6351_VMC_3V_SEL)
		return -EIO;

	return 0;
}

static int gemini_live_deactivate(const char *value,
				  const struct kernel_param *)
{
	struct device *mmc_dev;
	bool requested;
	int ret;

	ret = kstrtobool(value, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	if (!supplies_owned)
		return -ENOENT;

	mmc_dev = bus_find_device_by_name(&platform_bus_type, NULL,
					  GEMINI_MMC1_DEVICE);
	if (mmc_dev) {
		put_device(mmc_dev);
		return -EBUSY;
	}

	ret = gemini_live_restore_supplies();
	if (ret) {
		pr_err("gemini-live-mmc-supplies: restore failed: %d; retry is available\n",
		       ret);
		return ret;
	}

	supplies_owned = false;
	pr_info("gemini-live-mmc-supplies: restored VMCH/VMC snapshot\n");
	module_put(THIS_MODULE);

	return 0;
}

static const struct kernel_param_ops deactivate_ops = {
	.set = gemini_live_deactivate,
};

module_param_cb(deactivate, &deactivate_ops, NULL, 0200);
MODULE_PARM_DESC(deactivate, "write 1 after removing MSDC1 to restore VMCH/VMC");

static int __init gemini_live_mmc_supplies_init(void)
{
	struct mt6397_chip *pmic;
	struct device *regulator_dev;
	int restore_ret;
	int ret;

	if (!activate)
		return -EPERM;

	regulator_dev = bus_find_device_by_name(&platform_bus_type, NULL,
						"mt6351-regulator");
	if (!regulator_dev)
		return -EPROBE_DEFER;

	pmic = dev_get_drvdata(regulator_dev->parent);
	put_device(regulator_dev);
	if (!pmic || !pmic->regmap)
		return -ENODEV;
	pmic_regmap = pmic->regmap;

	ret = gemini_live_read_snapshot(&saved);
	if (ret)
		return ret;

	ret = gemini_live_activate_supplies();
	if (ret) {
		restore_ret = gemini_live_restore_supplies();
		if (!restore_ret)
			return ret;

		pr_err("gemini-live-mmc-supplies: activation failed: %d and restore failed: %d; retaining retry path\n",
		       ret, restore_ret);
	}

	supplies_owned = true;
	__module_get(THIS_MODULE);
	pr_info("gemini-live-mmc-supplies: VMCH=3.0V on, VMC=3.0V on; snapshot retained\n");

	return 0;
}

static void __exit gemini_live_mmc_supplies_exit(void)
{
	WARN_ON_ONCE(supplies_owned);
}

module_init(gemini_live_mmc_supplies_init);
module_exit(gemini_live_mmc_supplies_exit);

MODULE_AUTHOR("Gemini PDA mainline bring-up project");
MODULE_DESCRIPTION("Reversible live MSDC1 rail owner for Gemini PDA");
MODULE_LICENSE("GPL");
