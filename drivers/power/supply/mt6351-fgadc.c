// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6351 fuel-gauge ADC.
 *
 * The PMIC supplies instantaneous current and a coulomb counter. Optional
 * board-provided battery profiles and a charger supplying terminal voltage,
 * temperature and status allow software state-of-charge estimation. Model
 * Qmax is not exposed as measured or design capacity.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "mt6351-fgadc-model.h"

#define MT6351_FGADC_CLK_PDN_MASK	GENMASK(3, 2)
/* RG_FGADC_FT_CK_PDN, the third FGADC clock gate. */
#define MT6351_FGADC_FT_CLK_PDN		BIT(6)
#define MT6351_FGADC_CON0_LOW_MASK	GENMASK(7, 0)
#define MT6351_FGADC_CON0_HIGH_MASK	GENMASK(15, 8)
#define MT6351_FGADC_OSR_MASK		GENMASK(3, 0)
#define MT6351_FGADC_LATCHDATA_ST	BIT(10)
#define MT6351_FGADC_SW_READ_PRE	BIT(9)
#define MT6351_FGADC_SW_CLEAR		BIT(11)
/* The latch handshake: request, status and clear. None survive a capture. */
#define MT6351_FGADC_LATCH_BITS		(MT6351_FGADC_SW_READ_PRE | \
					 MT6351_FGADC_LATCHDATA_ST | \
					 MT6351_FGADC_SW_CLEAR)

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
	struct power_supply *charger;
	struct power_supply_desc psy_desc;
	struct mt6351_fg_model model;
	struct mt6351_fg_estimate estimate;
	/* Serializes estimates, publication and work scheduling/stop. */
	struct mutex model_lock;
	struct delayed_work model_work;
	bool model_active;
	int model_error;

	u32 init_control;
	u32 osr;
	u32 resistance_mohm;
	u32 board_base;
	u32 board_slope;
	u32 car_tune_percent;
	u32 trim_permille;

	unsigned int saved_clkpdn;
	unsigned int saved_ft_clkpdn;
	unsigned int saved_con0;
	unsigned int saved_osr;

	/* A latch timeout was reported; stay quiet until a capture succeeds. */
	bool stalled;
};

static int mt6351_fgadc_restore(struct mt6351_fgadc *fg)
{
	int ret;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON15,
				 MT6351_FGADC_OSR_MASK, fg->saved_osr);
	if (ret)
		return ret;

	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				 MT6351_FGADC_CON0_LOW_MASK, fg->saved_con0);
	if (ret)
		return ret;

	ret = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON3,
				 MT6351_FGADC_FT_CLK_PDN, fg->saved_ft_clkpdn);
	if (ret)
		return ret;

	return regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON2,
				  MT6351_FGADC_CLK_PDN_MASK, fg->saved_clkpdn);
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

	ret = regmap_read(fg->regmap, MT6351_TOP_CKPDN_CON3, &fg->saved_ft_clkpdn);
	if (ret)
		return ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &fg->saved_con0);
	if (ret)
		return ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON15, &fg->saved_osr);
	if (ret)
		return ret;

	/*
	 * The vendor sequence enables the analog and digital clocks first. The
	 * FT clock is left gated on the Gemini at handover, so enable it too.
	 * The analog VA18 supply must also be enabled before this sequence.
	 */
	ret = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON2,
				 MT6351_FGADC_CLK_PDN_MASK, 0);
	if (ret)
		goto restore;

	ret = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON3,
				 MT6351_FGADC_FT_CLK_PDN, 0);
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

	return 0;

restore:
	mt6351_fgadc_restore(fg);
	return ret;
}

/*
 * A latch that never asserts (or never clears) is a hardware state, not a
 * transient: every reader then waits the full poll timeout and fails the
 * same way.  Report it once, with the registers needed to tell a gated
 * clock or a disabled FGADC from a stuck handshake, and report recovery.
 * Called with fg->lock held.
 */
