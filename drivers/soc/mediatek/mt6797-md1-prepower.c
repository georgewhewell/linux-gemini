// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reversible MT6797 MD1 pre-power primitives.
 *
 * These are deliberately not a modem boot driver.  They implement only the
 * vendor PMIC rail values and the two MD1 PWR_ON requests, then restore the
 * captured state.  In particular they never clear PWR_CLK_DIS/PWR_ISO, release
 * PWR_RST_B, alter source-clock enable, touch CLDMA/CCIF, or release a boot
 * vector.  A poll bound constrains repeated reads, not an MMIO transaction
 * which has already stalled the interconnect.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>

#define MT6797_MD1_SPM_MD1_PWR_CON	0x320
#define MT6797_MD1_SPM_PWR_STATUS	0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND	0x184

#define MT6797_MD1_SPM_PWR_RST_B	BIT(0)
#define MT6797_MD1_SPM_PWR_ISO		BIT(1)
#define MT6797_MD1_SPM_PWR_ON		BIT(2)
#define MT6797_MD1_SPM_PWR_ON_2ND	BIT(3)
#define MT6797_MD1_SPM_PWR_CLK_DIS	BIT(4)
#define MT6797_MD1_SPM_MD_SRAM_PDN	BIT(8)
#define MT6797_MD1_SPM_PWR_STATUS_MD1	BIT(0)

#define MT6797_MD1_SPM_MAX_POLLS	100
#define MT6797_MD1_SPM_POLL_DELAY_US	10

enum mt6797_md1_pmic_field_id {
	MT6797_MD1_VMODEM_HW_CONTROL,
	MT6797_MD1_VMODEM_ENABLE,
	MT6797_MD1_VMODEM_SELECTOR,
	MT6797_MD1_VMODEM_SLEEP_ENABLE,
	MT6797_MD1_VMD1_HW_CONTROL,
	MT6797_MD1_VMD1_ENABLE,
	MT6797_MD1_VMD1_SELECTOR,
	MT6797_MD1_VMD1_SLEEP_ENABLE,
	MT6797_MD1_VSRAM_MD_HW_CONTROL,
	MT6797_MD1_VSRAM_MD_ENABLE,
	MT6797_MD1_VSRAM_MD_SELECTOR,
	MT6797_MD1_VSRAM_MD_SLEEP_ENABLE,
};

struct mt6797_md1_pmic_field {
	u16 reg;
	u16 mask;
	u16 vendor_value;
	bool write_vendor_value;
};

/* Register addresses and fields are from MT6351 vendor upmu_hw.h and the
 * exact values/order are from eccci/mt6797/cldma_platform.c:555-575.
 */
static const struct mt6797_md1_pmic_field mt6797_md1_pmic_fields[] = {
	[MT6797_MD1_VMODEM_HW_CONTROL] = {
		MT6351_BUCK_VMODEM_CON0, BIT(1), BIT(1), true,
	},
	[MT6797_MD1_VMODEM_ENABLE] = {
		MT6351_BUCK_VMODEM_CON2, BIT(0), BIT(0), true,
	},
	[MT6797_MD1_VMODEM_SELECTOR] = {
		MT6351_BUCK_VMODEM_CON5, GENMASK(6, 0), 0x40, true,
	},
	[MT6797_MD1_VMODEM_SLEEP_ENABLE] = {
		MT6351_BUCK_VMODEM_CON9, BIT(8), BIT(8), true,
	},
	[MT6797_MD1_VMD1_HW_CONTROL] = {
		MT6351_BUCK_VMD1_CON0, BIT(1), BIT(1), true,
	},
	[MT6797_MD1_VMD1_ENABLE] = {
		MT6351_BUCK_VMD1_CON2, BIT(0), BIT(0), true,
	},
	[MT6797_MD1_VMD1_SELECTOR] = {
		MT6351_BUCK_VMD1_CON5, GENMASK(6, 0), 0, false,
	},
	[MT6797_MD1_VMD1_SLEEP_ENABLE] = {
		MT6351_BUCK_VMD1_CON9, BIT(8), BIT(8), true,
	},
	[MT6797_MD1_VSRAM_MD_HW_CONTROL] = {
		MT6351_BUCK_VSRAM_MD_CON0, BIT(1), BIT(1), true,
	},
	[MT6797_MD1_VSRAM_MD_ENABLE] = {
		MT6351_BUCK_VSRAM_MD_CON2, BIT(0), BIT(0), true,
	},
	[MT6797_MD1_VSRAM_MD_SELECTOR] = {
		MT6351_BUCK_VSRAM_MD_CON5, GENMASK(6, 0), 0x50, true,
	},
	[MT6797_MD1_VSRAM_MD_SLEEP_ENABLE] = {
		MT6351_BUCK_VSRAM_MD_CON9, BIT(8), BIT(8), true,
	},
};

