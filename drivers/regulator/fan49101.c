// SPDX-License-Identifier: GPL-2.0-only
//
// onsemi FAN49101 I2C buck-boost regulator

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define FAN49101_VOUT		0x01
#define FAN49101_ID1		0x40
#define FAN49101_ID2		0x41
#define FAN49101_MANUFACTURER_ID	0x83
#define FAN49101_VOUT_MASK	GENMASK(5, 0)
#define FAN49101_ENABLE		BIT(7)

#define FAN49101_MIN_UV		603000
#define FAN49101_STEP_UV		12826
#define FAN49101_NVOLTAGES	64

static const struct regmap_config fan49101_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FAN49101_ID2,
};

static const struct regulator_ops fan49101_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.map_voltage = regulator_map_voltage_linear,
	.list_voltage = regulator_list_voltage_linear,
};

static const struct regulator_desc fan49101_regulator_desc = {
	.name = "fan49101-vout",
	.of_match = "vout",
	.regulators_node = "regulators",
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.ops = &fan49101_regulator_ops,
	.n_voltages = FAN49101_NVOLTAGES,
	.min_uV = FAN49101_MIN_UV,
	.uV_step = FAN49101_STEP_UV,
	.vsel_reg = FAN49101_VOUT,
	.vsel_mask = FAN49101_VOUT_MASK,
	.enable_reg = FAN49101_VOUT,
	.enable_mask = FAN49101_ENABLE,
};

static const struct of_device_id fan49101_of_match[] = {
	{ .compatible = "onsemi,fan49101" },
	{ }
};
MODULE_DEVICE_TABLE(of, fan49101_of_match);

static const struct i2c_device_id fan49101_i2c_id[] = {
	{ "fan49101" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fan49101_i2c_id);

static int fan49101_probe(struct i2c_client *client)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int value;
	int ret;

	regmap = devm_regmap_init_i2c(client, &fan49101_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(regmap, FAN49101_ID1, &value);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read manufacturer ID\n");
	if (value != FAN49101_MANUFACTURER_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected manufacturer ID 0x%02x\n", value);

	ret = regmap_read(regmap, FAN49101_ID2, &value);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read die ID\n");
	dev_info(&client->dev, "FAN49101 detected, die ID 0x%02x\n", value);

	config.dev = &client->dev;
	config.of_node = client->dev.of_node;
	config.regmap = regmap;

	rdev = devm_regulator_register(&client->dev,
				       &fan49101_regulator_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	return 0;
}

static struct i2c_driver fan49101_driver = {
	.driver = {
		.name = "fan49101-regulator",
		.of_match_table = fan49101_of_match,
	},
	.probe = fan49101_probe,
	.id_table = fan49101_i2c_id,
};
module_i2c_driver(fan49101_driver);

MODULE_DESCRIPTION("onsemi FAN49101 buck-boost regulator");
MODULE_LICENSE("GPL");