static void mt6351_fgadc_note_result(struct mt6351_fgadc *fg, int ret,
				     unsigned int saved_con0)
{
	unsigned int con0 = 0, clkpdn = 0, ft_clkpdn = 0, con15 = 0;

	if (ret != -ETIMEDOUT) {
		if (!ret && fg->stalled) {
			fg->stalled = false;
			dev_info(fg->dev, "FGADC latch recovered\n");
		}
		return;
	}
	if (fg->stalled)
		return;
	fg->stalled = true;
	regmap_read(fg->regmap, MT6351_FGADC_CON0, &con0);
	regmap_read(fg->regmap, MT6351_TOP_CKPDN_CON2, &clkpdn);
	regmap_read(fg->regmap, MT6351_TOP_CKPDN_CON3, &ft_clkpdn);
	regmap_read(fg->regmap, MT6351_FGADC_CON15, &con15);
	dev_warn(fg->dev,
		 "FGADC latch timed out; con0=%#06x (before %#06x) ckpdn_con2=%#06x ckpdn_con3=%#06x con15=%#06x; reads report no data until it recovers\n",
		 con0, saved_con0, clkpdn, ft_clkpdn, con15);
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
	/*
	 * Restore the other high-byte controls, but never re-issue a latch
	 * request or clear: the vendor sequence ends with both released, and
	 * echoing a stale request would leave the next capture a held latch.
	 */
	restore_ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
					 MT6351_FGADC_CON0_HIGH_MASK,
					 saved_con0 & MT6351_FGADC_CON0_HIGH_MASK &
					 ~MT6351_FGADC_LATCH_BITS);
	if (!ret)
		ret = restore_ret;
	mt6351_fgadc_note_result(fg, ret, saved_con0);
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

/* Read the charger and one coherent gauge snapshot before publishing. */
static int mt6351_fgadc_model_sample(struct mt6351_fgadc *fg)
{
	struct mt6351_fg_sample sample = {};
	union power_supply_propval value;
	s64 current_ua, counter;
	int ret;

	ret = power_supply_get_property(fg->charger, POWER_SUPPLY_PROP_VOLTAGE_NOW, &value);
	if (ret)
		return ret;
	sample.voltage_uv = value.intval;
	ret = power_supply_get_property(fg->charger, POWER_SUPPLY_PROP_TEMP, &value);
	if (ret)
		return ret;
	sample.temp = value.intval;
	ret = power_supply_get_property(fg->charger, POWER_SUPPLY_PROP_STATUS, &value);
	if (ret)
		return ret;
	sample.status = value.intval;
	ret = mt6351_fgadc_read(fg, &current_ua, &counter);
	if (ret)
		return ret == -ETIMEDOUT ? -ENODATA : ret;
	if (current_ua < INT_MIN || current_ua > INT_MAX || counter < INT_MIN || counter > INT_MAX)
		return -ERANGE;
	sample.current_ua = current_ua;
	sample.counter_uah = counter;
	sample.seconds = ktime_get_boottime_seconds();
	return mt6351_model_update(&fg->model, &fg->estimate, &sample);
}

static void mt6351_fgadc_model_work(struct work_struct *work)
{
	struct mt6351_fgadc *fg = container_of(to_delayed_work(work), struct mt6351_fgadc,
					     model_work);
	bool changed;
	int previous_error, previous_capacity;

	mutex_lock(&fg->model_lock);
	if (!fg->model_active) {
		mutex_unlock(&fg->model_lock);
		return;
	}
	previous_error = fg->model_error;
	previous_capacity = fg->estimate.capacity;
	fg->model_error = mt6351_fgadc_model_sample(fg);
	changed = previous_error != fg->model_error ||
		  previous_capacity != fg->estimate.capacity;
	queue_delayed_work(system_freezable_wq, &fg->model_work, 5 * HZ);
	mutex_unlock(&fg->model_lock);
	if (changed)
		power_supply_changed(fg->psy);
}

static int mt6351_fgadc_model_property(struct mt6351_fgadc *fg,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	u64 now;
	int ret;

	if (!fg->model.count)
		return -EINVAL;
	mutex_lock(&fg->model_lock);
	now = ktime_get_boottime_seconds();
	ret = fg->model_error;
	if (!ret && (!fg->estimate.initialized || now < fg->estimate.seconds ||
		     now - fg->estimate.seconds > MT6351_MODEL_MAX_GAP))
		ret = -ENODATA;
	if (!ret) {
		if (psp == POWER_SUPPLY_PROP_CAPACITY)
			val->intval = fg->estimate.capacity;
		else if (fg->estimate.ocv_valid)
			val->intval = fg->estimate.ocv_uv;
		else
			ret = -ENODATA;
	}
	mutex_unlock(&fg->model_lock);
	return ret;
}

