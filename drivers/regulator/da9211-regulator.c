// SPDX-License-Identifier: GPL-2.0+
//
// da9211-regulator.c - Regulator device driver for DA9211/DA9212
// /DA9213/DA9223/DA9214/DA9224/DA9215/DA9225
// Copyright (C) 2015  Dialog Semiconductor Ltd.

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regmap.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/da9211.h>
#include <dt-bindings/regulator/dlg,da9211-regulator.h>
#include "da9211-regulator.h"

/* DEVICE IDs */
#define DA9211_DEVICE_ID	0x22
#define DA9213_DEVICE_ID	0x23
#define DA9215_DEVICE_ID	0x24

#define DA9214_LEGACY_PRIMARY_ADDR	0x68
#define DA9214_LEGACY_PAGE2_ADDR		0x69

/* DA9211 REGULATOR IDs */
#define DA9211_ID_BUCKA	0
#define DA9211_ID_BUCKB	1

struct da9211 {
	struct device *dev;
	struct regmap *regmap;
	struct i2c_client *page2;
	struct da9211_pdata *pdata;
	struct regulator_dev *rdev[DA9211_MAX_REGULATORS];
	int num_regulator;
	int chip_irq;
	int chip_id;
	bool legacy;
};

struct da9211_variant {
	bool legacy;
};

static const struct da9211_variant da9211_paged_variant;

static const struct da9211_variant da9214_legacy_variant = {
	.legacy = true,
};

static const struct regmap_range_cfg da9211_regmap_range[] = {
	{
		.selector_reg = DA9211_REG_PAGE_CON,
		.selector_mask  = DA9211_REG_PAGE_MASK,
		.selector_shift = DA9211_REG_PAGE_SHIFT,
		.window_start = 0,
		.window_len = 256,
		.range_min = 0,
		.range_max = 5*128,
	},
};

static bool da9211_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case DA9211_REG_STATUS_A:
	case DA9211_REG_STATUS_B:
	case DA9211_REG_EVENT_A:
	case DA9211_REG_EVENT_B:
		return true;
	}
	return false;
}

static const struct regmap_config da9211_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 5 * 128,
	.volatile_reg = da9211_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
	.ranges = da9211_regmap_range,
	.num_ranges = ARRAY_SIZE(da9211_regmap_range),
};

/* Legacy application pages 0 and 1 share one flat 8-bit address space. */
static const struct regmap_config da9214_legacy_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

/* Default limits measured in millivolts and milliamps */
#define DA9211_MIN_MV		300
#define DA9211_MAX_MV		1570
#define DA9211_STEP_MV		10

/* Current limits for DA9211 buck (uA) indices
 * corresponds with register values
 */
static const int da9211_current_limits[] = {
	2000000, 2200000, 2400000, 2600000, 2800000, 3000000, 3200000, 3400000,
	3600000, 3800000, 4000000, 4200000, 4400000, 4600000, 4800000, 5000000
};
/* Current limits for DA9213 buck (uA) indices
 * corresponds with register values
 */
static const int da9213_current_limits[] = {
	3000000, 3200000, 3400000, 3600000, 3800000, 4000000, 4200000, 4400000,
	4600000, 4800000, 5000000, 5200000, 5400000, 5600000, 5800000, 6000000
};
/* Current limits for DA9215 buck (uA) indices
 * corresponds with register values
 */
static const int da9215_current_limits[] = {
	4000000, 4200000, 4400000, 4600000, 4800000, 5000000, 5200000, 5400000,
	5600000, 5800000, 6000000, 6200000, 6400000, 6600000, 6800000, 7000000
};

/* Legacy DA9214 per-phase limits (uA), indexed by BUCK_ILIM. */
static const int da9214_legacy_current_limits[] = {
	4000000, 4200000, 4400000, 4600000, 4800000, 5000000, 5200000, 5400000,
	5600000, 5800000, 6000000, 6200000, 6400000, 6600000, 6800000, 7000000
};

