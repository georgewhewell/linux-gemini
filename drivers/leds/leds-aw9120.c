// SPDX-License-Identifier: GPL-2.0-only
/*
 * AWINIC AW9120 20-channel LED driver
 *
 * The chip has 20 constant-current sinks. Each one has a 3-bit maximum
 * current code and an 8-bit PWM level. The PWM level is set with the same
 * instructions that its internal effect processor runs from SRAM. This
 * driver only uses the direct I2C path: every configured output is put
 * under I2C control and is set with a SETPWMI command written to CMDR.
 *
 * Data sheet: AW9120, 20 Programmable LED Driver, V1.4 (Feb. 2019).
 *
 * Copyright (C) 2026 George Whewell <georgerw@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define AW9120_REG_IDRST		0x00
#define AW9120_CHIP_ID			0xb223
#define AW9120_SOFT_RESET		0x55aa

#define AW9120_REG_GCR			0x01
#define AW9120_GCR_LEDE			BIT(0)

/* LER1/CTRS1 hold outputs 0..11, LER2/CTRS2 outputs 12..19 */
#define AW9120_REG_LER1			0x50
#define AW9120_REG_LCR			0x52
#define AW9120_LCR_LOGLIN		GENMASK(1, 0)
#define AW9120_LCR_LOGLIN_LINEAR	2
#define AW9120_REG_PMD			0x53
#define AW9120_REG_RMD			0x54
#define AW9120_REG_CTRS1		0x55
#define AW9120_REG_IMAX1		0x57
#define AW9120_REG_IMAX5		0x5b
#define AW9120_REG_LISR			0x5e
#define AW9120_REG_SADDR		0x5f
#define AW9120_REG_PCR			0x60
#define AW9120_REG_CMDR			0x61
#define AW9120_REG_GRP			0x6e
#define AW9120_REG_WPR			0x7d
#define AW9120_REG_WADDR		0x7e
#define AW9120_REG_WDATA		0x7f

#define AW9120_OUTPUTS_PER_REG		12
#define AW9120_IMAX_PER_REG		4
#define AW9120_IMAX_MASK		GENMASK(2, 0)

/* SETPWMI: 101b, 5-bit channel, 8-bit PWM level */
#define AW9120_CMD_SETPWMI		0xa000
#define AW9120_CMD_CH			GENMASK(12, 8)
#define AW9120_CMD_PWM			GENMASK(7, 0)
/* From CMDR, channel 0x1f addresses every output under I2C control */
#define AW9120_CMD_CH_ALL		0x1f

#define AW9120_NUM_OUTPUTS		20
#define AW9120_MAX_BRIGHTNESS		255
#define AW9120_IMAX_STEP_UA		3500
#define AW9120_IMAX_MAX_CODE		7
#define AW9120_DEFAULT_MAX_UA		AW9120_IMAX_STEP_UA

/* Minimum wait between PDN going high and the first I2C access */
#define AW9120_POWER_ON_DELAY_US	5000

struct aw9120;

struct aw9120_led {
	struct aw9120 *chip;
	struct led_classdev_mc mc_cdev;
	/* The only channel of a monochrome LED */
	struct mc_subled channel;
	bool multicolor;
};

struct aw9120 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *enable_gpio;
	/* Serialises register access and protects the fields below */
	struct mutex lock;
	u8 pwm[AW9120_NUM_OUTPUTS];
	bool active;
};

static unsigned int aw9120_output_reg(unsigned int base, unsigned int out)
{
	return base + out / AW9120_OUTPUTS_PER_REG;
}

static unsigned int aw9120_output_bit(unsigned int out)
{
	return BIT(out % AW9120_OUTPUTS_PER_REG);
}

static int aw9120_setpwm(struct aw9120 *chip, unsigned int ch, u8 pwm)
{
	return regmap_write(chip->regmap, AW9120_REG_CMDR,
			    AW9120_CMD_SETPWMI |
			    FIELD_PREP(AW9120_CMD_CH, ch) |
			    FIELD_PREP(AW9120_CMD_PWM, pwm));
}

