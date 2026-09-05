/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6351_CALIBRATION_H
#define __MT6351_CALIBRATION_H

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/types.h>

/* Pure arithmetic only: no regmap, acquisition, registration or fallback. */
struct mt6351_calibration {
	u16 vts;
	u8 degc;
	u8 slope;
	u8 slope_negative;
};

/* Output arguments are unchanged on every error. */
static inline int mt6351_calibration_decode(const u16 words[2],
					  struct mt6351_calibration *out)
{
	struct mt6351_calibration cal;

	if (!(words[0] & 1))
		return -ENODATA;
	cal.degc = (words[0] >> 1) & 0x3f;
	if (cal.degc < 38 || cal.degc > 60)
		return -ERANGE;
	cal.vts = ((words[1] & 0xf) << 9) | (words[0] >> 7);
	cal.slope_negative = (words[1] >> 4) & 1;
	cal.slope = (words[1] >> 5) & 0x3f;
	/* Vendor ID=0 means slope=0, not an uncalibrated chip. */
	if (!(words[1] & (1 << 11)))
		cal.slope = 0;
	*out = cal;
	return 0;
}

static inline int mt6351_calibration_muldiv(s64 a, s64 b, s64 divisor,
					  s64 *out)
{
	s64 product;

	if (!divisor)
		return -EINVAL;
	if (check_mul_overflow(a, b, &product) ||
	    (product == S64_MIN && divisor == -1))
		return -EOVERFLOW;
	*out = div64_s64(product, divisor);
	return 0;
}

/*
 * Vendor 8cfe6596a503612e3332d9c26e292a19525a7f07:
 * drivers/misc/mediatek/thermal/mt6797/src/mtk_pmic_efuse.c,
 * mtktspmic_cali_prepare2() and pmic_raw_to_temp(); ADC millivolt truncation
 * in power/mt6797/pmic_auxadc.c, PMIC_IMM_GetOneChannelValue().
 * Keep the distinct (1598 +/- slope*10) intercept and (1598 +/- slope)
 * sample denominators, and each integer division boundary. No vendor
 * default fuse values, temperature substitutions or previous-value cache.
 */
static inline int mt6351_calibration_to_mc(const struct mt6351_calibration *cal,
					 unsigned int raw, s32 *out)
{
	s64 slope, vts_mv, sample_mv, intercept, term, temperature;
	int ret;

	if (raw > 0xfff || cal->vts > 0x1fff || cal->degc < 38 ||
	    cal->degc > 60 || cal->slope > 63 || cal->slope_negative > 1)
		return -ERANGE;
	slope = cal->slope_negative ? -(s64)cal->slope : cal->slope;
	ret = mt6351_calibration_muldiv(cal->vts, 1800, 4096, &vts_mv);
	if (ret)
		return ret;
	ret = mt6351_calibration_muldiv(raw, 1800, 4096, &sample_mv);
	if (ret)
		return ret;
	ret = mt6351_calibration_muldiv(vts_mv, 1000000,
					1598 + slope * 10, &intercept);
	if (ret)
		return ret;
	ret = mt6351_calibration_muldiv(sample_mv, -1000000,
					1598 + slope, &term);
	if (ret)
		return ret;
	if (check_add_overflow(intercept, (s64)cal->degc * 500, &temperature) ||
	    check_add_overflow(temperature, term, &temperature))
		return -EOVERFLOW;
	if (temperature < -50000 || temperature > 150000)
		return -ERANGE;
	*out = temperature;
	return 0;
}

#endif
