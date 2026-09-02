// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6351 fuel-gauge ADC.
 *
 * This is deliberately a small battery power-supply. The PMIC can provide a
 * calibrated instantaneous current and a coulomb counter, but it cannot
 * establish a battery's design capacity or an OCV-to-capacity profile. Those
 * values therefore remain absent rather than being guessed from a handset
 * family or a charger configuration.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MT6351_FGADC_CLK_PDN_MASK	GENMASK(3, 2)
#define MT6351_FGADC_CON0_LOW_MASK	GENMASK(7, 0)
#define MT6351_FGADC_CON0_HIGH_MASK	GENMASK(15, 8)
#define MT6351_FGADC_OSR_MASK		GENMASK(3, 0)
#define MT6351_FGADC_LATCHDATA_ST	BIT(10)
#define MT6351_FGADC_SW_READ_PRE	BIT(9)
#define MT6351_FGADC_SW_CLEAR		BIT(11)
#define MT6351_BATON_EN			BIT(0)
#define MT6351_BATON_TDET_EN		BIT(2)
#define MT6351_BATON_UNDET		BIT(1)

/* Vendor UNIT_FGCURRENT (158122) expressed as nanoamps per raw count. */
#define MT6351_FGADC_CURRENT_LSB_NA	1581220
/* Vendor FG_CAR [28:14] LSB, expressed as 359.86 microamp-hours. */
#define MT6351_FGADC_CAR_LSB_NAH	35986

struct mt6351_fgadc {
	struct device *dev;
	struct regmap *regmap;
	/* Serializes the FGADC latch transaction and configuration restore. */
	struct mutex lock;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;

	u32 init_control;
	u32 osr;
	u32 resistance_mohm;
	u32 board_base;
	u32 board_slope;
	u32 car_tune_percent;
	u32 trim_permille;
	bool initialize_battery_detection;

	unsigned int saved_clkpdn;
	unsigned int saved_con0;
	unsigned int saved_osr;
	unsigned int saved_baton_control;
};

static int mt6351_fgadc_restore(struct mt6351_fgadc *fg)
{
	int ret = 0, tmp;

	if (fg->initialize_battery_detection) {
		tmp = regmap_update_bits(fg->regmap, MT6351_CHR_CON7,
					 MT6351_BATON_EN,
					 fg->saved_baton_control);
		if (tmp)
			ret = tmp;
		tmp = regmap_update_bits(fg->regmap, MT6351_CHR_CON7,
					 MT6351_BATON_TDET_EN,
					 fg->saved_baton_control);
		if (tmp && !ret)
			ret = tmp;
	}

	tmp = regmap_update_bits(fg->regmap, MT6351_FGADC_CON15,
				 MT6351_FGADC_OSR_MASK, fg->saved_osr);
	if (tmp && !ret)
		ret = tmp;

	tmp = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				 MT6351_FGADC_CON0_LOW_MASK, fg->saved_con0);
	if (tmp && !ret)
		ret = tmp;

	tmp = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON2,
				 MT6351_FGADC_CLK_PDN_MASK, fg->saved_clkpdn);
	if (tmp && !ret)
		ret = tmp;

	return ret;
}

static void mt6351_fgadc_restore_action(void *data)
{
	struct mt6351_fgadc *fg = data;
	int ret;

	mutex_lock(&fg->lock);
	ret = mt6351_fgadc_restore(fg);
	mutex_unlock(&fg->lock);
	if (ret)
		dev_warn(fg->dev, "failed to restore FGADC configuration: %d\n", ret);
}

static int mt6351_fgadc_initialize(struct mt6351_fgadc *fg)
{
	int ret;

	ret = regmap_read(fg->regmap, MT6351_TOP_CKPDN_CON2, &fg->saved_clkpdn);
	if (ret)
		return ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &fg->saved_con0);
	if (ret)
		return ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON15, &fg->saved_osr);
	if (ret)
		return ret;
	if (fg->initialize_battery_detection) {
		ret = regmap_read(fg->regmap, MT6351_CHR_CON7,
				  &fg->saved_baton_control);
		if (ret)
			return ret;
	}

	/* The vendor sequence enables the analog and digital clocks first. */
	ret = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON2,
				 MT6351_FGADC_CLK_PDN_MASK, 0);
	if (ret)
		goto restore;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				 MT6351_FGADC_CON0_LOW_MASK, fg->init_control);
	if (ret)
		goto restore;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				 MT6351_FGADC_CON0_LOW_MASK,
				 fg->init_control | BIT(0));
	if (ret)
		goto restore;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON15,
				 MT6351_FGADC_OSR_MASK, fg->osr);
	if (ret)
		goto restore;

	if (fg->initialize_battery_detection) {
		/* This is the battery-presence sequence used by MT6797 LK. */
		ret = regmap_update_bits(fg->regmap, MT6351_CHR_CON7,
					 MT6351_BATON_TDET_EN,
					 MT6351_BATON_TDET_EN);
		if (ret)
			goto restore;
		ret = regmap_update_bits(fg->regmap, MT6351_CHR_CON7,
					 MT6351_BATON_EN, MT6351_BATON_EN);
		if (ret)
			goto restore;
	}

	return 0;