static const unsigned int mt6797_md1_pmic_vendor_order[] = {
	MT6797_MD1_VSRAM_MD_ENABLE,
	MT6797_MD1_VSRAM_MD_SLEEP_ENABLE,
	MT6797_MD1_VMD1_ENABLE,
	MT6797_MD1_VMD1_SLEEP_ENABLE,
	MT6797_MD1_VMODEM_ENABLE,
	MT6797_MD1_VMODEM_SLEEP_ENABLE,
	MT6797_MD1_VSRAM_MD_SELECTOR,
	MT6797_MD1_VMODEM_SELECTOR,
	MT6797_MD1_VSRAM_MD_HW_CONTROL,
	MT6797_MD1_VMD1_HW_CONTROL,
	MT6797_MD1_VMODEM_HW_CONTROL,
};

int mt6797_md1_pmic_snapshot(struct regmap *pmic,
			     struct mt6797_md1_pmic_snapshot *snapshot)
{
	unsigned int value;
	int i, ret;

	if (!pmic || !snapshot)
		return -EINVAL;

	snapshot->valid = false;
	for (i = 0; i < ARRAY_SIZE(mt6797_md1_pmic_fields); i++) {
		ret = regmap_read(pmic, mt6797_md1_pmic_fields[i].reg, &value);
		if (ret)
			return ret;
		snapshot->value[i] = value & mt6797_md1_pmic_fields[i].mask;
	}
	snapshot->valid = true;
	return 0;
}

static int mt6797_md1_pmic_update(struct regmap *pmic, unsigned int field,
				   u16 value)
{
	const struct mt6797_md1_pmic_field *entry = &mt6797_md1_pmic_fields[field];

	return regmap_update_bits(pmic, entry->reg, entry->mask, value);
}

int mt6797_md1_pmic_apply_vendor_values(struct regmap *pmic,
					 struct mt6797_md1_pmic_snapshot *snapshot)
{
	int i, ret;

	if (!pmic || !snapshot || !snapshot->valid)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_pmic_vendor_order); i++) {
		unsigned int field = mt6797_md1_pmic_vendor_order[i];

		ret = mt6797_md1_pmic_update(pmic, field,
					     mt6797_md1_pmic_fields[field].vendor_value);
		if (ret)
			return ret;
	}
	return 0;
}

int mt6797_md1_pmic_restore(struct regmap *pmic,
			    struct mt6797_md1_pmic_snapshot *snapshot)
{
	int i, ret, first_error = 0;

	if (!pmic || !snapshot || !snapshot->valid)
		return -EINVAL;

	for (i = ARRAY_SIZE(mt6797_md1_pmic_vendor_order) - 1; i >= 0; i--) {
		unsigned int field = mt6797_md1_pmic_vendor_order[i];

		ret = mt6797_md1_pmic_update(pmic, field, snapshot->value[field]);
		if (ret && !first_error)
			first_error = ret;
	}
	snapshot->valid = false;
	return first_error;
}

int mt6797_md1_spm_snapshot(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	if (!spm || !snapshot)
		return -EINVAL;

	snapshot->pwr_con = readl(spm + MT6797_MD1_SPM_MD1_PWR_CON);
	snapshot->valid = true;
	return 0;
}

static int mt6797_md1_spm_wait(void __iomem *spm, bool on)
{
	unsigned int attempt;
	u32 status, status_2nd;

	for (attempt = 0; attempt < MT6797_MD1_SPM_MAX_POLLS; attempt++) {
		status = readl(spm + MT6797_MD1_SPM_PWR_STATUS) &
			 MT6797_MD1_SPM_PWR_STATUS_MD1;
		status_2nd = readl(spm + MT6797_MD1_SPM_PWR_STATUS_2ND) &
			     MT6797_MD1_SPM_PWR_STATUS_MD1;
		if (!!status == on && !!status_2nd == on)
			return 0;
		udelay(MT6797_MD1_SPM_POLL_DELAY_US);
	}
	return -ETIMEDOUT;
}

int mt6797_md1_spm_power_on(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	u32 value;
	int ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	/* Do not take ownership of an already-booted or half-powered MD1. */
	ret = mt6797_md1_spm_wait(spm, false);
	if (ret)
		return ret;

	value = snapshot->pwr_con | MT6797_MD1_SPM_PWR_ON;
	writel(value, spm + MT6797_MD1_SPM_MD1_PWR_CON);
	value |= MT6797_MD1_SPM_PWR_ON_2ND;
	writel(value, spm + MT6797_MD1_SPM_MD1_PWR_CON);

	return mt6797_md1_spm_wait(spm, true);
}

int mt6797_md1_spm_restore(void __iomem *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	int ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	/* Restore clock/isolation/reset/SRAM bits exactly before reporting exit. */
	writel(snapshot->pwr_con, spm + MT6797_MD1_SPM_MD1_PWR_CON);
	ret = mt6797_md1_spm_wait(spm, false);
	snapshot->valid = false;
	return ret;
}
