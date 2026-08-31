// SPDX-License-Identifier: GPL-2.0-only
/*
 * Default-disabled MT6797 MD1 cold power/PLL/CCIF owner.
 *
 * The MD1 power portion follows the active Gemini vendor CCF implementation,
 * drivers/clk/mediatek/clk-mt6797-pg.c::spm_mtcmos_ctrl_md1(). In particular,
 * it must not borrow the unbuilt mt_spm_mtcmos.c PCM/C2K path: the two paths
 * use different isolation and bus-protection controls.
 *
 * The explicitly separately-gated terminal stage follows vendor MD1 WDT
 * writes with an AP-visible status sample, applies the staged secure MPU
 * contract, writes MD_BOOT_VECTOR_EN once, and observes boot-status/CCIF.
 * It intentionally stops before IRQ, CLDMA, shared-memory or CCCI transport.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-boot-status-observe.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>
#include <linux/soc/mediatek/mt6797-md1-wdt-gate.h>

#define MT6797_MD1_INFRA_BASE			0x10001000
#define MT6797_MD1_INFRA_SIZE			0x1000
#define MT6797_MD1_AP_CCIF_BASE		0x10209000
#define MT6797_MD1_MD_CCIF_BASE		0x1020a000
#define MT6797_MD1_CCIF_SIZE			0x1000
#define MT6797_MD1_APMIXED_BASE		0x1000c000
#define MT6797_MD1_APMIXED_SIZE		0x1000
#define MT6797_MD1_CLKSW_BASE			0x20150000
#define MT6797_MD1_CLKSW_SIZE			0x118
#define MT6797_MD1_DCM_BASE			0x20130000
#define MT6797_MD1_DCM_SIZE			0x1004
#define MT6797_MD1_PERI_MISC_BASE		0x20060000
#define MT6797_MD1_PERI_MISC_SIZE		0xd0
#define MT6797_MD1_L1_A0_BASE			0x260f0000
#define MT6797_MD1_L1_A0_SIZE			0x200
#define MT6797_MD1_TOP_PLL_BASE		0x20140000
/* The vendor accesses offsets 0x10 and 0x14 from this base. */
#define MT6797_MD1_TOP_PLL_SIZE		0x18
#define MT6797_MD1_SYS_CLK_BASE		0x20120000
#define MT6797_MD1_SYS_CLK_SIZE		0xd0
#define MT6797_MD1_RGU_BASE			0x200f0100
#define MT6797_MD1_RGU_SIZE			0x300
#define MT6797_MD1_L1_RGU_BASE			0x26010000
#define MT6797_MD1_L1_RGU_SIZE			0x40
#define MT6797_MD1_BOOT_VECTOR_BASE		0x20000024
#define MT6797_MD1_BOOT_VECTOR_SIZE		0x4
#define MT6797_MD1_BOOT_STATUS_BASE		0x10201300
#define MT6797_MD1_BOOT_STATUS_SIZE		0x8

#define MT6797_MD1_INFRA_PROTECT_EN		0x220
#define MT6797_MD1_INFRA_PROTECT_STA1		0x228
#define MT6797_MD1_INFRA_SRC_CLKENA		0xf0c
#define MT6797_MD1_INFRA_MD1_PROT_MASK	(BIT(3) | BIT(4) | BIT(5) | \
					 BIT(7) | BIT(10) | BIT(16))

#define MT6797_MD1_SPM_POWERON_CONFIG_EN	0x000
#define MT6797_MD1_SPM_PWR_STATUS		0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND		0x184
#define MT6797_MD1_SPM_MD1_PWR_CON		0x320
#define MT6797_MD1_SPM_MD_EXT_BUCK_ISO		0x390
#define MT6797_MD1_SPM_PWR_STATUS_MD1		BIT(0)
#define MT6797_MD1_SPM_POWERON_CONFIG_EN_BIT	BIT(0)
#define MT6797_MD1_SPM_PWR_RST_B		BIT(0)
#define MT6797_MD1_SPM_PWR_ISO			BIT(1)
#define MT6797_MD1_SPM_PWR_ON			BIT(2)
#define MT6797_MD1_SPM_PWR_ON_2ND		BIT(3)
#define MT6797_MD1_SPM_PWR_CLK_DIS		BIT(4)
#define MT6797_MD1_SPM_MD_SRAM_PDN		BIT(8)
#define MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1	BIT(0)
#define MT6797_MD1_SPM_COLD_MASK		(MT6797_MD1_SPM_PWR_RST_B | \
	MT6797_MD1_SPM_PWR_ISO | MT6797_MD1_SPM_PWR_ON | \
	MT6797_MD1_SPM_PWR_ON_2ND | MT6797_MD1_SPM_PWR_CLK_DIS | \
	MT6797_MD1_SPM_MD_SRAM_PDN)
#define MT6797_MD1_SPM_COLD_VALUE		(MT6797_MD1_SPM_PWR_ISO | \
	MT6797_MD1_SPM_PWR_CLK_DIS | MT6797_MD1_SPM_MD_SRAM_PDN)
