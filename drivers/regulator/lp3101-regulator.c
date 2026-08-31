// SPDX-License-Identifier: GPL-2.0-only
/*
 * LP3101 display-bias regulator for the Planet Computers Gemini PDA.
 *
 * The electrical contract in this driver is intentionally limited to the
 * board sequence recovered from the Planet vendor kernel.  In particular,
 * drivers/misc/mediatek/gpio/lp3101.c only writes 0x0f to registers 0x00
 * and 0x01; it does not provide an LP3101 data sheet or output voltages.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define LP3101_REG_OUTP		0x00
#define LP3101_REG_OUTN		0x01
#define LP3101_GEMINI_VALUE		0x0f
#define LP3101_BIAS_SETTLE_MS		20

enum lp3101_output {
	LP3101_OUTP,
	LP3101_OUTN,
	LP3101_NUM_OUTPUTS,
};

struct lp3101 {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_dev *rdevs[LP3101_NUM_OUTPUTS];
	/* Protect enable state and the GPIO/I2C sequencing. */
	struct mutex lock;
	struct gpio_desc *enable_gpios[LP3101_NUM_OUTPUTS];
	unsigned int enabled;
};

static int lp3101_enable(struct regulator_dev *rdev)
{
	struct lp3101 *lp3101 = rdev_get_drvdata(rdev);
	unsigned int id = rdev_get_id(rdev);
	int ret;

	mutex_lock(&lp3101->lock);

	if (id == LP3101_OUTN && !(lp3101->enabled & BIT(LP3101_OUTP))) {
		ret = -EINVAL;
		dev_err(lp3101->dev, "outn enabled before outp\n");
		goto unlock;
	}

	gpiod_set_value_cansleep(lp3101->enable_gpios[id], 1);
	lp3101->enabled |= BIT(id);

	if (id != LP3101_OUTN) {
		ret = 0;
		goto unlock;
	}

	/*
	 * Exact downstream ordering from
	 * drivers/misc/mediatek/lcm/aeon_nt36672_fhd_dsi_vdo_x600_xinli/
	 * aeon_nt36672_fhd_dsi_vdo_x600_xinli.c: ENP high, ENN high, wait 20
	 * ms, then lp3101_poweron() writes 0x0f to registers 0x00 and 0x01.
	 * The panel consumes outp before outn, and regulator_bulk_enable()
	 * preserves that order.
	 */
	msleep(LP3101_BIAS_SETTLE_MS);

	ret = regmap_write(lp3101->regmap, LP3101_REG_OUTP, LP3101_GEMINI_VALUE);
	if (ret)
		goto disable_outn;

	ret = regmap_write(lp3101->regmap, LP3101_REG_OUTN, LP3101_GEMINI_VALUE);
	if (ret)
		goto disable_outn;

	goto unlock;

disable_outn:
	gpiod_set_value_cansleep(lp3101->enable_gpios[LP3101_OUTN], 0);
	lp3101->enabled &= ~BIT(LP3101_OUTN);
unlock:
	mutex_unlock(&lp3101->lock);
	return ret;
}

static int lp3101_disable(struct regulator_dev *rdev)
{
	struct lp3101 *lp3101 = rdev_get_drvdata(rdev);
	unsigned int id = rdev_get_id(rdev);

	mutex_lock(&lp3101->lock);

	/* regulator_bulk_disable() reverses the panel's outp/outn order. */
	gpiod_set_value_cansleep(lp3101->enable_gpios[id], 0);
	lp3101->enabled &= ~BIT(id);

	mutex_unlock(&lp3101->lock);
	return 0;
}

static int lp3101_is_enabled(struct regulator_dev *rdev)
{
	struct lp3101 *lp3101 = rdev_get_drvdata(rdev);

	return gpiod_get_value_cansleep(lp3101->enable_gpios[rdev_get_id(rdev)]);
}

static const struct regulator_ops lp3101_ops = {
	.enable = lp3101_enable,
	.disable = lp3101_disable,
	.is_enabled = lp3101_is_enabled,
};

static int lp3101_of_parse_cb(struct device_node *np,
			      const struct regulator_desc *desc,
			      struct regulator_config *config)
{
	struct lp3101 *lp3101 = config->driver_data;
	struct gpio_desc *gpiod;

	gpiod = devm_fwnode_gpiod_get(lp3101->dev,
				      of_fwnode_handle(np), "enable",
				      GPIOD_OUT_LOW, "enable");
	if (IS_ERR(gpiod))
		return dev_err_probe(lp3101->dev, PTR_ERR(gpiod),
				     "failed to get %s enable GPIO\n", desc->of_match);

	lp3101->enable_gpios[desc->id] = gpiod;
	return 0;
}

#define LP3101_REGULATOR_DESC(_id, _name) { \
	.name = "lp3101-" #_name, \
	.of_match = #_name, \
	.id = LP3101_##_id, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.ops = &lp3101_ops, \
	.of_parse_cb = lp3101_of_parse_cb, \
}

static const struct regulator_desc lp3101_descs[LP3101_NUM_OUTPUTS] = {
	LP3101_REGULATOR_DESC(OUTP, outp),
	LP3101_REGULATOR_DESC(OUTN, outn),
};

static const struct regmap_config lp3101_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LP3101_REG_OUTN,
};

static int lp3101_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lp3101 *lp3101;
	struct regulator_config config = { };
	int id;

	lp3101 = devm_kzalloc(dev, sizeof(*lp3101), GFP_KERNEL);
	if (!lp3101)
		return -ENOMEM;

	lp3101->regmap = devm_regmap_init_i2c(client, &lp3101_regmap_config);
	if (IS_ERR(lp3101->regmap))
		return dev_err_probe(dev, PTR_ERR(lp3101->regmap),
				     "failed to initialize regmap\n");

	lp3101->dev = dev;
	mutex_init(&lp3101->lock);
	i2c_set_clientdata(client, lp3101);

	for (id = 0; id < LP3101_NUM_OUTPUTS; id++) {
		config.dev = dev;
		config.of_node = dev_of_node(dev);
		config.driver_data = lp3101;

		lp3101->rdevs[id] = devm_regulator_register(dev, &lp3101_descs[id], &config);
		if (IS_ERR(lp3101->rdevs[id]))
			return dev_err_probe(dev, PTR_ERR(lp3101->rdevs[id]),
					     "failed to register regulator\n");
	}

	return 0;
}

static const struct of_device_id lp3101_of_match[] = {
	{ .compatible = "mediatek,i2c_lcd_bias" },
	{ }
};
MODULE_DEVICE_TABLE(of, lp3101_of_match);

static const struct i2c_device_id lp3101_i2c_ids[] = {
	{ "lp3101", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lp3101_i2c_ids);

static struct i2c_driver lp3101_driver = {
	.driver = {
		.name = "lp3101-regulator",
		.of_match_table = lp3101_of_match,
	},
	.probe = lp3101_probe,
	.id_table = lp3101_i2c_ids,
};
module_i2c_driver(lp3101_driver);

MODULE_AUTHOR("Gemini PDA mainline contributors");
MODULE_DESCRIPTION("LP3101 Gemini display-bias regulator");
MODULE_LICENSE("GPL");
