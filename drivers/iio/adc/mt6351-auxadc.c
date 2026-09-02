// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6351 PMIC AUXADC driver
 *
 * Copyright (c) 2026 George Rowlands
 *
 * MT6351 is not described by the later MT6359 driver's register model: its
 * AP result registers are interleaved with MD results, requests use a SET
 * alias without a stop write, and its initialization and timing differ.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/iio/adc/mediatek,mt6351-auxadc.h>

#define MT6351_AUXADC_DATA_MASK		GENMASK(14, 0)
#define MT6351_AUXADC_READY		BIT(15)

#define MT6351_AUXADC_SMPS_CK_PDN	BIT(9)
#define MT6351_AUXADC_CK_AON		BIT(15)
#define MT6351_AUXADC_AVG_NUM_SMALL	GENMASK(2, 0)
#define MT6351_AUXADC_AVG_NUM_LARGE	GENMASK(5, 3)
#define MT6351_AUXADC_AVG_NUM_SEL	GENMASK(11, 0)
#define MT6351_AUXADC_VBUF_EN		BIT(9)

#define MT6351_AUXADC_AVG_SMALL_VAL	2
#define MT6351_AUXADC_AVG_LARGE_VAL	6
#define MT6351_AUXADC_AVG_SEL_VAL	0x83

/* The vendor driver waits 50 us, then polls every 1.3--1.5 ms 17 times. */
#define MT6351_AUXADC_PREPARE_US		50
#define MT6351_AUXADC_POLL_MIN_US	1300
#define MT6351_AUXADC_POLL_MAX_US	1500
#define MT6351_AUXADC_POLL_ATTEMPTS	17

/* Both AP battery channels use a 1.8 V ADC with a proven 3:1 divider. */
#define MT6351_AUXADC_FULL_SCALE_MV	(3 * 1800)
#define MT6351_AUXADC_REALBITS		15

struct mt6351_auxadc_channel {
	u16 data_reg;
	u8 request_bit;
};

struct mt6351_auxadc {
	struct device *dev;
	struct regmap *regmap;
	/* Serialize the shared request and result registers. */
	struct mutex lock;
	unsigned int saved_ckpdn;
	unsigned int saved_con0;
	unsigned int saved_con1;
	unsigned int saved_con2;
	unsigned int saved_con10;
};

static const struct mt6351_auxadc_channel mt6351_auxadc_channels[] = {
	[MT6351_AUXADC_BATSNS] = {
		.data_reg = MT6351_AUXADC_ADC23,
		.request_bit = 0,
	},
	[MT6351_AUXADC_ISENSE] = {
		.data_reg = MT6351_AUXADC_ADC25,
		.request_bit = 1,
	},
};

#define MT6351_AUXADC_CHAN(_name, _channel) { \
	.type = IIO_VOLTAGE, \
	.channel = (_channel), \
	.datasheet_name = (_name), \
	.indexed = 1, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
			      BIT(IIO_CHAN_INFO_SCALE), \
	.scan_type = { \
		.sign = 'u', \
		.realbits = MT6351_AUXADC_REALBITS, \
		.storagebits = 16, \
		.endianness = IIO_CPU, \
	}, \
}

static const struct iio_chan_spec mt6351_auxadc_iio_channels[] = {
	MT6351_AUXADC_CHAN("batsns", MT6351_AUXADC_BATSNS),
	MT6351_AUXADC_CHAN("isense", MT6351_AUXADC_ISENSE),
};

static int mt6351_auxadc_enable(struct mt6351_auxadc *adc)
{
	int ret;

	ret = regmap_clear_bits(adc->regmap, MT6351_TOP_CKPDN_CON0,
				MT6351_AUXADC_SMPS_CK_PDN);
	if (ret)
		return ret;

	return regmap_set_bits(adc->regmap, MT6351_AUXADC_CON0,
			       MT6351_AUXADC_CK_AON);
}

static int
mt6351_auxadc_read_channel(struct mt6351_auxadc *adc, unsigned int channel,
			   int *value)
{
	const struct mt6351_auxadc_channel *desc;
	unsigned int val;
	int i, ret;

	desc = &mt6351_auxadc_channels[channel];

	mutex_lock(&adc->lock);

	ret = mt6351_auxadc_enable(adc);
	if (ret)
		goto out_unlock;

	/* RQST0_SET starts a one-shot conversion; vendor and LK do not clear it. */
	ret = regmap_write(adc->regmap, MT6351_AUXADC_RQST0_SET,
			   BIT(desc->request_bit));
	if (ret)
		goto out_unlock;

	/* The vendor sequence reads status before its settling delay. */
	ret = regmap_read(adc->regmap, MT6351_AUXADC_STA0, &val);
	if (ret)
		goto out_unlock;

	udelay(MT6351_AUXADC_PREPARE_US);

	for (i = 0; i < MT6351_AUXADC_POLL_ATTEMPTS; i++) {
		ret = regmap_read(adc->regmap, desc->data_reg, &val);
		if (ret)
			goto out_unlock;
		if (val & MT6351_AUXADC_READY)
			break;

		usleep_range(MT6351_AUXADC_POLL_MIN_US,
			     MT6351_AUXADC_POLL_MAX_US);
	}
	if (i == MT6351_AUXADC_POLL_ATTEMPTS) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	*value = val & MT6351_AUXADC_DATA_MASK;
	ret = IIO_VAL_INT;

out_unlock:
	mutex_unlock(&adc->lock);
	return ret;
}