static unsigned int da9211_map_buck_mode(unsigned int mode)
{
	switch (mode) {
	case DA9211_BUCK_MODE_SLEEP:
		return REGULATOR_MODE_STANDBY;
	case DA9211_BUCK_MODE_SYNC:
		return REGULATOR_MODE_FAST;
	case DA9211_BUCK_MODE_AUTO:
		return REGULATOR_MODE_NORMAL;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static unsigned int da9211_buck_get_mode(struct regulator_dev *rdev)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret, mode = 0;

	ret = regmap_read(chip->regmap, DA9211_REG_BUCKA_CONF+id, &data);
	if (ret < 0)
		return ret;

	switch (data & 0x03) {
	case DA9211_BUCK_MODE_SYNC:
		mode = REGULATOR_MODE_FAST;
		break;
	case DA9211_BUCK_MODE_AUTO:
		mode = REGULATOR_MODE_NORMAL;
		break;
	case DA9211_BUCK_MODE_SLEEP:
		mode = REGULATOR_MODE_STANDBY;
		break;
	}

	return mode;
}

static int da9211_buck_set_mode(struct regulator_dev *rdev,
					unsigned int mode)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	int val = 0;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = DA9211_BUCK_MODE_SYNC;
		break;
	case REGULATOR_MODE_NORMAL:
		val = DA9211_BUCK_MODE_AUTO;
		break;
	case REGULATOR_MODE_STANDBY:
		val = DA9211_BUCK_MODE_SLEEP;
		break;
	}

	return regmap_update_bits(chip->regmap, DA9211_REG_BUCKA_CONF+id,
					0x03, val);
}

static int da9211_set_current_limit(struct regulator_dev *rdev, int min,
				    int max)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	int i, max_size;
	const int *current_limits;

	switch (chip->chip_id) {
	case DA9211:
		current_limits = da9211_current_limits;
		max_size = ARRAY_SIZE(da9211_current_limits)-1;
		break;
	case DA9213:
		current_limits = da9213_current_limits;
		max_size = ARRAY_SIZE(da9213_current_limits)-1;
		break;
	case DA9215:
		current_limits = da9215_current_limits;
		max_size = ARRAY_SIZE(da9215_current_limits)-1;
		break;
	case DA9214:
		current_limits = da9214_legacy_current_limits;
		max_size = ARRAY_SIZE(da9214_legacy_current_limits) - 1;
		break;
	default:
		return -EINVAL;
	}

	/* search for closest to maximum */
	for (i = max_size; i >= 0; i--) {
		if (min <= current_limits[i] &&
		    max >= current_limits[i]) {
				return regmap_update_bits(chip->regmap,
					DA9211_REG_BUCK_ILIM,
					(0x0F << id*4), (i << id*4));
		}
	}

	return -EINVAL;
}

static int da9211_get_current_limit(struct regulator_dev *rdev)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret;
	const int *current_limits;

	switch (chip->chip_id) {
	case DA9211:
		current_limits = da9211_current_limits;
		break;
	case DA9213:
		current_limits = da9213_current_limits;
		break;
	case DA9215:
		current_limits = da9215_current_limits;
		break;
	case DA9214:
		current_limits = da9214_legacy_current_limits;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(chip->regmap, DA9211_REG_BUCK_ILIM, &data);
	if (ret < 0)
		return ret;

	/* select one of 16 values: 0000 (2000mA or 3000mA)
	 * to 1111 (5000mA or 6000mA).
	 */
	data = (data >> id*4) & 0x0F;
	return current_limits[data];
}

static int da9214_legacy_get_voltage_sel(struct regulator_dev *rdev)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);
	unsigned int control;
	unsigned int vsel;
	unsigned int reg;
	int id = rdev_get_id(rdev);
	int ret;

	/* CONT and both VSEL banks are application-page registers at 0x68. */
	ret = regmap_read(chip->regmap, DA9211_REG_BUCKA_CONT + id, &control);
	if (ret)
		return ret;

	reg = DA9211_REG_VBUCKA_A + id * 2;
	if (control & DA9211_VBUCKA_SEL)
		reg++;

	ret = regmap_read(chip->regmap, reg, &vsel);
	if (ret)
		return ret;

	return vsel & DA9211_VBUCK_MASK;
}

static const struct regulator_ops da9211_buck_ops = {
	.get_mode = da9211_buck_get_mode,
	.set_mode = da9211_buck_set_mode,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.set_current_limit = da9211_set_current_limit,
	.get_current_limit = da9211_get_current_limit,
};