static void mt6351_fgadc_model_schedule(struct mt6351_fgadc *fg)
{
	mutex_lock(&fg->model_lock);
	if (fg->model_active)
		mod_delayed_work(system_freezable_wq, &fg->model_work, HZ);
	mutex_unlock(&fg->model_lock);
}

static void mt6351_fgadc_model_stop(void *data)
{
	struct mt6351_fgadc *fg = data;

	mutex_lock(&fg->model_lock);
	fg->model_active = false;
	fg->estimate.initialized = false;
	fg->model_error = -ENODATA;
	mutex_unlock(&fg->model_lock);
	cancel_delayed_work_sync(&fg->model_work);
}

static void mt6351_fgadc_model_start(struct mt6351_fgadc *fg)
{
	mutex_lock(&fg->model_lock);
	if (fg->model.count) {
		fg->model_active = true;
		queue_delayed_work(system_freezable_wq, &fg->model_work, 0);
	}
	mutex_unlock(&fg->model_lock);
}

static int mt6351_fgadc_parse_model(struct mt6351_fgadc *fg)
{
	struct device *dev = fg->dev;
	struct device_node *np = dev->of_node;
	u32 config[MT6351_MODEL_PROFILES * 2];
	u32 entries[MT6351_MODEL_POINTS * 2];
	char name[48];
	int count, i, j, ret;

	if (!of_find_property(np, "mediatek,capacity-model", NULL))
		return 0;
	if (!fg->charger || !of_find_property(np, "monitored-battery", NULL))
		return -EINVAL;
	count = of_property_count_u32_elems(np, "mediatek,capacity-model");
	if (count < 2 || count > ARRAY_SIZE(config) || count % 2)
		return -EINVAL;
	ret = of_property_read_u32_array(np, "mediatek,capacity-model", config, count);
	if (ret)
		return ret;
	fg->model.count = count / 2;
	for (i = 0; i < fg->model.count; i++) {
		struct mt6351_fg_profile *p = &fg->model.profiles[i];
		struct power_supply_vbat_ri_table *table;
		s32 celsius = config[2 * i];

		if (celsius < -50 || celsius > 100)
			return -EINVAL;
		p->temp = celsius * 10;
		p->full_uah = config[2 * i + 1];
		snprintf(name, sizeof(name), "mediatek,resistance-table-%d", i);
		count = of_property_count_u32_elems(np, name);
		if (count < 4 || count > ARRAY_SIZE(entries) || count % 2)
			return -EINVAL;
		ret = of_property_read_u32_array(np, name, entries, count);
		if (ret)
			return ret;
		p->resistance_count = count / 2;
		table = devm_kcalloc(dev, p->resistance_count, sizeof(*table), GFP_KERNEL);
		if (!table)
			return -ENOMEM;
		for (j = 0; j < p->resistance_count; j++) {
			table[j].vbat_uv = entries[2 * j];
			table[j].ri_uohm = entries[2 * j + 1];
		}
		p->resistance = table;
	}
	return 0;
}

static int mt6351_fgadc_model_battery(struct mt6351_fgadc *fg)
{
	struct power_supply_battery_info *info = fg->psy->battery_info;
	int i, j;

	if (!fg->model.count)
		return 0;
	if (!info)
		return -EINVAL;
	for (i = 0; i < fg->model.count; i++) {
		struct mt6351_fg_profile *p = &fg->model.profiles[i];

		for (j = 0; j < POWER_SUPPLY_OCV_TEMP_MAX && info->ocv_table[j]; j++) {
			if (info->ocv_temp[j] != p->temp / 10)
				continue;
			p->ocv = info->ocv_table[j];
			p->ocv_count = info->ocv_table_size[j];
			break;
		}
	}
	return mt6351_model_validate(&fg->model);
}

static enum power_supply_property mt6351_fgadc_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static enum power_supply_property mt6351_fgadc_charger_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static enum power_supply_property mt6351_fgadc_model_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
};

