// SPDX-License-Identifier: GPL-2.0
/*
 * Power off through MediaTek PMIC
 *
 * Copyright (C) 2018 MediaTek Inc.
 *
 * Author: Sean Wang <sean.wang@mediatek.com>
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/property.h>
#include <linux/reboot.h>

/* MT6351 RTC_BBPU/RTC_AL_SEC fields, as the vendor kernel names them. */
#define RTC_BBPU_PWREN		BIT(0)	/* power on when the RTC alarm fires */
#define RTC_BBPU_AUTO		BIT(3)
#define RTC_AL_SEC_AUTO_PDN_SEL	BIT(6)
#define RTC_AL_SEC_2SEC_EN	BIT(8)
/* MT6351 CHR_CON0 RGS_CHRDET: a charger is attached. */
#define MT6351_CHR_CON0		0x0f78
#define MT6351_RGS_CHRDET	BIT(5)

struct mt6323_pwrc_data {
	int priority;
	/*
	 * Run from the sleepable power-off prepare phase: PMIC wrapper regmap
	 * access takes a mutex and polls with usleep_range().
	 */
	bool prepare;
	/* BBPU value written to power off, besides RTC_BBPU_KEY. */
	u16 bbpu;
	/* Disable the RTC's 2-second reboot before powering off. */
	bool no_2sec_reboot;
	/*
	 * With a charger attached the PMIC keeps the board powered: restart
	 * with "charger" so the bootloader shows off-mode charging.
	 */
	bool restart_on_charger;
};

struct mt6323_pwrc {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	const struct mt6323_pwrc_data *data;
};

/* Commit a written RTC register and wait until the RTC has taken it. */
static int mt6323_rtc_commit(struct mt6323_pwrc *pwrc)
{
	unsigned int val;

	regmap_write(pwrc->regmap, pwrc->base + RTC_WRTGR_MT6323, 1);
	return regmap_read_poll_timeout(pwrc->regmap, pwrc->base + RTC_BBPU,
					val, !(val & RTC_BBPU_CBUSY),
					MTK_RTC_POLL_DELAY_US,
					MTK_RTC_POLL_TIMEOUT);
}

static int mt6323_do_pwroff(struct sys_off_data *data)
{
	struct mt6323_pwrc *pwrc = data->cb_data;
	const struct mt6323_pwrc_data *pdata = pwrc->data;
	int ret;

	if (pdata->no_2sec_reboot) {
		regmap_clear_bits(pwrc->regmap, pwrc->base + RTC_AL_SEC,
				  RTC_AL_SEC_2SEC_EN | RTC_AL_SEC_AUTO_PDN_SEL);
		ret = mt6323_rtc_commit(pwrc);
		if (ret)
			dev_err(pwrc->dev, "failed to disable 2 s reboot: %d\n", ret);
	}

	regmap_write(pwrc->regmap, pwrc->base + RTC_BBPU,
		     RTC_BBPU_KEY | pdata->bbpu);
	ret = mt6323_rtc_commit(pwrc);
	if (ret)
		dev_err(pwrc->dev, "failed to write BBPU: %d\n", ret);

	if (pdata->restart_on_charger) {
		unsigned int chr;
		int i;

		/* As the vendor mt_power_off(): up to 1 s, then restart anyway. */
		for (i = 0; i < 10; i++) {
			mdelay(100);
			if (!regmap_read(pwrc->regmap, MT6351_CHR_CON0, &chr) &&
			    (chr & MT6351_RGS_CHRDET))
				break;
		}
		machine_restart("charger");
	}

	/* Wait some time until system down, otherwise, notice with a warn */
	mdelay(1000);

	WARN_ONCE(1, "Unable to power off system\n");

	return NOTIFY_DONE;
}

static int mt6323_pwrc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6323_pwrc *pwrc;
	struct resource *res;
	int ret;

	pwrc = devm_kzalloc(&pdev->dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	pwrc->base = res->start;
	pwrc->regmap = mt6397_chip->regmap;
	pwrc->dev = &pdev->dev;
	pwrc->data = device_get_match_data(&pdev->dev);

	ret = devm_register_sys_off_handler(pwrc->dev,
					    pwrc->data->prepare ?
					    SYS_OFF_MODE_POWER_OFF_PREPARE :
					    SYS_OFF_MODE_POWER_OFF,
					    pwrc->data->priority,
					    mt6323_do_pwroff,
					    pwrc);
	if (ret)
		return dev_err_probe(pwrc->dev, ret, "failed to register power-off handler\n");

	return 0;
}

static const struct mt6323_pwrc_data mt6323_pwrc_data = {
	.priority = SYS_OFF_PRIO_DEFAULT,
};

/*
 * MT6797 boards pair the MT6351 with a BL31 whose PSCI SYSTEM_OFF only parks
 * the calling CPU (the TF-A platform port never implemented it), which leaves
 * the PMIC powered and the board unresponsive to its power key.  Dropping BBPU
 * through the PMIC RTC is how the vendor kernel powers off, so take precedence
 * over the firmware handler, and do it as the vendor does
 * (mt6351/mtk_rtc_hal.c hal_rtc_bbpu_pwdn(), rtc_bbpu_pwrdown(true)): disable
 * the 2 s reboot, then write KEY | AUTO | PWREN, which keeps RTC alarm power-on
 * working. If the board is still powered after that (a charger is attached),
 * restart with "charger" as the vendor mt_power_off() does, rather than
 * falling through to a PSCI SYSTEM_OFF that only parks the CPUs. All of this
 * runs in the power-off prepare phase, after device shutdown: the PMIC
 * wrapper cannot be driven from the atomic power-off chain.
 */
static const struct mt6323_pwrc_data mt6351_pwrc_data = {
	.priority = SYS_OFF_PRIO_FIRMWARE + 1,
	.prepare = true,
	.bbpu = RTC_BBPU_AUTO | RTC_BBPU_PWREN,
	.no_2sec_reboot = true,
	.restart_on_charger = true,
};

static const struct of_device_id mt6323_pwrc_dt_match[] = {
	{ .compatible = "mediatek,mt6323-pwrc", .data = &mt6323_pwrc_data },
	{ .compatible = "mediatek,mt6351-pwrc", .data = &mt6351_pwrc_data },
	{},
};
MODULE_DEVICE_TABLE(of, mt6323_pwrc_dt_match);

static struct platform_driver mt6323_pwrc_driver = {
	.probe          = mt6323_pwrc_probe,
	.driver         = {
		.name   = "mt6323-pwrc",
		.of_match_table = mt6323_pwrc_dt_match,
	},
};

module_platform_driver(mt6323_pwrc_driver);

MODULE_DESCRIPTION("Poweroff driver for MT6323 PMIC");
MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