/* Legacy voltage, current and mode programming remains firmware-owned. */
static const struct regulator_ops da9214_legacy_read_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.get_voltage_sel = da9214_legacy_get_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.get_current_limit = da9211_get_current_limit,
};

/* Only BUCKB has an established writable owner and rollback path. */
static const struct regulator_ops da9214_legacy_buckb_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_voltage_sel = da9214_legacy_get_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.get_current_limit = da9211_get_current_limit,
};

#define DA9211_BUCK(_id, _ops) \
{\
	.name = #_id,\
	.ops = _ops,\
	.type = REGULATOR_VOLTAGE,\
	.id = DA9211_ID_##_id,\
	.n_voltages = (DA9211_MAX_MV - DA9211_MIN_MV) / DA9211_STEP_MV + 1,\
	.min_uV = (DA9211_MIN_MV * 1000),\
	.uV_step = (DA9211_STEP_MV * 1000),\
	.enable_reg = DA9211_REG_BUCKA_CONT + DA9211_ID_##_id,\
	.enable_mask = DA9211_BUCKA_EN,\
	.vsel_reg = DA9211_REG_VBUCKA_A + DA9211_ID_##_id * 2,\
	.vsel_mask = DA9211_VBUCK_MASK,\
	.owner = THIS_MODULE,\
	.of_map_mode = da9211_map_buck_mode,\
}

static const struct regulator_desc da9211_regulators[] = {
	DA9211_BUCK(BUCKA, &da9211_buck_ops),
	DA9211_BUCK(BUCKB, &da9211_buck_ops),
};

static const struct regulator_desc da9214_legacy_regulators[] = {
	DA9211_BUCK(BUCKA, &da9214_legacy_read_ops),
	DA9211_BUCK(BUCKB, &da9214_legacy_buckb_ops),
};

#ifdef CONFIG_OF
static struct of_regulator_match da9211_matches[] = {
	[DA9211_ID_BUCKA] = {
		.name = "BUCKA",
		.desc = &da9211_regulators[DA9211_ID_BUCKA],
	},
	[DA9211_ID_BUCKB] = {
		.name = "BUCKB",
		.desc = &da9211_regulators[DA9211_ID_BUCKB],
	},
	};

static struct da9211_pdata *da9211_parse_regulators_dt(
		struct device *dev)
{
	struct da9211_pdata *pdata;
	struct device_node *node;
	int i, num, n;

	node = of_get_child_by_name(dev->of_node, "regulators");
	if (!node) {
		dev_err(dev, "regulators node not found\n");
		return ERR_PTR(-ENODEV);
	}