static int aw9120_power_on(struct aw9120 *chip)
{
	int ret;

	if (chip->enable_gpio) {
		/* PDN going high resets every register */
		gpiod_set_value_cansleep(chip->enable_gpio, 1);
		fsleep(AW9120_POWER_ON_DELAY_US);

		regcache_cache_only(chip->regmap, false);
		ret = regcache_sync(chip->regmap);
		if (ret)
			goto err_power_off;
	}

	ret = regmap_set_bits(chip->regmap, AW9120_REG_GCR, AW9120_GCR_LEDE);
	if (ret)
		goto err_power_off;

	ret = aw9120_setpwm(chip, AW9120_CMD_CH_ALL, 0);
	if (ret)
		goto err_power_off;

	chip->active = true;

	return 0;

err_power_off:
	regmap_clear_bits(chip->regmap, AW9120_REG_GCR, AW9120_GCR_LEDE);
	if (chip->enable_gpio) {
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
		regcache_cache_only(chip->regmap, true);
		regcache_mark_dirty(chip->regmap);
	}

	return ret;
}

static int aw9120_power_off(struct aw9120 *chip)
{
	int ret;

	/* With LEDE clear the LED driver is held in reset and all outputs off */
	ret = regmap_clear_bits(chip->regmap, AW9120_REG_GCR, AW9120_GCR_LEDE);

	if (chip->enable_gpio) {
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
		regcache_cache_only(chip->regmap, true);
		regcache_mark_dirty(chip->regmap);
		ret = 0;
	}

	chip->active = false;

	return ret;
}

static bool aw9120_any_lit(struct aw9120 *chip)
{
	unsigned int i;

	for (i = 0; i < AW9120_NUM_OUTPUTS; i++)
		if (chip->pwm[i])
			return true;

	return false;
}

static int aw9120_brightness_set(struct led_classdev *cdev,
				 enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct aw9120_led *led = container_of(mc_cdev, struct aw9120_led,
					      mc_cdev);
	struct aw9120 *chip = led->chip;
	unsigned int i;
	int ret = 0;

	if (led->multicolor)
		led_mc_calc_color_components(mc_cdev, brightness);
	else
		led->channel.brightness = brightness;

	guard(mutex)(&chip->lock);

	for (i = 0; i < mc_cdev->num_colors; i++)
		chip->pwm[mc_cdev->subled_info[i].channel] =
			mc_cdev->subled_info[i].brightness;

	if (!aw9120_any_lit(chip))
		return chip->active ? aw9120_power_off(chip) : 0;

	if (!chip->active) {
		ret = aw9120_power_on(chip);
		if (ret)
			return ret;
	}

	for (i = 0; i < mc_cdev->num_colors; i++) {
		struct mc_subled *subled = &mc_cdev->subled_info[i];

		ret = aw9120_setpwm(chip, subled->channel, subled->brightness);
		if (ret)
			return ret;
	}

	return 0;
}

static int aw9120_setup_channel(struct aw9120 *chip, struct fwnode_handle *fwnode,
				struct mc_subled *subled, unsigned long *used)
{
	unsigned int imax_reg, imax_shift, code, bit;
	u32 out, max_ua = AW9120_DEFAULT_MAX_UA;
	int ret;

	ret = fwnode_property_read_u32(fwnode, "reg", &out);
	if (ret)
		return dev_err_probe(chip->dev, ret, "%pfw: missing reg\n", fwnode);

	if (out >= AW9120_NUM_OUTPUTS)
		return dev_err_probe(chip->dev, -EINVAL, "%pfw: invalid reg %u\n",
				     fwnode, out);

	if (test_and_set_bit(out, used))
		return dev_err_probe(chip->dev, -EINVAL,
				     "%pfw: output %u already in use\n",
				     fwnode, out);

	fwnode_property_read_u32(fwnode, "led-max-microamp", &max_ua);
	code = max_ua / AW9120_IMAX_STEP_UA;
	if (!code || code > AW9120_IMAX_MAX_CODE)
		return dev_err_probe(chip->dev, -EINVAL,
				     "%pfw: invalid led-max-microamp %u\n",
				     fwnode, max_ua);

	imax_reg = AW9120_REG_IMAX1 + out / AW9120_IMAX_PER_REG;
	imax_shift = 4 * (out % AW9120_IMAX_PER_REG);
	ret = regmap_update_bits(chip->regmap, imax_reg,
				 AW9120_IMAX_MASK << imax_shift,
				 code << imax_shift);
	if (ret)
		return ret;