#define MT6797_MD1_SPM_POWERED_VALUE		(MT6797_MD1_SPM_PWR_RST_B | \
	MT6797_MD1_SPM_PWR_ON | MT6797_MD1_SPM_PWR_ON_2ND)

#define MT6797_MD1_CCIF_BUSY			0x004
#define MT6797_MD1_CCIF_TCHNUM			0x00c
#define MT6797_MD1_CCIF_ACK			0x014
#define MT6797_MD1_CCIF_CHDATA			0x100
#define MT6797_MD1_CCIF_CHANNELS		16
#define MT6797_MD1_CCIF_SRAM_BYTES		512

/* cldma_platform.h WDT mode/status offsets and exact MD1 keys. */
#define MT6797_MD1_WDT_MD_MODE			0x000
#define MT6797_MD1_WDT_MD_STATUS		0x034
#define MT6797_MD1_WDT_L1_MODE			0x000
#define MT6797_MD1_WDT_L1_STATUS		0x00c
#define MT6797_MD1_WDT_MD_MODE_KEY		0x55000008
#define MT6797_MD1_WDT_L1_MODE_KEY		0x00002200
#define MT6797_MD1_BOOT_VECTOR_ENABLE		1
#define MT6797_MD1_BOOT_STATUS0			0x0
#define MT6797_MD1_BOOT_STATUS1			0x4

#define MT6797_MD1_MAX_POWER_POLLS		100
#define MT6797_MD1_MAX_BOOT_OBSERVE_POLLS	1000
#define MT6797_MD1_BOOT_OBSERVE_DELAY_US	100

struct mt6797_md1_cold_owner_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

enum mt6797_md1_cold_owner_resource_id {
	MT6797_MD1_RES_INFRA,
	MT6797_MD1_RES_AP_CCIF,
	MT6797_MD1_RES_MD_CCIF,
	MT6797_MD1_RES_APMIXED,
	MT6797_MD1_RES_CLKSW,
	MT6797_MD1_RES_DCM,
	MT6797_MD1_RES_PERI_MISC,
	MT6797_MD1_RES_L1_A0,
	MT6797_MD1_RES_TOP_PLL,
	MT6797_MD1_RES_SYS_CLK,
	MT6797_MD1_RES_MD_RGU,
	MT6797_MD1_RES_L1_RGU,
	MT6797_MD1_RES_BOOT_VECTOR,
	MT6797_MD1_RES_BOOT_STATUS,
	MT6797_MD1_RES_COUNT,
};

static const struct mt6797_md1_cold_owner_resource mt6797_md1_cold_resources[] = {
	[MT6797_MD1_RES_INFRA] = { "infracfg", MT6797_MD1_INFRA_BASE,
		MT6797_MD1_INFRA_SIZE },
	[MT6797_MD1_RES_AP_CCIF] = { "ap-ccif", MT6797_MD1_AP_CCIF_BASE,
		MT6797_MD1_CCIF_SIZE },
	[MT6797_MD1_RES_MD_CCIF] = { "md-ccif", MT6797_MD1_MD_CCIF_BASE,
		MT6797_MD1_CCIF_SIZE },
	[MT6797_MD1_RES_APMIXED] = { "apmixed", MT6797_MD1_APMIXED_BASE,
		MT6797_MD1_APMIXED_SIZE },
	[MT6797_MD1_RES_CLKSW] = { "md-clksw", MT6797_MD1_CLKSW_BASE,
		MT6797_MD1_CLKSW_SIZE },
	[MT6797_MD1_RES_DCM] = { "md-dcm", MT6797_MD1_DCM_BASE,
		MT6797_MD1_DCM_SIZE },
	[MT6797_MD1_RES_PERI_MISC] = { "md-peri-misc",
		MT6797_MD1_PERI_MISC_BASE, MT6797_MD1_PERI_MISC_SIZE },
	[MT6797_MD1_RES_L1_A0] = { "md-l1-a0", MT6797_MD1_L1_A0_BASE,
		MT6797_MD1_L1_A0_SIZE },
	[MT6797_MD1_RES_TOP_PLL] = { "md-top-pll", MT6797_MD1_TOP_PLL_BASE,
		MT6797_MD1_TOP_PLL_SIZE },
	[MT6797_MD1_RES_SYS_CLK] = { "md-sys-clk", MT6797_MD1_SYS_CLK_BASE,
		MT6797_MD1_SYS_CLK_SIZE },
	[MT6797_MD1_RES_MD_RGU] = { "md-rgu", MT6797_MD1_RGU_BASE,
		MT6797_MD1_RGU_SIZE },
	[MT6797_MD1_RES_L1_RGU] = { "l1-rgu", MT6797_MD1_L1_RGU_BASE,
		MT6797_MD1_L1_RGU_SIZE },
	[MT6797_MD1_RES_BOOT_VECTOR] = { "boot-vector", MT6797_MD1_BOOT_VECTOR_BASE,
		MT6797_MD1_BOOT_VECTOR_SIZE },
	[MT6797_MD1_RES_BOOT_STATUS] = { "boot-status", MT6797_MD1_BOOT_STATUS_BASE,
		MT6797_MD1_BOOT_STATUS_SIZE },
};

