// SPDX-License-Identifier: GPL-2.0-only
/*
 * Regulator driver for direct-address DA9213-DA9215 devices.
 *
 * This interface exposes the primary and identification banks at separate
 * I2C addresses. Unlike the PAGE_CON interface supported by da9211-regulator,
 * selecting the identification bank therefore does not modify device state.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/driver.h>

#include "da9213-legacy-transaction.h"

#define DA9213_LEGACY_PRIMARY_ADDR	0x68
#define DA9213_LEGACY_PAGE2_ADDR		0x69
#define DA9213_LEGACY_PASSES		2

#define DA9213_LEGACY_BUCKA		0
#define DA9213_LEGACY_BUCKB		1

enum da9213_legacy_bank {
	DA9213_LEGACY_PRIMARY,
	DA9213_LEGACY_PAGE2,
};

struct da9213_legacy_variant {
	const char *name;
	unsigned int num_bucks;
};

struct da9213_legacy_sample {
	enum da9213_legacy_bank bank;
	u8 reg;
	u8 expected;
};

struct da9213_legacy {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_client *page2;
	const struct da9213_legacy_variant *variant;
	/* Serialize read-modify-write transactions across both buck outputs. */
	struct mutex lock;
};

static const struct da9213_legacy_variant da9213_legacy_da9213 = {
	.name = "DA9213",
	.num_bucks = 1,
};

static const struct da9213_legacy_variant da9213_legacy_da9214 = {
	.name = "DA9214",
	.num_bucks = 2,
};

static const struct da9213_legacy_variant da9213_legacy_da9215 = {
	.name = "DA9215",
	.num_bucks = 2,
};

/* Immutable identification-bank values observed on the Gemini device. */
static const struct da9213_legacy_sample da9213_legacy_samples[] = {
	{ DA9213_LEGACY_PAGE2,   0x05, 0xd9 },
	{ DA9213_LEGACY_PAGE2,   0x06, 0xd0 },
	{ DA9213_LEGACY_PAGE2,   0x47, 0xc0 },
};

static const u8 da9213_legacy_vsel_reg[DA9213_LEGACY_BUCK_COUNT] = {
	[DA9213_LEGACY_BUCKA] = 0xd7,
	[DA9213_LEGACY_BUCKB] = 0xd9,
};

static const u8 da9213_legacy_cont_reg[DA9213_LEGACY_BUCK_COUNT] = {
	[DA9213_LEGACY_BUCKA] = 0x5d,
	[DA9213_LEGACY_BUCKB] = 0x5e,
};

static int da9213_legacy_raw_read(struct i2c_client *client, u8 reg, u8 *value)
{
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.len = 1,
			.buf = &reg,
		}, {
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static int da9213_legacy_raw_write(struct i2c_client *client, u8 reg, u8 value)
{
	u8 data[] = { reg, value };
	int ret;

	ret = i2c_master_send(client, data, ARRAY_SIZE(data));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(data))
		return -EIO;

	return 0;
}

static int da9213_legacy_io_read(void *context, u8 reg, u8 *value)
{
	struct da9213_legacy *chip = context;

	return da9213_legacy_raw_read(chip->client, reg, value);
}

static int da9213_legacy_io_write(void *context, u8 reg, u8 value)
{
	struct da9213_legacy *chip = context;

	return da9213_legacy_raw_write(chip->client, reg, value);
}

static const struct da9213_legacy_transaction_io da9213_legacy_io = {
	.read = da9213_legacy_io_read,
	.write = da9213_legacy_io_write,
};

int da9213_legacy_update_bits_transaction(const struct da9213_legacy_transaction_io *io,
					  void *context, u8 reg, u8 mask,
					  u8 value)
{
	u8 before;
	u8 expected;
	u8 observed;
	int primary_ret;
	int ret;

	if (!io || !io->read || !io->write || !mask || value & ~mask)
		return -EINVAL;

	ret = io->read(context, reg, &before);
	if (ret)
		return ret;

	expected = (before & ~mask) | value;
	if (expected == before)
		return 0;

	ret = io->write(context, reg, expected);
	if (ret) {
		primary_ret = ret;
		goto rollback;
	}

	ret = io->read(context, reg, &observed);
	if (!ret && observed == expected)
		return 0;

	primary_ret = ret ?: -EIO;

rollback:
	ret = io->write(context, reg, before);
	if (ret)
		return -EUCLEAN;

	ret = io->read(context, reg, &observed);
	if (ret || observed != before)
		return -EUCLEAN;

	return primary_ret;
}