	/* Enable the output and put it under I2C (CMDR) control */
	bit = aw9120_output_bit(out);
	ret = regmap_set_bits(chip->regmap,
			      aw9120_output_reg(AW9120_REG_LER1, out), bit);
	if (ret)
		return ret;

	ret = regmap_set_bits(chip->regmap,
			      aw9120_output_reg(AW9120_REG_CTRS1, out), bit);
	if (ret)
		return ret;

	subled->channel = out;

	return 0;
}

static int aw9120_setup_led(struct aw9120 *chip, struct fwnode_handle *fwnode,
			    struct aw9120_led *led, unsigned long *used)
{
	struct led_classdev_mc *mc_cdev = &led->mc_cdev;
	struct mc_subled *subleds;
	u32 color = LED_COLOR_ID_WHITE;
	unsigned int i = 0;
	int ret;

	fwnode_property_read_u32(fwnode, "color", &color);
	led->multicolor = color == LED_COLOR_ID_RGB ||
			  color == LED_COLOR_ID_MULTI;
	led->chip = chip;

	if (!led->multicolor) {
		mc_cdev->subled_info = &led->channel;
		mc_cdev->num_colors = 1;

		return aw9120_setup_channel(chip, fwnode, &led->channel, used);
	}

	mc_cdev->num_colors = fwnode_get_child_node_count(fwnode);
	if (!mc_cdev->num_colors)
		return dev_err_probe(chip->dev, -EINVAL,
				     "%pfw: multicolor LED without channels\n",
				     fwnode);

	subleds = devm_kcalloc(chip->dev, mc_cdev->num_colors, sizeof(*subleds),
			       GFP_KERNEL);
	if (!subleds)
		return -ENOMEM;

	fwnode_for_each_child_node_scoped(fwnode, child) {
		ret = fwnode_property_read_u32(child, "color",
					       &subleds[i].color_index);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "%pfw: missing color\n", child);

		ret = aw9120_setup_channel(chip, child, &subleds[i], used);
		if (ret)
			return ret;

		subleds[i].intensity = AW9120_MAX_BRIGHTNESS;
		i++;
	}

	mc_cdev->subled_info = subleds;

	return 0;
}

static int aw9120_register_led(struct aw9120 *chip, struct fwnode_handle *fwnode,
			       struct aw9120_led *led)
{
	struct led_classdev *cdev = &led->mc_cdev.led_cdev;
	struct led_init_data init_data = { .fwnode = fwnode };
	int ret;

	cdev->max_brightness = AW9120_MAX_BRIGHTNESS;
	cdev->brightness_set_blocking = aw9120_brightness_set;

	if (led->multicolor)
		ret = devm_led_classdev_multicolor_register_ext(chip->dev,
								&led->mc_cdev,
								&init_data);
	else
		ret = devm_led_classdev_register_ext(chip->dev, cdev, &init_data);

	if (ret)
		return dev_err_probe(chip->dev, ret, "%pfw: failed to register\n",
				     fwnode);

	return 0;
}

static void aw9120_disable(void *data)
{
	struct aw9120 *chip = data;

	guard(mutex)(&chip->lock);
	aw9120_power_off(chip);
}

static int aw9120_chip_init(struct aw9120 *chip)
{
	unsigned int id;
	int ret;

	if (chip->enable_gpio) {
		/*
		 * The GPIO was requested low, so PDN is in shutdown. Raising
		 * it generates the power-on reset. The data sheet requires
		 * about 100 us between VBAT power-up and PDN going high, and
		 * does not give a minimum low time; keep it low for as long as
		 * the power-on wait.
		 */
		fsleep(AW9120_POWER_ON_DELAY_US);
		gpiod_set_value_cansleep(chip->enable_gpio, 1);
		fsleep(AW9120_POWER_ON_DELAY_US);
	}

	ret = regmap_read(chip->regmap, AW9120_REG_IDRST, &id);
	if (ret)
		return dev_err_probe(chip->dev, ret, "failed to read chip ID\n");

	if (id != AW9120_CHIP_ID)
		return dev_err_probe(chip->dev, -ENODEV,
				     "unexpected chip ID %#06x\n", id);

	if (!chip->enable_gpio) {
		/* No PDN control, so reset the registers by software */
		ret = regmap_write(chip->regmap, AW9120_REG_IDRST,
				   AW9120_SOFT_RESET);
		if (ret)
			return ret;

		fsleep(AW9120_POWER_ON_DELAY_US);
	}

	/* Map the 8-bit PWM level linearly to the duty cycle */
	return regmap_update_bits(chip->regmap, AW9120_REG_LCR,
				  AW9120_LCR_LOGLIN,
				  FIELD_PREP(AW9120_LCR_LOGLIN,
					     AW9120_LCR_LOGLIN_LINEAR));
}