restore:
	mt6351_fgadc_restore(fg);
	return ret;
}

/*
 * Request a latch and read both ADC current and CAR while it is held. This is
 * the vendor's read-pre/latch/clear ordering, except the prior high byte
 * is restored instead of assuming it was zero.
 */
static int mt6351_fgadc_capture(struct mt6351_fgadc *fg,
				unsigned int *current_raw,
				unsigned int *car_hi, unsigned int *car_lo)
{
	unsigned int con0, saved_con0;
	int ret, clear_ret, restore_ret;

	mutex_lock(&fg->lock);
	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &saved_con0);
	if (ret)
		goto out_unlock;
	con0 = saved_con0;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				 MT6351_FGADC_CON0_HIGH_MASK,
				 MT6351_FGADC_SW_READ_PRE);
	if (ret)
		goto out_restore;

	ret = regmap_read_poll_timeout(fg->regmap, MT6351_FGADC_CON0, con0,
				       con0 & MT6351_FGADC_LATCHDATA_ST,
				       100, 100000);
	if (ret)
		goto out_clear;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON11, current_raw);
	if (ret)
		goto out_clear;
	ret = regmap_read(fg->regmap, MT6351_FGADC_CON1, car_hi);
	if (ret)
		goto out_clear;
	ret = regmap_read(fg->regmap, MT6351_FGADC_CON2, car_lo);

out_clear:
	/* Clear the latch even if one of the data reads failed. */
	clear_ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				       MT6351_FGADC_CON0_HIGH_MASK,
				       MT6351_FGADC_SW_CLEAR);
	if (!clear_ret)
		clear_ret = regmap_read_poll_timeout(fg->regmap, MT6351_FGADC_CON0,
						     con0,
						     !(con0 & MT6351_FGADC_LATCHDATA_ST),
						     100, 100000);
	if (!ret)
		ret = clear_ret;
out_restore:
	restore_ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
					 MT6351_FGADC_CON0_HIGH_MASK,
					 saved_con0 & MT6351_FGADC_CON0_HIGH_MASK);
	if (!ret)
		ret = restore_ret;
out_unlock:
	mutex_unlock(&fg->lock);
	return ret;
}

static s64 mt6351_fgadc_apply_current_calibration(struct mt6351_fgadc *fg,
						  s64 value_ua)
{
	value_ua = div_s64(value_ua * 20, fg->resistance_mohm);
	value_ua *= fg->board_base;
	value_ua += fg->board_slope / 2;
	value_ua = div_s64(value_ua, fg->board_slope);
	value_ua = div_s64(value_ua * fg->car_tune_percent, 100);
	return div_s64(value_ua * fg->trim_permille, 1000);
}

static s64 mt6351_fgadc_apply_counter_calibration(struct mt6351_fgadc *fg,
						  s64 value_uah)
{
	value_uah = div_s64(value_uah * 20, fg->resistance_mohm);
	value_uah = div_s64(value_uah * fg->car_tune_percent, 100);
	return div_s64(value_uah * fg->trim_permille, 1000);
}

static int mt6351_fgadc_read(struct mt6351_fgadc *fg, s64 *current_ua,
			     s64 *charge_counter_uah)
{
	unsigned int raw_current, car_hi, car_lo;
	s64 fg_current, counter, calibrated;
	u32 raw_car;
	int ret;

	ret = mt6351_fgadc_capture(fg, &raw_current, &car_hi, &car_lo);
	if (ret)
		return ret;

	if (raw_current > 0x7fff)
		fg_current = (s64)raw_current - 0xffff;
	else
		fg_current = raw_current;

	/* The vendor driver truncates to mA before applying board calibration. */
	calibrated = fg_current < 0 ? -fg_current : fg_current;
	calibrated *= MT6351_FGADC_CURRENT_LSB_NA;
	calibrated = div_s64(calibrated, 1000000) * 1000;
	calibrated = mt6351_fgadc_apply_current_calibration(fg, calibrated);
	fg_current = fg_current < 0 ? -calibrated : calibrated;

	raw_car = (car_lo >> 11) | ((car_hi & 0x0fff) << 5);
	if (!raw_car || raw_car == 0x1ffff)
		counter = 0;
	else if (car_hi & BIT(15))
		counter = (s64)raw_car - 0x1ffff;
	else
		counter = raw_car;

	/* Match the vendor's two-step 359.86 uAh/count rounding before OSR. */
	if (counter < 0) {
		counter = div_s64(-counter * MT6351_FGADC_CAR_LSB_NAH, 10);
		counter = div_s64(counter + 5, 10);
		counter = div_s64(counter, fg->osr);
		counter = -mt6351_fgadc_apply_counter_calibration(fg, counter);
	} else {
		counter = div_s64(counter * MT6351_FGADC_CAR_LSB_NAH, 10);
		counter = div_s64(counter + 5, 10);
		counter = div_s64(counter, fg->osr);
		counter = mt6351_fgadc_apply_counter_calibration(fg, counter);
	}

	*current_ua = fg_current;
	*charge_counter_uah = counter;
	return 0;
}

