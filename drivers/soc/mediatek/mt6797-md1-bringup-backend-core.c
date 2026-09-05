// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 MD1 source-backed pre-release MMIO helpers.
 *
 * Vendor references:
 *   ccci_platform.c:183-187, 688-689, 760-761, 932-962
 *   cldma_platform.c:659-789, 791-852
 *   modem_cldma.c:2142-2167
 *   dguidipc/gemini-lk-android8@f4988d7:
 *     lk/platform/mt6797/ccci_lk_load_img_plat.c:72-90, 208-230
 *
 * No function here discovers physical resources or starts a transaction.  The
 * companion platform adapter maps resources only behind independent module and
 * DT gates, records that MD1 EMI-MPU ownership is unknown, and has no run
 * path.  The immutable fence therefore refuses all callbacks on hardware.
 */

#include <linux/errno.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-backend.h>
#include <linux/string.h>

struct mt6797_md1_pll_check {
	u32 offset;
	u32 mask;
};

/* In vendor order, each loop waits for every bit in its mask to become one. */
static const struct mt6797_md1_pll_check mt6797_md1_pll_checks[] = {
	[MT6797_MD1_PLL_PSMCU] = { 0x040, 0x00000001 },
	[MT6797_MD1_PLL_L1MCU] = { 0x040, 0x00000002 },
	[MT6797_MD1_PLL_DFE_IMC] = { 0x040, 0x00000090 },
	[MT6797_MD1_PLL_L1SYS] = { 0x030, 0x80800000 },
	[MT6797_MD1_PLL_FLEX1] = { 0x034, 0x80808080 },
	[MT6797_MD1_PLL_FLEX2] = { 0x048, 0x00008080 },
	[MT6797_MD1_PLL_DCM] = { 0x030, 0x00000080 },
};

static int mt6797_md1_bringup_write_verify(void __iomem *base, u32 offset,
					  u32 value)
{
	writel(value, base + offset);
	return readl(base + offset) == value ? 0 : -EIO;
}

static int mt6797_md1_bringup_first_error(int first, int next)
{
	return first ?: next;
}

int mt6797_md1_bringup_backend_init(struct mt6797_md1_bringup_backend *backend,
	struct device *dev, void __iomem *infracfg, void __iomem *ap_ccif,
	void __iomem *md_ccif, void __iomem *md_clksw)
{
	if (!backend || !infracfg || !ap_ccif || !md_ccif || !md_clksw)
		return -EINVAL;

	memset(backend, 0, sizeof(*backend));
	backend->dev = dev;
	backend->infracfg = infracfg;
	backend->ap_ccif = ap_ccif;
	backend->md_ccif = md_ccif;
	backend->md_clksw = md_clksw;
	backend->pll_stage = MT6797_MD1_PLL_PSMCU;
	return 0;
}

void mt6797_md1_bringup_backend_authorization(
	struct mt6797_md1_bringup_backend *backend,
	struct mt6797_md1_bringup_authorization *authorization)
{
	if (!authorization)
		return;

	/* The provider captures SET status and latches fatal; all other owners are
	 * deliberately still unproven, so this inventory cannot prepare or run.
	 */
	memset(authorization, 0, sizeof(*authorization));
	authorization->mpu_owner_and_failure_fence =
		backend && backend->emi_mpu_provider_available;
	if (backend && backend->dev)
		dev_warn(backend->dev,
			 "MD1 backend mapped but not runnable: secure EMI-MPU is one-way; remap, CCIF, power and clock ownership are unproven; no MMIO is issued\n");
}

static int mt6797_md1_bringup_remap_is_locked(
	struct mt6797_md1_bringup_backend *backend)
{
	if (readl(backend->infracfg + MT6797_MD1_INFRA_REMAP_LOCK) &
	    MT6797_MD1_INFRA_REMAP_LOCK_MD1) {
		if (backend->dev)
			dev_err(backend->dev,
				"MD1 remap refused: LK one-way remap lock is set\n");
		return -EUCLEAN;
	}
	return 0;
}