static int mt6351_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6351_auxadc *adc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return mt6351_auxadc_read_channel(adc, chan->channel, val);
	case IIO_CHAN_INFO_SCALE:
		*val = MT6351_AUXADC_FULL_SCALE_MV;
		*val2 = MT6351_AUXADC_REALBITS;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mt6351_auxadc_info = {
	.read_raw = mt6351_auxadc_read_raw,
};

static int mt6351_auxadc_restore(struct mt6351_auxadc *adc)
{
	int ret = 0, tmp;

	tmp = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON10,
				 MT6351_AUXADC_VBUF_EN, adc->saved_con10);
	if (tmp)
		ret = tmp;

	tmp = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON2,
				 MT6351_AUXADC_AVG_NUM_SEL, adc->saved_con2);
	if (tmp && !ret)
		ret = tmp;

	tmp = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON1,
				 MT6351_AUXADC_AVG_NUM_SMALL |
				 MT6351_AUXADC_AVG_NUM_LARGE, adc->saved_con1);
	if (tmp && !ret)
		ret = tmp;

	tmp = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON0,
				 MT6351_AUXADC_CK_AON, adc->saved_con0);
	if (tmp && !ret)
		ret = tmp;

	tmp = regmap_update_bits(adc->regmap, MT6351_TOP_CKPDN_CON0,
				 MT6351_AUXADC_SMPS_CK_PDN, adc->saved_ckpdn);
	if (tmp && !ret)
		ret = tmp;

	return ret;
}

static void mt6351_auxadc_restore_action(void *data)
{
	struct mt6351_auxadc *adc = data;
	int ret;

	mutex_lock(&adc->lock);
	ret = mt6351_auxadc_restore(adc);
	mutex_unlock(&adc->lock);
	if (ret)
		dev_warn(adc->dev, "failed to restore AUXADC configuration: %d\n",
			 ret);
}

static int mt6351_auxadc_hw_init(struct mt6351_auxadc *adc)
{
	unsigned int avg;
	int ret;

	ret = regmap_read(adc->regmap, MT6351_TOP_CKPDN_CON0,
			  &adc->saved_ckpdn);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap, MT6351_AUXADC_CON0, &adc->saved_con0);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap, MT6351_AUXADC_CON1, &adc->saved_con1);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap, MT6351_AUXADC_CON2, &adc->saved_con2);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap, MT6351_AUXADC_CON10,
			  &adc->saved_con10);
	if (ret)
		return ret;

	ret = mt6351_auxadc_enable(adc);
	if (ret)
		goto restore;

	avg = FIELD_PREP(MT6351_AUXADC_AVG_NUM_SMALL,
			 MT6351_AUXADC_AVG_SMALL_VAL) |
	      FIELD_PREP(MT6351_AUXADC_AVG_NUM_LARGE,
			 MT6351_AUXADC_AVG_LARGE_VAL);
	ret = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON1,
				 MT6351_AUXADC_AVG_NUM_SMALL |
				 MT6351_AUXADC_AVG_NUM_LARGE, avg);
	if (ret)
		goto restore;

	ret = regmap_update_bits(adc->regmap, MT6351_AUXADC_CON2,
				 MT6351_AUXADC_AVG_NUM_SEL,
				 MT6351_AUXADC_AVG_SEL_VAL);
	if (ret)
		goto restore;

	ret = regmap_set_bits(adc->regmap, MT6351_AUXADC_CON10,
			      MT6351_AUXADC_VBUF_EN);
	if (ret)
		goto restore;

	return 0;

restore:
	mt6351_auxadc_restore(adc);
	return ret;
}

static int mt6351_auxadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *chip = dev_get_drvdata(dev->parent);
	struct mt6351_auxadc *adc;
	struct iio_dev *indio_dev;
	int ret;

	if (!chip || !chip->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get PMIC regmap\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->dev = dev;
	adc->regmap = chip->regmap;
	mutex_init(&adc->lock);

	ret = mt6351_auxadc_hw_init(adc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize AUXADC\n");

	ret = devm_add_action_or_reset(dev, mt6351_auxadc_restore_action, adc);
	if (ret)
		return ret;

	indio_dev->name = "mt6351-auxadc";
	indio_dev->info = &mt6351_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mt6351_auxadc_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(mt6351_auxadc_iio_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id mt6351_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6351-auxadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_auxadc_of_match);

static struct platform_driver mt6351_auxadc_driver = {
	.probe = mt6351_auxadc_probe,
	.driver = {
		.name = "mt6351-auxadc",
		.of_match_table = mt6351_auxadc_of_match,
	},
};
module_platform_driver(mt6351_auxadc_driver);

MODULE_AUTHOR("George Rowlands");
MODULE_DESCRIPTION("MediaTek MT6351 PMIC AUXADC driver");
MODULE_LICENSE("GPL");