struct mt6797_md1_cold_owner_backend {
	struct device *dev;
	void __iomem *base[MT6797_MD1_RES_COUNT];
	struct regmap *pmic;
	struct regmap *spm;
	struct mt6797_md1_pmic_snapshot pmic_snapshot;
	struct mt6797_md1_clkbuf_snapshot clkbuf_snapshot;
	enum mt6797_md1_cold_owner_pll_stage pll_stage;
	struct mt6797_md1_wdt_gate wdt_gate;
};

static bool enable_experimental_md1_cold_owner;
static bool enable_experimental_md1_boot_vector_release;
/* A failed post-observation transaction may have changed MD1 state. */
static atomic_t mt6797_md1_cold_owner_fatal_latch = ATOMIC_INIT(0);
module_param_named(enable_experimental_md1_cold_owner,
			   enable_experimental_md1_cold_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_cold_owner,
	"run the one-shot, source-backed MT6797 MD1 cold owner (default: false)");
module_param_named(enable_experimental_md1_boot_vector_release,
			enable_experimental_md1_boot_vector_release, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_boot_vector_release,
	"allow the separately gated MT6797 MD1 WDT/boot-vector candidate (default: false)");

static int mt6797_md1_cold_map_resource(struct platform_device *pdev,
	enum mt6797_md1_cold_owner_resource_id id, void __iomem **base)
{
	const struct mt6797_md1_cold_owner_resource *expected =
		&mt6797_md1_cold_resources[id];
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing %s resource\n",
				     expected->name);
	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid %s resource\n",
				     expected->name);
	*base = devm_ioremap_resource(&pdev->dev, res);
	return IS_ERR(*base) ? PTR_ERR(*base) : 0;
}

static struct regmap *mt6797_md1_cold_pmic_regmap(struct device *dev)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct regmap *regmap;

	node = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	return regmap ?: ERR_PTR(-EPROBE_DEFER);
}

static int mt6797_md1_cold_wait_power(struct mt6797_md1_cold_owner_backend *backend,
	bool on)
{
	unsigned int i, status, status_2nd;
	int ret;

	for (i = 0; i < MT6797_MD1_MAX_POWER_POLLS; i++) {
		ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS, &status);
		if (ret)
			return ret;
		ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
				  &status_2nd);
		if (ret)
			return ret;
		if (!!(status & MT6797_MD1_SPM_PWR_STATUS_MD1) == on &&
		    !!(status_2nd & MT6797_MD1_SPM_PWR_STATUS_MD1) == on)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int mt6797_md1_cold_wait_bus_protect(
	struct mt6797_md1_cold_owner_backend *backend, bool asserted)
{
	unsigned int i;
	u32 value;

	for (i = 0; i < MT6797_MD1_MAX_POWER_POLLS; i++) {
		value = readl(backend->base[MT6797_MD1_RES_INFRA] +
			      MT6797_MD1_INFRA_PROTECT_STA1);
		if (asserted ?
		    (value & MT6797_MD1_INFRA_MD1_PROT_MASK) ==
			MT6797_MD1_INFRA_MD1_PROT_MASK :
		    !(value & MT6797_MD1_INFRA_MD1_PROT_MASK))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int mt6797_md1_cold_verify(void *context,
	struct mt6797_md1_cold_state *state)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	int ret;

	if (!backend || !state)
		return -EINVAL;
	ret = mt6797_md1_pmic_verify_cold(backend->pmic);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_POWERON_CONFIG_EN,
			  &state->poweron_config);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
			  &state->md1_pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS,
			  &state->pwr_status);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
			  &state->pwr_status_2nd);
	if (ret)
		return ret;
	state->infra_protect_en = readl(backend->base[MT6797_MD1_RES_INFRA] +
		MT6797_MD1_INFRA_PROTECT_EN);
	state->infra_protect_sta = readl(backend->base[MT6797_MD1_RES_INFRA] +
		MT6797_MD1_INFRA_PROTECT_STA1);
	if (!(state->poweron_config & MT6797_MD1_SPM_POWERON_CONFIG_EN_BIT) ||
	    (state->md1_pwr_con & MT6797_MD1_SPM_COLD_MASK) !=
		MT6797_MD1_SPM_COLD_VALUE ||
	    state->pwr_status & MT6797_MD1_SPM_PWR_STATUS_MD1 ||
	    state->pwr_status_2nd & MT6797_MD1_SPM_PWR_STATUS_MD1 ||
	    (state->infra_protect_en & MT6797_MD1_INFRA_MD1_PROT_MASK) !=
		MT6797_MD1_INFRA_MD1_PROT_MASK ||
	    (state->infra_protect_sta & MT6797_MD1_INFRA_MD1_PROT_MASK) !=
		MT6797_MD1_INFRA_MD1_PROT_MASK)
		return -EBUSY;
	return 0;
}