static void mt6797_md1_bringup_remap_snapshot(
	struct mt6797_md1_bringup_backend *backend)
{
	backend->remap_original.bank0_map0 = readl(backend->infracfg +
		MT6797_MD1_BANK0_MAP0_OFFSET);
	backend->remap_original.bank0_map1 = readl(backend->infracfg +
		MT6797_MD1_BANK0_MAP1_OFFSET);
	backend->remap_original.bank4_map0 = readl(backend->infracfg +
		MT6797_MD1_BANK4_MAP0_OFFSET);
	backend->remap_original.bank4_map1 = readl(backend->infracfg +
		MT6797_MD1_BANK4_MAP1_OFFSET);
	backend->remap_original_valid = true;
}

int mt6797_md1_bringup_backend_apply_remap(void *context,
	const struct mt6797_md1_remap_values *values)
{
	struct mt6797_md1_bringup_backend *backend = context;
	int ret;

	if (!backend || !backend->infracfg || !values)
		return -EINVAL;
	ret = mt6797_md1_bringup_remap_is_locked(backend);
	if (ret)
		return ret;
	mt6797_md1_bringup_remap_snapshot(backend);

	/* Exact MD1 BANK0 then BANK4 write order from vendor CCCI. */
	ret = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK0_MAP0_OFFSET, values->bank0_map0);
	if (ret)
		return ret;
	ret = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK0_MAP1_OFFSET, values->bank0_map1);
	if (ret)
		return ret;
	ret = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK4_MAP0_OFFSET, values->bank4_map0);
	if (ret)
		return ret;
	return mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK4_MAP1_OFFSET, values->bank4_map1);
}

int mt6797_md1_bringup_backend_restore_remap(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	int ret = 0, next;

	if (!backend || !backend->infracfg)
		return -EINVAL;
	if (!backend->remap_original_valid)
		return 0;

	/* LK makes this lock one-way; never overwrite a locked remap. */
	next = mt6797_md1_bringup_remap_is_locked(backend);
	if (next)
		return next;
	next = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK0_MAP0_OFFSET, backend->remap_original.bank0_map0);
	ret = mt6797_md1_bringup_first_error(ret, next);
	next = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK0_MAP1_OFFSET, backend->remap_original.bank0_map1);
	ret = mt6797_md1_bringup_first_error(ret, next);
	next = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK4_MAP0_OFFSET, backend->remap_original.bank4_map0);
	ret = mt6797_md1_bringup_first_error(ret, next);
	next = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_BANK4_MAP1_OFFSET, backend->remap_original.bank4_map1);
	ret = mt6797_md1_bringup_first_error(ret, next);
	if (!ret)
		backend->remap_original_valid = false;
	return ret;
}

int mt6797_md1_bringup_backend_apply_initial_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	struct mt6797_md1_bringup_backend *backend = context;

	if (!backend || !plan || !entries ||
	    count != MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES)
		return -EINVAL;
	if (!backend->emi_mpu_provider_available || !backend->dev)
		return -EOPNOTSUPP;
	return mt6797_md1_emi_mpu_apply(backend->dev, plan,
		MT6797_MD1_EMI_MPU_INITIAL, entries, count);
}

void mt6797_md1_bringup_backend_mpu_latch_fatal(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;

	mt6797_md1_emi_mpu_latch_fatal();
	if (backend && backend->dev)
		dev_crit(backend->dev,
			 "MD1 secure MPU transaction fenced; reset/reboot required before any future release review\n");
}

int mt6797_md1_bringup_backend_ccif_observe(void *context,
	struct mt6797_md1_ccif_observation *observation)
{
	struct mt6797_md1_bringup_backend *backend = context;
	u32 word;

	if (!backend || !backend->ap_ccif || !backend->md_ccif || !observation)
		return -EINVAL;

	memset(observation, 0, sizeof(*observation));
	observation->ap_busy = readl(backend->ap_ccif + MT6797_MD1_CCIF_BUSY);
	observation->md_busy = readl(backend->md_ccif + MT6797_MD1_CCIF_BUSY);
	observation->ap_ack = readl(backend->ap_ccif + MT6797_MD1_CCIF_ACK);
	observation->md_ack = readl(backend->md_ccif + MT6797_MD1_CCIF_ACK);
	for (word = 0; word < MT6797_MD1_CCIF_SRAM_BYTES / sizeof(u32); word++) {
		if (readl(backend->ap_ccif + MT6797_MD1_CCIF_CHDATA +
			  word * sizeof(u32)))
			observation->ap_sram_nonzero_words++;
		if (readl(backend->md_ccif + MT6797_MD1_CCIF_CHDATA +
			  word * sizeof(u32)))
			observation->md_sram_nonzero_words++;
	}
	return 0;
}

