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
#include <linux/mfd/mt6351/registers.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>

#define MT6797_MD1_SPM_POWERON_CONFIG_EN	0x000
#define MT6797_MD1_SPM_PWR_STATUS	0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND	0x184
#define MT6797_MD1_SPM_MD1_PWR_CON	0x320
#define MT6797_MD1_SPM_MD_EXT_BUCK_ISO	0x390
#define MT6797_MD1_SPM_MDBSI_CON	0x404
#define MT6797_MD1_SPM_MDBSI_MASK	GENMASK(2, 0)
/* Vendor clk_buf_swctrl_modem_on[] yields CON=0, NFC=1, AUD=1. */
#define MT6797_MD1_SPM_MDBSI_MODEM_ON	GENMASK(2, 1)

#define MT6797_MD1_SPM_PWR_RST_B	BIT(0)
#define MT6797_MD1_SPM_PWR_ISO		BIT(1)
#define MT6797_MD1_SPM_PWR_ON		BIT(2)
#define MT6797_MD1_SPM_PWR_ON_2ND	BIT(3)
#define MT6797_MD1_SPM_PWR_CLK_DIS	BIT(4)
#define MT6797_MD1_SPM_MD_SRAM_PDN	BIT(8)
#define MT6797_MD1_SPM_PWR_STATUS_MD1	BIT(0)
#define MT6797_MD1_SPM_POWERON_CONFIG_EN_BIT	BIT(0)
#define MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1	BIT(0)
#define MT6797_MD1_SPM_MD1_PWR_CON_OWNED	(MT6797_MD1_SPM_PWR_ON | \
						 MT6797_MD1_SPM_PWR_ON_2ND)
#define MT6797_MD1_SPM_MD1_PWR_CON_INTERNAL	(MT6797_MD1_SPM_PWR_CLK_DIS | \
							 MT6797_MD1_SPM_PWR_ISO | \
							 MT6797_MD1_SPM_MD_SRAM_PDN)

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
	/* cldma_platform.c delays 300us after the two selector writes. */
	/* The three hardware-control writes below must remain after that delay. */
	MT6797_MD1_VSRAM_MD_HW_CONTROL,
	MT6797_MD1_VMD1_HW_CONTROL,
	MT6797_MD1_VMODEM_HW_CONTROL,
};

/*
 * The exact 3.18 MT6797 clock-buffer owner masks only MDBSI_CON[2:0],
 * writes its modem-on configuration, and delays two microseconds.  It has no
 * completion bit or acknowledgement.  Preserve and restore just the owned
 * fields: doing so does not overwrite independent SPM users which may update
 * another field while this explicitly gated diagnostic is active.
 */
int mt6797_md1_clkbuf_snapshot(struct regmap *spm,
			       struct mt6797_md1_clkbuf_snapshot *snapshot)
{
	unsigned int value;
	int ret;

	if (!spm || !snapshot)
		return -EINVAL;

	snapshot->valid = false;
	ret = regmap_read(spm, MT6797_MD1_SPM_MDBSI_CON, &value);
	if (ret)
		return ret;
	snapshot->mdbsi_con_low = value & MT6797_MD1_SPM_MDBSI_MASK;
	snapshot->valid = true;
	return 0;
}

int mt6797_md1_clkbuf_set_modem_on(struct regmap *spm,
				   struct mt6797_md1_clkbuf_snapshot *snapshot)
{
	unsigned int value;
	int ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MDBSI_CON,
				 MT6797_MD1_SPM_MDBSI_MASK,
				 MT6797_MD1_SPM_MDBSI_MODEM_ON);
	if (ret)
		return ret;
	udelay(2);
	/* This proves the AP-visible command latch only, never clock arrival. */
	ret = regmap_read(spm, MT6797_MD1_SPM_MDBSI_CON, &value);
	if (ret)
		return ret;
	return (value & MT6797_MD1_SPM_MDBSI_MASK) ==
		MT6797_MD1_SPM_MDBSI_MODEM_ON ? 0 : -EIO;
}