static bool aw9120_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AW9120_REG_IDRST:
	case AW9120_REG_GCR:
	case AW9120_REG_LER1 ... AW9120_REG_IMAX5:
	case AW9120_REG_LISR ... AW9120_REG_GRP:
	case AW9120_REG_WPR ... AW9120_REG_WDATA:
		return true;
	default:
		return false;
	}
}

static bool aw9120_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AW9120_REG_IDRST:
	case AW9120_REG_GCR:
	case AW9120_REG_LER1 ... AW9120_REG_IMAX5:
	case AW9120_REG_SADDR ... AW9120_REG_CMDR:
	case AW9120_REG_WPR ... AW9120_REG_WDATA:
		return true;
	default:
		return false;
	}
}

static bool aw9120_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AW9120_REG_IDRST:
	case AW9120_REG_PMD:
	case AW9120_REG_RMD:
	case AW9120_REG_LISR:
	case AW9120_REG_PCR ... AW9120_REG_GRP:
	case AW9120_REG_WADDR:
	case AW9120_REG_WDATA:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config aw9120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW9120_REG_WDATA,
	.readable_reg = aw9120_readable_reg,
	.writeable_reg = aw9120_writeable_reg,
	.volatile_reg = aw9120_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
	/* The data sheet only documents single-register writes */
	.use_single_write = true,
};

static const char * const aw9120_supplies[] = { "vbat", "vio" };

static int aw9120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	DECLARE_BITMAP(used, AW9120_NUM_OUTPUTS) = { };
	struct aw9120_led *leds;
	struct aw9120 *chip;
	unsigned int count, i;
	int ret;

	count = device_get_child_node_count(dev);
	if (!count)
		return dev_err_probe(dev, -ENODEV, "no LEDs defined\n");

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	leds = devm_kcalloc(dev, count, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	chip->dev = dev;
	i2c_set_clientdata(client, chip);

	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;

	chip->regmap = devm_regmap_init_i2c(client, &aw9120_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "failed to initialise regmap\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(aw9120_supplies),
					     aw9120_supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supplies\n");

	chip->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->enable_gpio),
				     "failed to get enable GPIO\n");

	ret = devm_add_action_or_reset(dev, aw9120_disable, chip);
	if (ret)
		return ret;

	ret = aw9120_chip_init(chip);
	if (ret)
		return ret;

	i = 0;
	device_for_each_child_node_scoped(dev, child) {
		ret = aw9120_setup_led(chip, child, &leds[i++], used);
		if (ret)
			return ret;
	}

	/* Nothing is lit yet: stay in shutdown until an LED is turned on */
	ret = aw9120_power_off(chip);
	if (ret)
		return ret;

	i = 0;
	device_for_each_child_node_scoped(dev, child) {
		ret = aw9120_register_led(chip, child, &leds[i++]);
		if (ret)
			return ret;
	}

	return 0;
}

static void aw9120_shutdown(struct i2c_client *client)
{
	aw9120_disable(i2c_get_clientdata(client));
}

static const struct i2c_device_id aw9120_id[] = {
	{ "aw9120" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw9120_id);

static const struct of_device_id aw9120_of_match[] = {
	{ .compatible = "awinic,aw9120" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9120_of_match);

static struct i2c_driver aw9120_driver = {
	.driver = {
		.name = "leds-aw9120",
		.of_match_table = aw9120_of_match,
	},
	.probe = aw9120_probe,
	.shutdown = aw9120_shutdown,
	.id_table = aw9120_id,
};
module_i2c_driver(aw9120_driver);

MODULE_AUTHOR("George Whewell <georgerw@gmail.com>");
MODULE_DESCRIPTION("AWINIC AW9120 LED driver");
MODULE_LICENSE("GPL");