int mt6797_md1_bringup_backend_ccif_clear(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	u32 channel;

	if (!backend || !backend->ap_ccif || !backend->md_ccif)
		return -EINVAL;

	/* This is ccci_reset_ccif_hw() verbatim in operation, including both sides. */
	for (channel = 0; channel < MT6797_MD1_CCIF_CHANNELS; channel++) {
		if (readl(backend->ap_ccif + MT6797_MD1_CCIF_BUSY) & BIT(channel))
			writel(channel, backend->ap_ccif + MT6797_MD1_CCIF_TCHNUM);
		if (readl(backend->md_ccif + MT6797_MD1_CCIF_BUSY) & BIT(channel))
			writel(channel, backend->md_ccif + MT6797_MD1_CCIF_TCHNUM);
	}
	writel(readl(backend->md_ccif + MT6797_MD1_CCIF_BUSY),
	       backend->ap_ccif + MT6797_MD1_CCIF_ACK);
	writel(readl(backend->ap_ccif + MT6797_MD1_CCIF_BUSY),
	       backend->md_ccif + MT6797_MD1_CCIF_ACK);
	for (channel = 0; channel < MT6797_MD1_CCIF_SRAM_BYTES / sizeof(u32);
	     channel++) {
		writel(0, backend->ap_ccif + MT6797_MD1_CCIF_CHDATA +
		       channel * sizeof(u32));
		writel(0, backend->md_ccif + MT6797_MD1_CCIF_CHDATA +
		       channel * sizeof(u32));
	}
	return 0;
}

int mt6797_md1_bringup_backend_ccif_verify_cleared(void *context,
	const struct mt6797_md1_ccif_observation *before,
	const struct mt6797_md1_ccif_observation *after)
{
	struct mt6797_md1_bringup_backend *backend = context;

	if (!backend || !before || !after)
		return -EINVAL;
	/* Vendor supplies writes but not ACK readback semantics; retain both values
	 * for logs, and enforce only the source-observable BUSY/SRAM postcondition.
	 */
	if (after->ap_busy || after->md_busy || after->ap_sram_nonzero_words ||
	    after->md_sram_nonzero_words)
		return -EUCLEAN;
	if (backend->dev)
		dev_info(backend->dev,
			 "CCIF observed busy %#x/%#x -> %#x/%#x, ACK %#x/%#x -> %#x/%#x\n",
			 before->ap_busy, before->md_busy, after->ap_busy, after->md_busy,
			 before->ap_ack, before->md_ack, after->ap_ack, after->md_ack);
	return 0;
}