static int da9213_legacy_read_primary(struct da9213_legacy *chip, u8 reg,
				      u8 *value)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = da9213_legacy_raw_read(chip->client, reg, value);
	mutex_unlock(&chip->lock);

	return ret;
}

static int da9213_legacy_update_primary(struct da9213_legacy *chip, u8 reg,
					u8 mask, u8 value)
{
	int ret;

	mutex_lock(&chip->lock);
	ret = da9213_legacy_update_bits_transaction(&da9213_legacy_io, chip,
						    reg, mask, value);
	mutex_unlock(&chip->lock);

	return ret;
}

static int da9213_legacy_get_voltage_sel(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);
	u8 reg = da9213_legacy_vsel_reg[rdev_get_id(rdev)];
	u8 value;
	int ret;

	ret = da9213_legacy_read_primary(chip, reg, &value);
	if (ret)
		return ret;

	return value & DA9213_LEGACY_VSEL_MASK;
}

static int da9213_legacy_set_voltage_sel(struct regulator_dev *rdev,
					 unsigned int selector)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);

	if (selector > DA9213_LEGACY_VSEL_MASK)
		return -EINVAL;

	return da9213_legacy_update_primary(chip,
			da9213_legacy_vsel_reg[rdev_get_id(rdev)],
			DA9213_LEGACY_VSEL_MASK, selector);
}

static int da9213_legacy_is_enabled(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);
	u8 reg = da9213_legacy_cont_reg[rdev_get_id(rdev)];
	u8 value;
	int ret;

	ret = da9213_legacy_read_primary(chip, reg, &value);
	if (ret)
		return ret;

	return !!(value & DA9213_LEGACY_ENABLE_MASK);
}

static int da9213_legacy_enable(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);

	return da9213_legacy_update_primary(chip,
			da9213_legacy_cont_reg[rdev_get_id(rdev)],
			DA9213_LEGACY_ENABLE_MASK, DA9213_LEGACY_ENABLE_MASK);
}

static int da9213_legacy_disable(struct regulator_dev *rdev)
{
	struct da9213_legacy *chip = rdev_get_drvdata(rdev);

	return da9213_legacy_update_primary(chip,
			da9213_legacy_cont_reg[rdev_get_id(rdev)],
			DA9213_LEGACY_ENABLE_MASK, 0);
}

static const struct regulator_ops da9213_legacy_ops = {
	.get_voltage_sel = da9213_legacy_get_voltage_sel,
	.set_voltage_sel = da9213_legacy_set_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.is_enabled = da9213_legacy_is_enabled,
	.enable = da9213_legacy_enable,
	.disable = da9213_legacy_disable,
};

#define DA9213_LEGACY_BUCK(_id, _name)					\
{									\
	.name = "DA921x-" _name,					\
	.ops = &da9213_legacy_ops,					\
	.type = REGULATOR_VOLTAGE,					\
	.id = _id,							\
	.n_voltages = DA9213_LEGACY_VSEL_MASK + 1,			\
	.min_uV = DA9213_LEGACY_MIN_UV,				\
	.uV_step = DA9213_LEGACY_STEP_UV,				\
	.owner = THIS_MODULE,						\
}

static const struct regulator_desc da9213_legacy_regulators[] = {
	DA9213_LEGACY_BUCK(DA9213_LEGACY_BUCKA, "BUCKA"),
	DA9213_LEGACY_BUCK(DA9213_LEGACY_BUCKB, "BUCKB"),
};