int mt6797_md1_clkbuf_restore(struct regmap *spm,
			      struct mt6797_md1_clkbuf_snapshot *snapshot)
{
	int ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MDBSI_CON,
				 MT6797_MD1_SPM_MDBSI_MASK,
				 snapshot->mdbsi_con_low);
	snapshot->valid = false;
	return ret;
}

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

int mt6797_md1_pmic_verify_cold(struct regmap *pmic)
{
	static const unsigned int cold_fields[] = {
		MT6797_MD1_VMODEM_ENABLE,
		MT6797_MD1_VMODEM_SLEEP_ENABLE,
		MT6797_MD1_VMD1_ENABLE,
		MT6797_MD1_VMD1_SLEEP_ENABLE,
		MT6797_MD1_VSRAM_MD_ENABLE,
		MT6797_MD1_VSRAM_MD_SLEEP_ENABLE,
	};
	unsigned int value;
	int i, ret;

	if (!pmic)
		return -EINVAL;

	/* LK md1_power_down() writes these six fields to zero before cold start. */
	for (i = 0; i < ARRAY_SIZE(cold_fields); i++) {
		const struct mt6797_md1_pmic_field *field =
			&mt6797_md1_pmic_fields[cold_fields[i]];

		ret = regmap_read(pmic, field->reg, &value);
		if (ret)
			return ret;
		if (value & field->mask)
			return -EBUSY;
	}
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
		if (field == MT6797_MD1_VMODEM_SELECTOR)
			udelay(300);
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

static int mt6797_md1_spm_wait(struct regmap *spm, bool on)
{
	unsigned int attempt, status, status_2nd;
	int ret;

	for (attempt = 0; attempt < MT6797_MD1_SPM_MAX_POLLS; attempt++) {
		ret = regmap_read(spm, MT6797_MD1_SPM_PWR_STATUS, &status);
		if (ret)
			return ret;
		ret = regmap_read(spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
				  &status_2nd);
		if (ret)
			return ret;
		if (!!(status & MT6797_MD1_SPM_PWR_STATUS_MD1) == on &&
		    !!(status_2nd & MT6797_MD1_SPM_PWR_STATUS_MD1) == on)
			return 0;
		udelay(MT6797_MD1_SPM_POLL_DELAY_US);
	}
	return -ETIMEDOUT;
}

int mt6797_md1_spm_snapshot(struct regmap *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	unsigned int poweron_config, pwr_con, ext_buck_iso;
	int ret;

	if (!spm || !snapshot)
		return -EINVAL;

	snapshot->valid = false;
	/* Do not alter the shared SPM key: it must already be enabled. */
	ret = regmap_read(spm, MT6797_MD1_SPM_POWERON_CONFIG_EN,
			  &poweron_config);
	if (ret)
		return ret;
	if (!(poweron_config & MT6797_MD1_SPM_POWERON_CONFIG_EN_BIT))
		return -EPERM;

	ret = regmap_read(spm, MT6797_MD1_SPM_MD1_PWR_CON, &pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(spm, MT6797_MD1_SPM_MD_EXT_BUCK_ISO, &ext_buck_iso);
	if (ret)
		return ret;
	/* Refuse both a live MD1 and command/isolation state inconsistent with off. */
	if ((pwr_con & MT6797_MD1_SPM_MD1_PWR_CON_OWNED) ||
	    !(ext_buck_iso & MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1))
		return -EBUSY;
	ret = mt6797_md1_spm_wait(spm, false);
	if (ret)
		return ret == -ETIMEDOUT ? -EBUSY : ret;

	snapshot->pwr_con_owned = pwr_con & MT6797_MD1_SPM_MD1_PWR_CON_OWNED;
	snapshot->ext_buck_iso = ext_buck_iso & MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1;
	snapshot->internal_valid = false;
	snapshot->valid = true;
	return 0;
}

int mt6797_md1_spm_power_on(struct regmap *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	int ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	/* Vendor MD1 power-on first clears MD_EXT_BUCK_ISO[0], then PWR_ON. */
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD_EXT_BUCK_ISO,
				 MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ON, MT6797_MD1_SPM_PWR_ON);
	if (ret)
		return ret;
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ON_2ND,
				 MT6797_MD1_SPM_PWR_ON_2ND);
	if (ret)
		return ret;

	return mt6797_md1_spm_wait(spm, true);
}