	num = of_regulator_match(dev, node, da9211_matches,
				 ARRAY_SIZE(da9211_matches));
	of_node_put(node);
	if (num < 0) {
		dev_err(dev, "Failed to match regulators\n");
		return ERR_PTR(-EINVAL);
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->num_buck = num;

	n = 0;
	for (i = 0; i < ARRAY_SIZE(da9211_matches); i++) {
		if (!da9211_matches[i].init_data)
			continue;

		pdata->init_data[n] = da9211_matches[i].init_data;
		pdata->reg_node[n] = da9211_matches[i].of_node;
		pdata->gpiod_ren[n] = devm_fwnode_gpiod_get(dev,
					of_fwnode_handle(pdata->reg_node[n]),
					"enable",
					GPIOD_OUT_HIGH |
						GPIOD_FLAGS_BIT_NONEXCLUSIVE,
					"da9211-enable");
		if (IS_ERR(pdata->gpiod_ren[n]))
			pdata->gpiod_ren[n] = NULL;
		n++;
	}

	return pdata;
}
#else
static struct da9211_pdata *da9211_parse_regulators_dt(
		struct device *dev)
{
	return ERR_PTR(-ENODEV);
}
#endif

static irqreturn_t da9211_irq_handler(int irq, void *data)
{
	struct da9211 *chip = data;
	int reg_val, err, ret = IRQ_NONE;

	err = regmap_read(chip->regmap, DA9211_REG_EVENT_B, &reg_val);
	if (err < 0)
		goto error_i2c;

	if (reg_val & DA9211_E_OV_CURR_A) {
		regulator_notifier_call_chain(chip->rdev[0],
			REGULATOR_EVENT_OVER_CURRENT, NULL);

		err = regmap_write(chip->regmap, DA9211_REG_EVENT_B,
			DA9211_E_OV_CURR_A);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	if (reg_val & DA9211_E_OV_CURR_B) {
		regulator_notifier_call_chain(chip->rdev[1],
			REGULATOR_EVENT_OVER_CURRENT, NULL);

		err = regmap_write(chip->regmap, DA9211_REG_EVENT_B,
			DA9211_E_OV_CURR_B);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	return ret;

error_i2c:
	dev_err(chip->dev, "I2C error : %d\n", err);
	return IRQ_NONE;
}

static int da9211_regulator_init(struct da9211 *chip)
{
	const struct regulator_desc *regulators = chip->legacy ?
		da9214_legacy_regulators : da9211_regulators;
	struct regulator_config config = { };
	int i, ret;
	unsigned int data;

	if (chip->legacy) {
		/* DA9214 has two independent two-phase bucks. */
		if (chip->pdata->num_buck == 2) {
			chip->num_regulator = 2;
		} else {
			dev_err(chip->dev, "Configuration is mismatched\n");
			return -EINVAL;
		}
	} else {
		ret = regmap_read(chip->regmap, DA9211_REG_CONFIG_E, &data);
		if (ret < 0) {
			dev_err(chip->dev, "Failed to read CONFIG_E reg: %d\n", ret);
			return ret;
		}

		data &= DA9211_SLAVE_SEL;
		/* If configuration for 1/2 bucks is different between platform data
		 * and the register, driver should exit.
		 */
		if (chip->pdata->num_buck == 1 && data == 0x00) {
			chip->num_regulator = 1;
		} else if (chip->pdata->num_buck == 2 && data != 0x00) {
			chip->num_regulator = 2;
		} else {
			dev_err(chip->dev, "Configuration is mismatched\n");
			return -EINVAL;
		}
	}

	for (i = 0; i < chip->num_regulator; i++) {
		config.init_data = chip->pdata->init_data[i];
		config.dev = chip->dev;
		config.driver_data = chip;
		config.regmap = chip->regmap;
		config.of_node = chip->pdata->reg_node[i];

		if (chip->pdata->gpiod_ren[i])
			config.ena_gpiod = chip->pdata->gpiod_ren[i];
		else
			config.ena_gpiod = NULL;

		/*
		 * Hand the GPIO descriptor management over to the regulator
		 * core, remove it from GPIO devres management.
		 */
		if (config.ena_gpiod)
			devm_gpiod_unhinge(chip->dev, config.ena_gpiod);
		chip->rdev[i] = devm_regulator_register(chip->dev,
			&regulators[i], &config);
		if (IS_ERR(chip->rdev[i])) {
			dev_err(chip->dev,
				"Failed to register DA9211 regulator\n");
			return PTR_ERR(chip->rdev[i]);
		}

		if (chip->chip_irq != 0) {
			ret = regmap_update_bits(chip->regmap,
				DA9211_REG_MASK_B, DA9211_M_OV_CURR_A << i, 0);
			if (ret < 0) {
				dev_err(chip->dev,
					"Failed to update mask reg: %d\n", ret);
				return ret;
			}
		}
	}

	return 0;
}

/*
 * I2C driver interface functions
 */
static int da9211_i2c_probe(struct i2c_client *i2c)
{
	const struct da9211_variant *variant;
	const struct regmap_config *regmap_config;
	struct da9211 *chip;
	int error, ret;
	unsigned int data;

	chip = devm_kzalloc(&i2c->dev, sizeof(struct da9211), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &i2c->dev;
	variant = i2c_get_match_data(i2c);
	if (!variant)
		return -ENODEV;
	chip->legacy = variant->legacy;

	if (chip->legacy) {
		u32 page2_addr;

		if (i2c->addr != DA9214_LEGACY_PRIMARY_ADDR)
			return dev_err_probe(chip->dev, -ENODEV,
					     "legacy primary address must be 0x%02x\n",
					     DA9214_LEGACY_PRIMARY_ADDR);
		if (i2c->dev.of_node &&
		    (of_property_count_u32_elems(i2c->dev.of_node, "reg") != 2 ||
		     of_property_read_u32_index(i2c->dev.of_node, "reg", 1,
						&page2_addr) ||
		     page2_addr != DA9214_LEGACY_PAGE2_ADDR))
			return dev_err_probe(chip->dev, -EINVAL,
					     "invalid page2 address\n");

		/*
		 * Legacy pages 2 and 3 are exposed at 0x69. Claim that address so
		 * no second driver can own it; regulator runtime accesses stay on
		 * the primary 0x68 application pages.
		 */
		chip->page2 = devm_i2c_new_dummy_device(chip->dev,
							i2c->adapter,
							DA9214_LEGACY_PAGE2_ADDR);
		if (IS_ERR(chip->page2))
			return dev_err_probe(chip->dev, PTR_ERR(chip->page2),
					     "failed to claim page2 address\n");
		regmap_config = &da9214_legacy_regmap_config;
	} else {
		regmap_config = &da9211_regmap_config;
	}

	chip->regmap = devm_regmap_init_i2c(i2c, regmap_config);
	if (IS_ERR(chip->regmap)) {
		error = PTR_ERR(chip->regmap);
		dev_err(chip->dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	i2c_set_clientdata(i2c, chip);

	chip->pdata = i2c->dev.platform_data;

	if (chip->legacy) {
		chip->chip_id = DA9214;
	} else {
		ret = regmap_read(chip->regmap, DA9211_REG_DEVICE_ID, &data);
		if (ret < 0) {
			dev_err(chip->dev, "Failed to read DEVICE_ID reg: %d\n", ret);
			return ret;
		}

		switch (data) {
		case DA9211_DEVICE_ID:
			chip->chip_id = DA9211;
			break;
		case DA9213_DEVICE_ID:
			chip->chip_id = DA9213;
			break;
		case DA9215_DEVICE_ID:
			chip->chip_id = DA9215;
			break;
		default:
			dev_err(chip->dev, "Unsupported device id = 0x%x.\n", data);
			return -ENODEV;
		}
	}

	if (!chip->pdata)
		chip->pdata = da9211_parse_regulators_dt(chip->dev);

	if (IS_ERR(chip->pdata)) {
		dev_err(chip->dev, "No regulators defined for the platform\n");
		return PTR_ERR(chip->pdata);
	}

	chip->chip_irq = i2c->irq;

	ret = da9211_regulator_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to initialize regulator: %d\n", ret);
		return ret;
	}

	if (chip->chip_irq != 0) {
		ret = devm_request_threaded_irq(chip->dev, chip->chip_irq, NULL,
					da9211_irq_handler,
					IRQF_TRIGGER_LOW|IRQF_ONESHOT,
					"da9211", chip);
		if (ret != 0) {
			dev_err(chip->dev, "Failed to request IRQ: %d\n",
				chip->chip_irq);
			return ret;
		}
	} else {
		dev_warn(chip->dev, "No IRQ configured\n");
	}

	return ret;
}

static const struct i2c_device_id da9211_i2c_id[] = {
	{ "da9211", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9212", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9213", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9223", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9214", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9224", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9215", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9225", (kernel_ulong_t)&da9211_paged_variant },
	{ "da9214-legacy", (kernel_ulong_t)&da9214_legacy_variant },
	{},
};
MODULE_DEVICE_TABLE(i2c, da9211_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id da9211_dt_ids[] = {
	{ .compatible = "dlg,da9211", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9212", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9213", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9223", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9214", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9224", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9215", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9225", .data = &da9211_paged_variant },
	{ .compatible = "dlg,da9214-legacy", .data = &da9214_legacy_variant },
	{},
};
MODULE_DEVICE_TABLE(of, da9211_dt_ids);
#endif

static struct i2c_driver da9211_regulator_driver = {
	.driver = {
		.name = "da9211",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(da9211_dt_ids),
	},
	.probe = da9211_i2c_probe,
	.id_table = da9211_i2c_id,
};

module_i2c_driver(da9211_regulator_driver);

MODULE_AUTHOR("James Ban <James.Ban.opensource@diasemi.com>");
MODULE_DESCRIPTION("DA9211/DA9212/DA9213/DA9223/DA9214/DA9224/DA9215/DA9225 regulator driver");
MODULE_LICENSE("GPL");