static enum power_supply_property mt6351_fgadc_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_PRESENT,
};

static int mt6351_fgadc_battery_present(struct mt6351_fgadc *fg, int *present)
{
	unsigned int value;
	int ret;

	ret = regmap_read(fg->regmap, MT6351_CHR_CON44, &value);
	if (ret)
		return ret;

	*present = !(value & MT6351_BATON_UNDET);
	return 0;
}

static int mt6351_fgadc_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct mt6351_fgadc *fg = power_supply_get_drvdata(psy);
	s64 current_ua, charge_counter_uah;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "MediaTek";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MT6351 FGADC";
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		if (!fg->initialize_battery_detection)
			return -ENODATA;
		return mt6351_fgadc_battery_present(fg, &val->intval);
	default:
		break;
	}

	ret = mt6351_fgadc_read(fg, &current_ua, &charge_counter_uah);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = current_ua > 0 ? POWER_SUPPLY_STATUS_CHARGING :
			current_ua < 0 ? POWER_SUPPLY_STATUS_DISCHARGING :
			POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = current_ua;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = charge_counter_uah;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mt6351_fgadc_parse_dt(struct mt6351_fgadc *fg)
{
	struct device_node *np = fg->dev->of_node;
	int ret;

	if (!of_property_read_bool(np, "mediatek,initialize-fgadc"))
		return -EPERM;

	ret = of_property_read_u32(np, "mediatek,fgadc-init-control",
				   &fg->init_control);
	if (ret || fg->init_control & BIT(0))
		return -EINVAL;

	ret = of_property_read_u32(np, "mediatek,fgadc-osr", &fg->osr);
	if (ret || !fg->osr || fg->osr > MT6351_FGADC_OSR_MASK)
		return -EINVAL;

	ret = of_property_read_u32(np, "mediatek,fg-resistance-milliohm",
				   &fg->resistance_mohm);
	if (ret || !fg->resistance_mohm)
		return -EINVAL;
	ret = of_property_read_u32(np, "mediatek,fg-board-base", &fg->board_base);
	if (ret || !fg->board_base)
		return -EINVAL;
	ret = of_property_read_u32(np, "mediatek,fg-board-slope",
				   &fg->board_slope);
	if (ret || !fg->board_slope)
		return -EINVAL;
	ret = of_property_read_u32(np, "mediatek,car-tune",
				   &fg->car_tune_percent);
	if (ret || !fg->car_tune_percent)
		return -EINVAL;
	ret = of_property_read_u32(np, "mediatek,fg-trim-permille",
				   &fg->trim_permille);
	if (ret || !fg->trim_permille)
		return -EINVAL;

	fg->initialize_battery_detection =
		of_property_read_bool(np, "mediatek,initialize-battery-detection");

	return 0;
}

static int mt6351_fgadc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6351 = dev_get_drvdata(pdev->dev.parent);
	struct mt6351_fgadc *fg;
	struct power_supply_config psy_cfg = {};
	int ret;

	if (!mt6351 || !mt6351->regmap)
		return -ENODEV;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->dev = &pdev->dev;
	fg->regmap = mt6351->regmap;
	mutex_init(&fg->lock);

	ret = mt6351_fgadc_parse_dt(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing or unsafe FGADC configuration\n");

	mutex_lock(&fg->lock);
	ret = mt6351_fgadc_initialize(fg);
	mutex_unlock(&fg->lock);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to initialize FGADC\n");

	ret = devm_add_action_or_reset(&pdev->dev,
				       mt6351_fgadc_restore_action, fg);
	if (ret)
		return ret;

	fg->psy_desc.name = "mt6351-fgadc";
	fg->psy_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	fg->psy_desc.properties = mt6351_fgadc_properties;
	fg->psy_desc.num_properties = ARRAY_SIZE(mt6351_fgadc_properties) -
		!fg->initialize_battery_detection;
	fg->psy_desc.get_property = mt6351_fgadc_get_property;
	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);
	fg->psy = devm_power_supply_register(&pdev->dev, &fg->psy_desc, &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(fg->psy),
				     "failed to register battery power supply\n");

	platform_set_drvdata(pdev, fg);
	return 0;
}

static const struct of_device_id mt6351_fgadc_of_match[] = {
	{ .compatible = "mediatek,mt6351-fgadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_fgadc_of_match);

static struct platform_driver mt6351_fgadc_driver = {
	.probe = mt6351_fgadc_probe,
	.driver = {
		.name = "mt6351-fgadc",
		.of_match_table = mt6351_fgadc_of_match,
	},
};
module_platform_driver(mt6351_fgadc_driver);

MODULE_DESCRIPTION("MediaTek MT6351 FGADC battery monitor");
MODULE_LICENSE("GPL");