int mt6797_md1_spm_release_internal_reset_held(struct regmap *spm,
					       struct mt6797_md1_spm_snapshot *snapshot)
{
	unsigned int pwr_con;
	int ret;

	if (!spm || !snapshot || !snapshot->valid || snapshot->internal_valid)
		return -EINVAL;

	ret = regmap_read(spm, MT6797_MD1_SPM_MD1_PWR_CON, &pwr_con);
	if (ret)
		return ret;
	/*
	 * Stage 5 starts only from the exact reset-held, bus-protected state
	 * left by stage 4.  In particular, it never accepts a released reset.
	 */
	if ((pwr_con & MT6797_MD1_SPM_MD1_PWR_CON_OWNED) !=
		MT6797_MD1_SPM_MD1_PWR_CON_OWNED ||
	    (pwr_con & MT6797_MD1_SPM_MD1_PWR_CON_INTERNAL) !=
		MT6797_MD1_SPM_MD1_PWR_CON_INTERNAL ||
	    pwr_con & MT6797_MD1_SPM_PWR_RST_B)
		return -EBUSY;

	snapshot->pwr_con_internal = pwr_con & MT6797_MD1_SPM_MD1_PWR_CON_INTERNAL;
	snapshot->internal_valid = true;

	/* Vendor order after the two PWR_STATUS acknowledgements. */
	ret = regmap_clear_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				MT6797_MD1_SPM_PWR_CLK_DIS);
	if (ret)
		return ret;
	ret = regmap_clear_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				MT6797_MD1_SPM_PWR_ISO);
	if (ret)
		return ret;
	ret = regmap_clear_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				MT6797_MD1_SPM_MD_SRAM_PDN);
	if (ret)
		return ret;

	ret = regmap_read(spm, MT6797_MD1_SPM_MD1_PWR_CON, &pwr_con);
	if (ret)
		return ret;
	if (pwr_con & (MT6797_MD1_SPM_MD1_PWR_CON_INTERNAL |
		       MT6797_MD1_SPM_PWR_RST_B))
		return -EIO;
	return 0;
}

int mt6797_md1_spm_restore_internal(struct regmap *spm,
				    struct mt6797_md1_spm_snapshot *snapshot)
{
	int ret, first_error = 0;

	if (!spm || !snapshot || !snapshot->valid || !snapshot->internal_valid)
		return -EINVAL;

	/*
	 * Reverse the vendor prefix before stage 4 removes PWR_ON.  Bus protection
	 * was never released and PWR_RST_B was never asserted by this stage.
	 */
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_MD_SRAM_PDN,
				 snapshot->pwr_con_internal & MT6797_MD1_SPM_MD_SRAM_PDN);
	if (ret)
		first_error = ret;
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ISO,
				 snapshot->pwr_con_internal & MT6797_MD1_SPM_PWR_ISO);
	if (ret && !first_error)
		first_error = ret;
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_CLK_DIS,
				 snapshot->pwr_con_internal & MT6797_MD1_SPM_PWR_CLK_DIS);
	if (ret && !first_error)
		first_error = ret;
	snapshot->internal_valid = false;
	return first_error;
}

int mt6797_md1_spm_restore(struct regmap *spm,
			    struct mt6797_md1_spm_snapshot *snapshot)
{
	int ret, poll_ret, iso_ret;

	if (!spm || !snapshot || !snapshot->valid)
		return -EINVAL;

	/* Reverse only our owned power requests, wait, then restore buck isolation. */
	ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_MD1_PWR_CON_OWNED,
				 snapshot->pwr_con_owned);
	poll_ret = ret ? ret : mt6797_md1_spm_wait(spm, false);
	/* Attempt isolation restoration even when the prior power write failed. */
	iso_ret = regmap_update_bits(spm, MT6797_MD1_SPM_MD_EXT_BUCK_ISO,
				     MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1,
				     snapshot->ext_buck_iso);
	snapshot->valid = false;
	return ret ?: poll_ret ?: iso_ret;
}
