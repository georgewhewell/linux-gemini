/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Source-backed, but deliberately unarmed, MT6797 MD1 pre-release backend.
 *
 * The structures and helpers below describe the exact vendor MMIO operands
 * that can be checked from source.  They do not create ownership of those
 * registers.  In particular, the MT6797 vendor EMI-MPU path has no reviewed
 * mainline MD1 provider or rollback/readback contract, so the platform
 * adapter always reports its MPU authorization false and never runs these
 * callbacks on hardware.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_BACKEND_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_BACKEND_H

#include <linux/io.h>
#include <linux/bits.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-fence.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>

struct device;
struct regmap;

/* Exact resources validated by the default-disabled platform adapter. */
#define MT6797_MD1_INFRACFG_BASE		0x10001000
#define MT6797_MD1_INFRACFG_SIZE		0x1000
#define MT6797_MD1_AP_CCIF_BASE		0x10209000
#define MT6797_MD1_MD_CCIF_BASE		0x1020a000
#define MT6797_MD1_CCIF_SIZE			0x1000
#define MT6797_MD1_CLKSW_BASE			0x20150000
#define MT6797_MD1_CLKSW_SIZE			0x118

#define MT6797_MD1_INFRA_SRC_CLKENA		0xf0c
#define MT6797_MD1_INFRA_REMAP_LOCK		0xf80
#define MT6797_MD1_INFRA_REMAP_LOCK_MD1	BIT(16)

#define MT6797_MD1_CCIF_BUSY			0x004
#define MT6797_MD1_CCIF_TCHNUM		0x00c
#define MT6797_MD1_CCIF_ACK			0x014
#define MT6797_MD1_CCIF_CHDATA		0x100
#define MT6797_MD1_CCIF_CHANNELS		16
#define MT6797_MD1_CCIF_SRAM_BYTES		512

enum mt6797_md1_pll_poll_stage {
	MT6797_MD1_PLL_PSMCU,
	MT6797_MD1_PLL_L1MCU,
	MT6797_MD1_PLL_DFE_IMC,
	MT6797_MD1_PLL_L1SYS,
	MT6797_MD1_PLL_FLEX1,
	MT6797_MD1_PLL_FLEX2,
	MT6797_MD1_PLL_DCM,
	MT6797_MD1_PLL_DONE,
};

struct mt6797_md1_bringup_backend {
	struct device *dev;
	void __iomem *infracfg;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	void __iomem *md_clksw;
	struct regmap *pmic;
	struct regmap *spm;
	struct mt6797_md1_pmic_snapshot pmic_snapshot;
	struct mt6797_md1_clkbuf_snapshot clkbuf_snapshot;
	struct mt6797_md1_spm_snapshot spm_snapshot;
	enum mt6797_md1_pll_poll_stage pll_stage;
	struct mt6797_md1_remap_values remap_original;
	u32 srcclk_original;
	bool remap_original_valid;
	bool srcclk_valid;
	bool emi_mpu_provider_available;
};

int mt6797_md1_bringup_backend_init(struct mt6797_md1_bringup_backend *backend,
	struct device *dev, void __iomem *infracfg, void __iomem *ap_ccif,
	void __iomem *md_ccif, void __iomem *md_clksw);
void mt6797_md1_bringup_backend_authorization(
	struct mt6797_md1_bringup_backend *backend,
	struct mt6797_md1_bringup_authorization *authorization);

int mt6797_md1_bringup_backend_apply_remap(void *context,
	const struct mt6797_md1_remap_values *values);
int mt6797_md1_bringup_backend_restore_remap(void *context);
int mt6797_md1_bringup_backend_apply_initial_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count);
void mt6797_md1_bringup_backend_mpu_latch_fatal(void *context);
int mt6797_md1_bringup_backend_ccif_observe(void *context,
	struct mt6797_md1_ccif_observation *observation);
int mt6797_md1_bringup_backend_ccif_clear(void *context);
int mt6797_md1_bringup_backend_ccif_verify_cleared(void *context,
	const struct mt6797_md1_ccif_observation *before,
	const struct mt6797_md1_ccif_observation *after);
int mt6797_md1_bringup_backend_power_on_reset_held(void *context);
int mt6797_md1_bringup_backend_enable_srcclk(void *context);
int mt6797_md1_bringup_backend_pll_poll(void *context);
int mt6797_md1_bringup_backend_apply_stage1_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count);
int mt6797_md1_bringup_backend_power_off_reset_held(void *context);

extern const struct mt6797_md1_bringup_ops mt6797_md1_bringup_backend_ops;

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_BACKEND_H */