static int mt6797_md1_cold_ccif_observe(void *context,
	struct mt6797_md1_cold_owner_ccif *state)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	u32 word;

	if (!backend || !state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));
	state->ap_busy = readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_BUSY);
	state->md_busy = readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_BUSY);
	state->ap_ack = readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_ACK);
	state->md_ack = readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_ACK);
	for (word = 0; word < MT6797_MD1_CCIF_SRAM_BYTES / sizeof(u32); word++) {
		if (readl(backend->base[MT6797_MD1_RES_AP_CCIF] +
			  MT6797_MD1_CCIF_CHDATA + word * sizeof(u32)))
			state->ap_sram_nonzero_words++;
		if (readl(backend->base[MT6797_MD1_RES_MD_CCIF] +
			  MT6797_MD1_CCIF_CHDATA + word * sizeof(u32)))
			state->md_sram_nonzero_words++;
	}
	return 0;
}

static int mt6797_md1_cold_ccif_clear(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	u32 channel;

	if (!backend)
		return -EINVAL;
	for (channel = 0; channel < MT6797_MD1_CCIF_CHANNELS; channel++) {
		if (readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_BUSY) &
		    BIT(channel))
			writel(channel, backend->base[MT6797_MD1_RES_AP_CCIF] +
			       MT6797_MD1_CCIF_TCHNUM);
		if (readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_BUSY) &
		    BIT(channel))
			writel(channel, backend->base[MT6797_MD1_RES_MD_CCIF] +
			       MT6797_MD1_CCIF_TCHNUM);
	}
	writel(readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_BUSY),
	       backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_ACK);
	writel(readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_BUSY),
	       backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_ACK);
	for (channel = 0; channel < MT6797_MD1_CCIF_SRAM_BYTES / sizeof(u32); channel++) {
		writel(0, backend->base[MT6797_MD1_RES_AP_CCIF] +
		       MT6797_MD1_CCIF_CHDATA + channel * sizeof(u32));
		writel(0, backend->base[MT6797_MD1_RES_MD_CCIF] +
		       MT6797_MD1_CCIF_CHDATA + channel * sizeof(u32));
	}
	return 0;
}

static int mt6797_md1_cold_ccif_verify(void *context,
	const struct mt6797_md1_cold_owner_ccif *before,
	const struct mt6797_md1_cold_owner_ccif *after)
{
	struct mt6797_md1_cold_owner_backend *backend = context;

	if (!backend || !before || !after)
		return -EINVAL;
	if (after->ap_busy || after->md_busy || after->ap_sram_nonzero_words ||
	    after->md_sram_nonzero_words)
		return -EUCLEAN;
	dev_info(backend->dev,
		 "MD1 CCIF busy %#x/%#x -> %#x/%#x ACK %#x/%#x -> %#x/%#x\n",
		 before->ap_busy, before->md_busy, after->ap_busy, after->md_busy,
		 before->ap_ack, before->md_ack, after->ap_ack, after->md_ack);
	return 0;
}

static int mt6797_md1_cold_power_on(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	unsigned int pwr_con;
	u32 value;
	int ret;

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

	/* Exact active CCF order: buck isolation, requests, both acknowledgements. */
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD_EXT_BUCK_ISO,
				 MT6797_MD1_SPM_MD_EXT_BUCK_ISO_MD1, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ON, MT6797_MD1_SPM_PWR_ON);
	if (ret)
		return ret;
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ON_2ND, MT6797_MD1_SPM_PWR_ON_2ND);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_wait_power(backend, true);
	if (ret)
		return ret;

	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_CLK_DIS, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_ISO, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_MD_SRAM_PDN, 0);
	if (ret)
		return ret;
	value = readl(backend->base[MT6797_MD1_RES_INFRA] +
		      MT6797_MD1_INFRA_PROTECT_EN);
	writel(value & ~MT6797_MD1_INFRA_MD1_PROT_MASK,
	       backend->base[MT6797_MD1_RES_INFRA] + MT6797_MD1_INFRA_PROTECT_EN);
	ret = mt6797_md1_cold_wait_bus_protect(backend, false);
	if (ret)
		return ret;
	ret = regmap_update_bits(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON,
				 MT6797_MD1_SPM_PWR_RST_B, MT6797_MD1_SPM_PWR_RST_B);
	if (ret)
		return ret;

	ret = regmap_read(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON, &pwr_con);
	if (ret)
		return ret;
	if ((pwr_con & MT6797_MD1_SPM_COLD_MASK) != MT6797_MD1_SPM_POWERED_VALUE)
		return -EIO;
	return 0;
}

static int mt6797_md1_cold_write_or(void __iomem *base, u32 offset, u32 bits)
{
	u32 value = readl(base + offset) | bits;

	writel(value, base + offset);
	return (readl(base + offset) & bits) == bits ? 0 : -EIO;
}