static int da9213_legacy_register_regulators(struct da9213_legacy *chip)
{
	struct regulator_config config = {
		.dev = chip->dev,
		.driver_data = chip,
	};
	struct device_node *regulators;
	unsigned int registered = 0;
	unsigned int i;

	regulators = of_get_child_by_name(chip->dev->of_node, "regulators");
	if (!regulators)
		return -EINVAL;

	for (i = 0; i < chip->variant->num_bucks; i++) {
		struct regulator_dev *rdev;
		const char *name = i == DA9213_LEGACY_BUCKA ? "BUCKA" : "BUCKB";

		config.of_node = of_get_child_by_name(regulators, name);
		if (!config.of_node) {
			of_node_put(regulators);
			return -EINVAL;
		}
		if (!of_device_is_available(config.of_node)) {
			of_node_put(config.of_node);
			continue;
		}

		rdev = devm_regulator_register(chip->dev,
					       &da9213_legacy_regulators[i],
					       &config);
		of_node_put(config.of_node);
		if (IS_ERR(rdev)) {
			of_node_put(regulators);
			return PTR_ERR(rdev);
		}
		registered++;
	}

	of_node_put(regulators);
	return registered ? 0 : -ENODEV;
}

static int da9213_legacy_validate_node(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	const char *name;
	u32 addr;
	int i;

	if (!np || client->flags & I2C_CLIENT_TEN ||
	    client->addr != DA9213_LEGACY_PRIMARY_ADDR)
		return -EINVAL;

	if (of_property_count_u32_elems(np, "reg") != 2 ||
	    of_property_count_strings(np, "reg-names") != 2)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		u32 expected_addr = i ? DA9213_LEGACY_PAGE2_ADDR :
					 DA9213_LEGACY_PRIMARY_ADDR;
		const char *expected_name = i ? "page2" : "primary";

		if (of_property_read_u32_index(np, "reg", i, &addr) ||
		    addr != expected_addr ||
		    of_property_read_string_index(np, "reg-names", i, &name) ||
		    strcmp(name, expected_name))
			return -EINVAL;
	}

	return 0;
}

static int da9213_legacy_identify(struct da9213_legacy *chip)
{
	unsigned int pass;
	unsigned int sample;

	for (pass = 0; pass < DA9213_LEGACY_PASSES; pass++) {
		for (sample = 0; sample < ARRAY_SIZE(da9213_legacy_samples);
		     sample++) {
			const struct da9213_legacy_sample *expected =
				&da9213_legacy_samples[sample];
			struct i2c_client *target;
			u8 value;
			int ret;

			target = expected->bank == DA9213_LEGACY_PAGE2 ?
				 chip->page2 : chip->client;
			ret = da9213_legacy_raw_read(target, expected->reg, &value);
			if (ret)
				return ret;
			if (value != expected->expected)
				return -ENODEV;
		}
	}

	return 0;
}

static int da9213_legacy_probe(struct i2c_client *client)
{
	struct da9213_legacy *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->variant = i2c_get_match_data(client);
	if (!chip->variant)
		return -ENODEV;

	chip->dev = &client->dev;
	chip->client = client;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	ret = da9213_legacy_validate_node(client);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "invalid direct-address description\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&client->dev, -EOPNOTSUPP,
				     "adapter lacks raw I2C transfers\n");

	chip->page2 = devm_i2c_new_dummy_device(&client->dev, client->adapter,
						DA9213_LEGACY_PAGE2_ADDR);
	if (IS_ERR(chip->page2))
		return dev_err_probe(&client->dev, PTR_ERR(chip->page2),
				     "failed to claim identification address\n");

	ret = da9213_legacy_identify(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "read-only identity transcript failed\n");

	ret = da9213_legacy_register_regulators(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to register regulators\n");

	dev_info(&client->dev, "%s direct-address regulators registered\n",
		 chip->variant->name);
	return 0;
}

static const struct of_device_id da9213_legacy_of_match[] = {
	{
		.compatible = "dlg,da9213-legacy",
		.data = &da9213_legacy_da9213,
	}, {
		.compatible = "dlg,da9214-legacy",
		.data = &da9213_legacy_da9214,
	}, {
		.compatible = "dlg,da9215-legacy",
		.data = &da9213_legacy_da9215,
	}, { }
};
MODULE_DEVICE_TABLE(of, da9213_legacy_of_match);

static struct i2c_driver da9213_legacy_driver = {
	.driver = {
		.name = "da9213-legacy-regulator",
		.of_match_table = da9213_legacy_of_match,
	},
	.probe = da9213_legacy_probe,
};
module_i2c_driver(da9213_legacy_driver);

MODULE_AUTHOR("Julien Etienne <julien.etienne@gmail.com>");
MODULE_DESCRIPTION("Dialog DA9213-DA9215 direct-address regulator driver");
MODULE_LICENSE("GPL");
