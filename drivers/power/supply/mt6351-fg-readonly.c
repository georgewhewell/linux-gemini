// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MT6351 fuel-gauge qualification driver.
 *
 * The MT6351 FGADC requires a write/latch/clear sequence for coherent current
 * and coulomb-counter data. Board calibration and gauge ownership are not yet
 * established, so this driver intentionally does neither. It exposes only the
 * raw FGADC control word and the vendor's direct instantaneous-current read.
 */

#include <linux/device.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct mt6351_fg_readonly {
	struct regmap *regmap;
};

static ssize_t fgadc_control_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mt6351_fg_readonly *fg = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%04x\\n", val);
}
static DEVICE_ATTR_RO(fgadc_control);

static ssize_t raw_current_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mt6351_fg_readonly *fg = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON25, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%04x\\n", val);
}
static DEVICE_ATTR_RO(raw_current);

static struct attribute *mt6351_fg_readonly_attrs[] = {
	&dev_attr_fgadc_control.attr,
	&dev_attr_raw_current.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6351_fg_readonly);

static int mt6351_fg_readonly_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6351 = dev_get_drvdata(pdev->dev.parent);
	struct mt6351_fg_readonly *fg;
	unsigned int val;
	int ret;

	if (!mt6351 || !mt6351->regmap)
		return -ENODEV;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->regmap = mt6351->regmap;
	platform_set_drvdata(pdev, fg);

	/* Confirm the fuel-gauge register block is reachable without changing it. */
	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &val);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read FGADC control\n");

	dev_dbg(&pdev->dev, "FGADC control is 0x%04x\n", val);

	return 0;
}

static struct platform_driver mt6351_fg_readonly_driver = {
	.probe = mt6351_fg_readonly_probe,
	.driver = {
		.name = "mt6351-fg-readonly",
		.dev_groups = mt6351_fg_readonly_groups,
	},
};
module_platform_driver(mt6351_fg_readonly_driver);

MODULE_DESCRIPTION("Read-only MT6351 fuel-gauge qualification driver");
MODULE_LICENSE("GPL");