static int mt6797_md1_cold_write_and(void __iomem *base, u32 offset, u32 mask)
{
	u32 value = readl(base + offset) & mask;

	writel(value, base + offset);
	return readl(base + offset) == value ? 0 : -EIO;
}

static int mt6797_md1_cold_write_value(void __iomem *base, u32 offset, u32 value)
{
	writel(value, base + offset);
	return readl(base + offset) == value ? 0 : -EIO;
}

static int mt6797_md1_cold_enable_srcclk(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	void __iomem *infra = backend->base[MT6797_MD1_RES_INFRA];
	u32 value = readl(infra + MT6797_MD1_INFRA_SRC_CLKENA);

	value = (value & ~0xff) | 0x29;
	return mt6797_md1_cold_write_value(infra, MT6797_MD1_INFRA_SRC_CLKENA,
					     value);
}

static int mt6797_md1_cold_pll_start(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	void __iomem *apmixed = backend->base[MT6797_MD1_RES_APMIXED];
	void __iomem *clksw = backend->base[MT6797_MD1_RES_CLKSW];
	void __iomem *top_pll = backend->base[MT6797_MD1_RES_TOP_PLL];
	void __iomem *peri = backend->base[MT6797_MD1_RES_PERI_MISC];
	void __iomem *sys = backend->base[MT6797_MD1_RES_SYS_CLK];
	void __iomem *l1 = backend->base[MT6797_MD1_RES_L1_A0];
	int ret;

	ret = mt6797_md1_cold_write_value(apmixed, 0x2c8, 0x2ee8);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(clksw, 0x20, BIT(26));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(top_pll, 0x10, BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(top_pll, 0x14, BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(apmixed, 0x2c8, BIT(0));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(apmixed, 0x0, BIT(1));
	if (ret)
		return ret;
	udelay(200);
	ret = mt6797_md1_cold_write_and(apmixed, 0x2c8, ~(u32)BIT(0));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_and(apmixed, 0x2c8, ~(u32)BIT(7));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_and(top_pll, 0x10, ~(u32)BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_and(top_pll, 0x14, ~(u32)BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_and(clksw, 0x20, ~(u32)BIT(26));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_and(apmixed, 0x2c8, ~(u32)BIT(9));
	if (ret)
		return ret;

	ret = mt6797_md1_cold_write_or(peri, 0xc4, 0x7);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(sys, 0xc0, 0x82);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(l1, 0x190, BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_or(l1, 0x194, BIT(16));
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_value(l1, 0x198, 0x0000fde7);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_value(l1, 0x198, 0x1000fde7);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_value(l1, 0x180, 0x0001fde7);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_value(l1, 0x184, 0x00000000);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_write_value(l1, 0x184, 0x80000000);
	if (!ret)
		backend->pll_stage = MT6797_MD1_COLD_OWNER_PLL_PSMCU;
	return ret;
}

struct mt6797_md1_cold_pll_check {
	u32 offset;
	u32 mask;
};

static const struct mt6797_md1_cold_pll_check mt6797_md1_cold_pll_checks[] = {
	[MT6797_MD1_COLD_OWNER_PLL_PSMCU] = { 0x040, 0x00000001 },
	[MT6797_MD1_COLD_OWNER_PLL_L1MCU] = { 0x040, 0x00000002 },
	[MT6797_MD1_COLD_OWNER_PLL_DFE_IMC] = { 0x040, 0x00000090 },
	[MT6797_MD1_COLD_OWNER_PLL_L1SYS] = { 0x030, 0x80800000 },
	[MT6797_MD1_COLD_OWNER_PLL_FLEX1] = { 0x034, 0x80808080 },
	[MT6797_MD1_COLD_OWNER_PLL_FLEX2] = { 0x048, 0x00008080 },
	[MT6797_MD1_COLD_OWNER_PLL_DCM] = { 0x030, 0x00000080 },
};

static int mt6797_md1_cold_pll_advance(struct mt6797_md1_cold_owner_backend *backend)
{
	void __iomem *clksw = backend->base[MT6797_MD1_RES_CLKSW];
	int ret = 0;

	switch (backend->pll_stage) {
	case MT6797_MD1_COLD_OWNER_PLL_PSMCU:
		ret = mt6797_md1_cold_write_or(clksw, 0x24, BIT(1));
		break;
	case MT6797_MD1_COLD_OWNER_PLL_L1MCU:
		ret = mt6797_md1_cold_write_or(clksw, 0x24, BIT(8));
		break;
	case MT6797_MD1_COLD_OWNER_PLL_DFE_IMC:
		ret = mt6797_md1_cold_write_value(clksw, 0x2c, 0x30302020);
		if (!ret)
			ret = mt6797_md1_cold_write_value(clksw, 0x44, 0x00002030);
		break;
	case MT6797_MD1_COLD_OWNER_PLL_L1SYS:
		ret = mt6797_md1_cold_write_or(clksw, 0x24, 0x3fe00);
		if (!ret)
			ret = mt6797_md1_cold_write_value(clksw, 0x28, 0x30203031);
		if (!ret)
			ret = mt6797_md1_cold_write_value(
				backend->base[MT6797_MD1_RES_DCM], 0x1000, 0x67550000);
		break;
	case MT6797_MD1_COLD_OWNER_PLL_FLEX1:
		break;
	case MT6797_MD1_COLD_OWNER_PLL_FLEX2:
		ret = mt6797_md1_cold_write_or(backend->base[MT6797_MD1_RES_L1_A0],
					       0x188, BIT(7));
		break;
	case MT6797_MD1_COLD_OWNER_PLL_DCM:
		ret = mt6797_md1_cold_write_or(clksw, 0x24, BIT(0));
		break;
	default:
		return -EINVAL;
	}
	if (!ret)
		backend->pll_stage++;
	return ret;
}

static int mt6797_md1_cold_pll_poll(void *context,
	struct mt6797_md1_cold_owner_pll_status *status)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	const struct mt6797_md1_cold_pll_check *check;
	u32 value;
	int ret;

	if (!backend || !status ||
	    backend->pll_stage >= MT6797_MD1_COLD_OWNER_PLL_DONE)
		return -EINVAL;
	check = &mt6797_md1_cold_pll_checks[backend->pll_stage];
	value = readl(backend->base[MT6797_MD1_RES_CLKSW] + check->offset);
	*status = (struct mt6797_md1_cold_owner_pll_status) {
		.stage = backend->pll_stage,
		.offset = check->offset,
		.mask = check->mask,
		.last_value = value,
	};
	if ((value & check->mask) != check->mask) {
		udelay(1);
		return -EAGAIN;
	}
	ret = mt6797_md1_cold_pll_advance(backend);
	if (ret)
		return ret;
	return backend->pll_stage == MT6797_MD1_COLD_OWNER_PLL_DONE ? 0 : -EAGAIN;
}

static const struct mt6797_md1_cold_owner_ops mt6797_md1_cold_owner_ops = {
	.abi = MT6797_MD1_COLD_OWNER_ABI,
	.verify_cold_reset_held = mt6797_md1_cold_verify,
	.ccif_observe = mt6797_md1_cold_ccif_observe,
	.ccif_clear = mt6797_md1_cold_ccif_clear,
	.ccif_verify_cleared = mt6797_md1_cold_ccif_verify,
	.power_on = mt6797_md1_cold_power_on,
	.enable_srcclk = mt6797_md1_cold_enable_srcclk,
	.pll_start = mt6797_md1_cold_pll_start,
	.pll_poll = mt6797_md1_cold_pll_poll,
};

/*
 * The LK loader owns the live remap and permanently locks it.  This runner
 * never replaces it: a separately gated read-only observer must first attest
 * the exact plan that this transaction will send to the secure MPU provider.
 */
static int mt6797_md1_cold_candidate_plan_get(
	struct mt6797_md1_mpu_remap_plan *plan)
{
	struct mt6797_md1_remap_observation observation;
	int ret;

	ret = mt6797_md1_lk_handoff_plan_get(plan);
	if (ret)
		return ret == -ENODATA ? -EPROBE_DEFER : ret;
	ret = mt6797_md1_remap_observation_get(&observation);
	if (ret)
		return ret == -ENODATA ? -EPROBE_DEFER : ret;
	if (observation.classification || !observation.attestation.lk_lock_observed ||
	    !observation.attestation.secure_mpu_unobservable ||
	    observation.attestation.mode == MT6797_MD1_REMAP_ATTEST_NONE)
		return -EUCLEAN;

	return 0;
}

static int mt6797_md1_cold_apply_initial_mpu(struct device *dev,
	const struct mt6797_md1_mpu_remap_plan *plan)
{
	struct mt6797_md1_mpu_plan_entry entries[2] = {
		plan->mpu[0],
		plan->mpu[MT6797_MD1_MPU_SHARED_ENTRY],
	};

	return mt6797_md1_emi_mpu_apply(dev, plan, MT6797_MD1_EMI_MPU_INITIAL,
					entries, ARRAY_SIZE(entries));
}

static int mt6797_md1_cold_apply_stage1_mpu(struct device *dev,
	const struct mt6797_md1_mpu_remap_plan *plan)
{
	struct mt6797_md1_mpu_plan_entry entries[4] = {
		plan->mpu[1], plan->mpu[2], plan->mpu[3], plan->mpu[4],
	};

	/* Vendor ccci_set_mem_access_protection_1st_stage() temporary region 13. */
	entries[1].access_permission = 0x0016db44;
	return mt6797_md1_emi_mpu_apply(dev, plan, MT6797_MD1_EMI_MPU_STAGE1,
					entries, ARRAY_SIZE(entries));
}

static int mt6797_md1_cold_wdt_observe(void *context,
	struct mt6797_md1_wdt_status_observation *observation)
{
	struct mt6797_md1_cold_owner_backend *backend = context;

	if (!backend || !observation)
		return -EINVAL;
	observation->md_status = readl(backend->base[MT6797_MD1_RES_MD_RGU] +
		MT6797_MD1_WDT_MD_STATUS);
	observation->l1_status = readl(backend->base[MT6797_MD1_RES_L1_RGU] +
		MT6797_MD1_WDT_L1_STATUS);
	return 0;
}

static int mt6797_md1_cold_wdt_write_md_key(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;

	if (!backend)
		return -EINVAL;
	writel(MT6797_MD1_WDT_MD_MODE_KEY,
	       backend->base[MT6797_MD1_RES_MD_RGU] + MT6797_MD1_WDT_MD_MODE);
	return 0;
}

static int mt6797_md1_cold_wdt_write_l1_key(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;

	if (!backend)
		return -EINVAL;
	writel(MT6797_MD1_WDT_L1_MODE_KEY,
	       backend->base[MT6797_MD1_RES_L1_RGU] + MT6797_MD1_WDT_L1_MODE);
	return 0;
}

static const struct mt6797_md1_wdt_gate_ops mt6797_md1_cold_wdt_ops = {
	.abi = MT6797_MD1_WDT_GATE_ABI,
	.observe_status = mt6797_md1_cold_wdt_observe,
	.write_md_mode_key = mt6797_md1_cold_wdt_write_md_key,
	.write_l1_mode_key = mt6797_md1_cold_wdt_write_l1_key,
};

static int mt6797_md1_cold_wdt_run(struct mt6797_md1_cold_owner_backend *backend,
	struct mt6797_md1_cold_owner *owner)
{
	const struct mt6797_md1_wdt_gate_preflight preflight = {
		.cold_owner_pll_ready_wdt_unowned =
			mt6797_md1_cold_owner_state(owner) ==
			MT6797_MD1_COLD_OWNER_PLL_READY_WDT_UNOWNED,
	};
	int ret;

	ret = mt6797_md1_wdt_gate_init(&backend->wdt_gate,
					&mt6797_md1_cold_wdt_ops, backend);
	if (ret)
		return ret;
	ret = mt6797_md1_wdt_gate_run(&backend->wdt_gate, &preflight);
	if (ret)
		return ret;
	dev_info(backend->dev,
		 "MD1 WDT keys attempted; status %#x/%#x -> %#x/%#x (diagnostic only)\n",
		 backend->wdt_gate.observation.md_status,
		 backend->wdt_gate.observation.l1_status,
		 backend->wdt_gate.post_observation.md_status,
		 backend->wdt_gate.post_observation.l1_status);
	return 0;
}

static bool mt6797_md1_cold_ccif_activity(
	const struct mt6797_md1_cold_owner_ccif *ccif)
{
	return ccif->ap_busy || ccif->md_busy || ccif->ap_ack || ccif->md_ack ||
		ccif->ap_sram_nonzero_words || ccif->md_sram_nonzero_words;
}

static int mt6797_md1_cold_boot_vector_release(
	struct mt6797_md1_cold_owner_backend *backend,
	const struct mt6797_md1_boot_status_evidence *before)
{
	struct mt6797_md1_cold_owner_ccif ccif;
	void __iomem *boot_vector = backend->base[MT6797_MD1_RES_BOOT_VECTOR];
	void __iomem *boot_status = backend->base[MT6797_MD1_RES_BOOT_STATUS];
	u32 vector, stats0, stats1;
	unsigned int attempt;
	int ret;

	/* Vendor cldma_platform.c::md_cd_let_md_go(): MD_BOOT_VECTOR_EN <- 1. */
	writel(MT6797_MD1_BOOT_VECTOR_ENABLE, boot_vector);
	vector = readl(boot_vector);
	if (vector != MT6797_MD1_BOOT_VECTOR_ENABLE)
		return -EIO;

	for (attempt = 0; attempt < MT6797_MD1_MAX_BOOT_OBSERVE_POLLS; attempt++) {
		stats0 = readl(boot_status + MT6797_MD1_BOOT_STATUS0);
		stats1 = readl(boot_status + MT6797_MD1_BOOT_STATUS1);
		ret = mt6797_md1_cold_ccif_observe(backend, &ccif);
		if (ret)
			return ret;
		if (stats0 != before->stats0_final || stats1 != before->stats1_final ||
		    mt6797_md1_cold_ccif_activity(&ccif)) {
			dev_info(backend->dev,
				 "MD1 boot-vector readback=%#x after %u polls; boot=%#x/%#x CCIF busy=%#x/%#x ack=%#x/%#x sram=%u/%u\n",
				 vector, attempt + 1, stats0, stats1, ccif.ap_busy,
				 ccif.md_busy, ccif.ap_ack, ccif.md_ack,
				 ccif.ap_sram_nonzero_words, ccif.md_sram_nonzero_words);
			return 0;
		}
		udelay(MT6797_MD1_BOOT_OBSERVE_DELAY_US);
	}

	dev_crit(backend->dev,
		 "MD1 boot-vector readback=%#x but no boot-status or CCIF movement in %u bounded polls\n",
		 vector, MT6797_MD1_MAX_BOOT_OBSERVE_POLLS);
	return -ETIMEDOUT;
}

static int mt6797_md1_cold_owner_probe(struct platform_device *pdev)
{
	struct mt6797_md1_boot_status_evidence boot_status;
	struct mt6797_md1_boot_status_preflight preflight;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_cold_owner_backend *backend;
	struct mt6797_md1_cold_owner *owner;
	bool mpu_started = false;
	unsigned int i;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_cold_owner) ||
	    !READ_ONCE(enable_experimental_md1_boot_vector_release) ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,experimental-md1-cold-owner") ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,stage-cold-reset-held") ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,stage-md1-boot-vector-release"))
		return dev_err_probe(&pdev->dev, -EPERM,
			"MD1 boot candidate requires independent cold-owner and release gates\n");
	if (atomic_read(&mt6797_md1_cold_owner_fatal_latch))
		return dev_err_probe(&pdev->dev, -EUCLEAN,
			"MD1 candidate was previously attempted; reset/reboot required\n");
	if (!mt6797_md1_lk_handoff_image_loaded())
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
			"waiting for verified LK image-load evidence\n");
	ret = mt6797_md1_cold_candidate_plan_get(&plan);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"waiting for attested LK remap plan/locked snapshot\n");
	ret = mt6797_md1_boot_status_evidence_get(&boot_status);
	if (ret)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
			"waiting for three-read MD1 boot-status evidence\n");

	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	owner = devm_kzalloc(&pdev->dev, sizeof(*owner), GFP_KERNEL);
	if (!backend || !owner)
		return -ENOMEM;
	backend->dev = &pdev->dev;
	for (i = 0; i < MT6797_MD1_RES_COUNT; i++) {
		ret = mt6797_md1_cold_map_resource(pdev, i, &backend->base[i]);
		if (ret)
			return ret;
	}
	backend->pmic = mt6797_md1_cold_pmic_regmap(&pdev->dev);
	if (IS_ERR(backend->pmic))
		return dev_err_probe(&pdev->dev, PTR_ERR(backend->pmic),
			"MT6351 PMIC regmap unavailable\n");
	backend->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
					       "mediatek,spm");
	if (IS_ERR(backend->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(backend->spm),
			"SPM syscon regmap unavailable\n");
	ret = mt6797_md1_cold_owner_init(owner, &mt6797_md1_cold_owner_ops, backend);
	if (ret)
		return ret;
	preflight = (struct mt6797_md1_boot_status_preflight) {
		.lk_image_loaded = true,
		.boot_status_observed = boot_status.observed,
		.stats0_samples = boot_status.stats0_samples,
		.stats1_samples = boot_status.stats1_samples,
		.stats0_final = boot_status.stats0_final,
		.stats1_final = boot_status.stats1_final,
	};

	/* Vendor CCCI: final LK remap, initial MPU, then CCIF/power sequence. */
	ret = mt6797_md1_cold_apply_initial_mpu(&pdev->dev, &plan);
	if (ret)
		goto out_fatal;
	mpu_started = true;
	ret = mt6797_md1_cold_owner_run(owner, &preflight);
	if (ret)
		goto out_fatal;
	/* Vendor md_cd_power_on() writes the two WDT keys before stage-one MPU. */
	ret = mt6797_md1_cold_wdt_run(backend, owner);
	if (ret)
		goto out_fatal;
	ret = mt6797_md1_cold_apply_stage1_mpu(&pdev->dev, &plan);
	if (ret)
		goto out_fatal;
	/* The one vendor boot-vector write is followed only by bounded observation. */
	ret = mt6797_md1_cold_boot_vector_release(backend, &boot_status);
	if (ret)
		goto out_fatal;

	platform_set_drvdata(pdev, owner);
	dev_info(&pdev->dev,
		 "MD1 boot candidate observed post-vector activity; no IRQ, CLDMA, shared-memory or CCCI transport is installed\n");
	return 0;

out_fatal:
	if (mpu_started || mt6797_md1_emi_mpu_is_fatal()) {
		mt6797_md1_emi_mpu_latch_fatal();
		atomic_set(&mt6797_md1_cold_owner_fatal_latch, 1);
		dev_crit(&pdev->dev,
			 "MD1 boot candidate failed %d; reset/reboot required before any retry\n", ret);
	} else {
		dev_err(&pdev->dev,
			"MD1 boot candidate preflight failed %d; no transaction was started\n", ret);
	}
	return ret;
}

static const struct of_device_id mt6797_md1_cold_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-cold-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_cold_owner_of_match);

static struct platform_driver mt6797_md1_cold_owner_driver = {
	.probe = mt6797_md1_cold_owner_probe,
	.driver = {
		.name = "mt6797-md1-cold-owner",
		.of_match_table = mt6797_md1_cold_owner_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_cold_owner_driver);

MODULE_DESCRIPTION("Default-disabled MT6797 MD1 one-shot cold power/PLL/CCIF owner");
MODULE_LICENSE("GPL");
