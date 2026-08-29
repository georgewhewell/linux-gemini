// SPDX-License-Identifier: GPL-2.0-only
/*
 * Richtek RT5735 regulator driver
 *
 * The RT5735 contract in this driver is limited to the VSEL0 output used by
 * the MT6797 Gemini GPU. Protection and external-buck GPIO workarounds from
 * downstream kernels are intentionally left to board firmware and userspace.
 */

#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define RT5735_REG_PRODUCT_ID		0x03
#define RT5735_REG_VSEL0		0x11
#define RT5735_REG_PGOOD		0x12

#define RT5735_PRODUCT_ID		0x10
#define RT5735_VSEL_MASK		GENMASK(6, 0)
#define RT5735_ENABLE_MASK		BIT(7)
#define RT5735_ACTIVE_DISCHARGE_MASK	BIT(4)

#define RT5735_MIN_UV			600000
#define RT5735_STEP_UV			6250
#define RT5735_N_VOLTAGES		128

static const struct regulator_ops rt5735_regulator_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_active_discharge = regulator_set_active_discharge_regmap,
};

static const struct regulator_desc rt5735_regulator_desc = {
	.name = "rt5735-vsel0",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &rt5735_regulator_ops,
	.min_uV = RT5735_MIN_UV,
	.uV_step = RT5735_STEP_UV,
	.n_voltages = RT5735_N_VOLTAGES,
	.vsel_reg = RT5735_REG_VSEL0,
	.vsel_mask = RT5735_VSEL_MASK,
	.enable_reg = RT5735_REG_VSEL0,
	.enable_mask = RT5735_ENABLE_MASK,
	.active_discharge_reg = RT5735_REG_PGOOD,
	.active_discharge_mask = RT5735_ACTIVE_DISCHARGE_MASK,
	.active_discharge_on = RT5735_ACTIVE_DISCHARGE_MASK,
};

static const struct regmap_config rt5735_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x16,
};

static int rt5735_probe(struct i2c_client *client)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int product_id;
	int ret;

	regmap = devm_regmap_init_i2c(client, &rt5735_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(regmap, RT5735_REG_PRODUCT_ID, &product_id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read product ID\n");

	if (product_id != RT5735_PRODUCT_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected product ID 0x%02x\n", product_id);

	config.dev = &client->dev;
	config.of_node = dev_of_node(&client->dev);
	config.regmap = regmap;
	config.init_data = of_get_regulator_init_data(&client->dev,
							config.of_node,
							&rt5735_regulator_desc);

	rdev = devm_regulator_register(&client->dev, &rt5735_regulator_desc,
					       &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&client->dev, PTR_ERR(rdev),
				     "failed to register regulator\n");

	return 0;
}

static const struct of_device_id rt5735_of_match[] = {
	{ .compatible = "richtek,rt5735" },
	{ }
};
MODULE_DEVICE_TABLE(of, rt5735_of_match);

static const struct i2c_device_id rt5735_i2c_ids[] = {
	{ "rt5735", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt5735_i2c_ids);

static struct i2c_driver rt5735_driver = {
	.driver = {
		.name = "rt5735-regulator",
		.of_match_table = rt5735_of_match,
	},
	.probe = rt5735_probe,
	.id_table = rt5735_i2c_ids,
};
module_i2c_driver(rt5735_driver);

MODULE_AUTHOR("Gemini PDA mainline contributors");
MODULE_DESCRIPTION("Richtek RT5735 VSEL0 regulator driver");
MODULE_LICENSE("GPL");