static int mt6351_fgadc_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct mt6351_fgadc *fg = power_supply_get_drvdata(psy);
	s64 current_ua, charge_counter_uah;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		return mt6351_fgadc_model_property(fg, psp, val);
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "MediaTek";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MT6351 FGADC";
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_TEMP:
		if (!fg->charger)
			return -ENODATA;
		/* The charger measures the battery terminals and thermistor. */
		return power_supply_get_property(fg->charger, psp, val);
	case POWER_SUPPLY_PROP_STATUS:
		/* Termination and input state belong to the charger, not ADC noise. */
		if (fg->charger)
			return power_supply_get_property(fg->charger, psp, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		break;
	default:
		return -EINVAL;
	}

	ret = mt6351_fgadc_read(fg, &current_ua, &charge_counter_uah);
	/*
	 * A latch timeout has already been reported once by the capture.
	 * -ENODATA keeps the power-supply core from logging every sysfs read
	 * and lets a uevent still carry the static properties.
	 */
	if (ret == -ETIMEDOUT)
		return -ENODATA;
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

static void mt6351_fgadc_external_power_changed(struct power_supply *psy)
{
	mt6351_fgadc_model_schedule(power_supply_get_drvdata(psy));
	power_supply_changed(psy);
}

static int mt6351_fgadc_get_charger(struct mt6351_fgadc *fg)
{
	struct device *dev = fg->dev;
	int count;

	if (!of_find_property(dev->of_node, "power-supplies", NULL))
		return 0;
	count = of_count_phandle_with_args(dev->of_node, "power-supplies", NULL);
	if (count != 1)
		return count < 0 ? count : -EINVAL;

	fg->charger = devm_power_supply_get_by_reference(dev, "power-supplies");
	if (IS_ERR(fg->charger))
		return PTR_ERR(fg->charger);
	return fg->charger ? 0 : -EPROBE_DEFER;
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
	mutex_init(&fg->model_lock);
	INIT_DELAYED_WORK(&fg->model_work, mt6351_fgadc_model_work);
	fg->model_error = -ENODATA;

	ret = mt6351_fgadc_parse_dt(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing or unsafe FGADC configuration\n");

	/* Resolve the supplier before changing any gauge hardware. */
	ret = mt6351_fgadc_get_charger(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot acquire battery charger\n");

	ret = mt6351_fgadc_parse_model(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "invalid battery model configuration\n");

	/*
	 * The FGADC analog front end runs from VA18; without it the gauge
	 * never latches a sample. Vendor fgauge_initialization() enables it
	 * ("Enable VA2") before configuring the converter.
	 */
	ret = devm_regulator_get_enable(&pdev->dev, "vdda");
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable the FGADC analog supply\n");

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
	fg->psy_desc.num_properties = ARRAY_SIZE(mt6351_fgadc_properties);
	if (fg->charger) {
		fg->psy_desc.properties = mt6351_fgadc_charger_properties;
		fg->psy_desc.num_properties = ARRAY_SIZE(mt6351_fgadc_charger_properties);
	}
	if (fg->model.count) {
		fg->psy_desc.properties = mt6351_fgadc_model_properties;
		fg->psy_desc.num_properties = ARRAY_SIZE(mt6351_fgadc_model_properties);
	}
	fg->psy_desc.get_property = mt6351_fgadc_get_property;
	fg->psy_desc.external_power_changed = mt6351_fgadc_external_power_changed;
	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);
	fg->psy = devm_power_supply_register(&pdev->dev, &fg->psy_desc, &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(fg->psy),
				     "failed to register battery power supply\n");

	ret = mt6351_fgadc_model_battery(fg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "invalid battery model profiles\n");
	/* Cancel before devres unregisters the power supply or restores hardware. */
	ret = devm_add_action_or_reset(&pdev->dev, mt6351_fgadc_model_stop, fg);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, fg);
	mt6351_fgadc_model_start(fg);
	return 0;
}

static int mt6351_fgadc_suspend(struct device *dev)
{
	mt6351_fgadc_model_stop(dev_get_drvdata(dev));
	return 0;
}

static int mt6351_fgadc_resume(struct device *dev)
{
	mt6351_fgadc_model_start(dev_get_drvdata(dev));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6351_fgadc_pm_ops,
			       mt6351_fgadc_suspend, mt6351_fgadc_resume);

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
		.pm = pm_sleep_ptr(&mt6351_fgadc_pm_ops),
	},
};
module_platform_driver(mt6351_fgadc_driver);

MODULE_DESCRIPTION("MediaTek MT6351 FGADC battery monitor");
MODULE_LICENSE("GPL");