int mt6797_md1_bringup_backend_power_on_reset_held(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	int ret;

	if (!backend || !backend->pmic || !backend->spm)
		return -EOPNOTSUPP;
	ret = mt6797_md1_pmic_snapshot(backend->pmic, &backend->pmic_snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_pmic_apply_vendor_values(backend->pmic,
						  &backend->pmic_snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_clkbuf_snapshot(backend->spm, &backend->clkbuf_snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_clkbuf_set_modem_on(backend->spm,
					      &backend->clkbuf_snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_spm_snapshot(backend->spm, &backend->spm_snapshot);
	if (ret)
		return ret;
	ret = mt6797_md1_spm_power_on(backend->spm, &backend->spm_snapshot);
	if (ret)
		return ret;
	/* This vendor-derived helper deliberately leaves PWR_RST_B asserted. */
	return mt6797_md1_spm_release_internal_reset_held(backend->spm,
						       &backend->spm_snapshot);
}

int mt6797_md1_bringup_backend_enable_srcclk(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	u32 value;
	int ret;

	if (!backend || !backend->infracfg)
		return -EINVAL;

	backend->srcclk_original = readl(backend->infracfg +
					  MT6797_MD1_INFRA_SRC_CLKENA);
	backend->srcclk_valid = true;
	value = (backend->srcclk_original & ~0xff) | 0x29;
	ret = mt6797_md1_bringup_write_verify(backend->infracfg,
		MT6797_MD1_INFRA_SRC_CLKENA, value);
	if (ret && backend->dev)
		dev_err(backend->dev, "MD1 source-clock readback failed\n");
	return ret;
}

int mt6797_md1_bringup_backend_pll_poll(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	const struct mt6797_md1_pll_check *check;

	if (!backend || !backend->md_clksw ||
	    backend->pll_stage >= MT6797_MD1_PLL_DONE)
		return -EINVAL;
	check = &mt6797_md1_pll_checks[backend->pll_stage];
	if ((readl(backend->md_clksw + check->offset) & check->mask) != check->mask)
		return -EAGAIN;
	backend->pll_stage++;
	return backend->pll_stage == MT6797_MD1_PLL_DONE ? 0 : -EAGAIN;
}

int mt6797_md1_bringup_backend_apply_stage1_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	struct mt6797_md1_bringup_backend *backend = context;

	if (!backend || !plan || !entries ||
	    count != MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES)
		return -EINVAL;
	if (!backend->emi_mpu_provider_available || !backend->dev)
		return -EOPNOTSUPP;
	return mt6797_md1_emi_mpu_apply(backend->dev, plan,
		MT6797_MD1_EMI_MPU_STAGE1, entries, count);
}

int mt6797_md1_bringup_backend_power_off_reset_held(void *context)
{
	struct mt6797_md1_bringup_backend *backend = context;
	int ret = 0, next;

	if (!backend)
		return -EINVAL;
	if (backend->spm_snapshot.internal_valid) {
		next = mt6797_md1_spm_restore_internal(backend->spm,
						       &backend->spm_snapshot);
		ret = mt6797_md1_bringup_first_error(ret, next);
	}
	if (backend->spm_snapshot.valid) {
		next = mt6797_md1_spm_restore(backend->spm, &backend->spm_snapshot);
		ret = mt6797_md1_bringup_first_error(ret, next);
	}
	if (backend->srcclk_valid) {
		next = mt6797_md1_bringup_write_verify(backend->infracfg,
			MT6797_MD1_INFRA_SRC_CLKENA, backend->srcclk_original);
		ret = mt6797_md1_bringup_first_error(ret, next);
	}
	if (backend->clkbuf_snapshot.valid) {
		next = mt6797_md1_clkbuf_restore(backend->spm,
						 &backend->clkbuf_snapshot);
		ret = mt6797_md1_bringup_first_error(ret, next);
	}
	if (backend->pmic_snapshot.valid) {
		next = mt6797_md1_pmic_restore(backend->pmic, &backend->pmic_snapshot);
		ret = mt6797_md1_bringup_first_error(ret, next);
	}
	return ret;
}

const struct mt6797_md1_bringup_ops mt6797_md1_bringup_backend_ops = {
	.abi = MT6797_MD1_BRINGUP_FENCE_ABI,
	.apply_remap = mt6797_md1_bringup_backend_apply_remap,
	.restore_remap = mt6797_md1_bringup_backend_restore_remap,
	.apply_initial_mpu = mt6797_md1_bringup_backend_apply_initial_mpu,
	.mpu_latch_fatal = mt6797_md1_bringup_backend_mpu_latch_fatal,
	.ccif_observe = mt6797_md1_bringup_backend_ccif_observe,
	.ccif_clear = mt6797_md1_bringup_backend_ccif_clear,
	.ccif_verify_cleared = mt6797_md1_bringup_backend_ccif_verify_cleared,
	.power_on_reset_held = mt6797_md1_bringup_backend_power_on_reset_held,
	.enable_srcclk = mt6797_md1_bringup_backend_enable_srcclk,
	.pll_poll = mt6797_md1_bringup_backend_pll_poll,
	.apply_stage1_mpu = mt6797_md1_bringup_backend_apply_stage1_mpu,
	.power_off_reset_held = mt6797_md1_bringup_backend_power_off_reset_held,
};
