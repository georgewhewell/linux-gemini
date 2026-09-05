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
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/mediatek-wdt.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-md1-boot-status-observe.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-md1-firmware-loader.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>
#include <linux/soc/mediatek/mt6797-md1-wdt-gate.h>
#include <linux/workqueue.h>
#include <linux/wait.h>

#define MT6797_MD1_INFRA_BASE			0x10001000
#define MT6797_MD1_INFRA_SIZE			0x1000
#define MT6797_MD1_AP_CCIF_BASE		0x10209000
#define MT6797_MD1_MD_CCIF_BASE		0x1020a000
#define MT6797_MD1_CCIF_SIZE			0x1000
#define MT6797_MD1_AP_CLDMA_AO_BASE		0x10014000
#define MT6797_MD1_AP_CLDMA_PDN_BASE		0x10219000
#define MT6797_MD1_CLDMA_SIZE			0x1e00
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
/*
 * Vendor accesses offsets 0x10 and 0x14 from this base; its cldma_platform.h
 * MD_Clkctrl dump (DUMP_ADDR03/LEN03) spans 0x00..0x9b, so the window covers
 * the full dump region with one word of margin.
 */
#define MT6797_MD1_TOP_PLL_SIZE		0xa0
#define MT6797_MD1_TOP_PLL_DUMP_WORDS	40
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
/* Vendor clk-mt6797-pg.c SPM project code keying every mtcmos sequence. */
#define MT6797_MD1_SPM_PROJECT_CODE		0x0b16
#define MT6797_MD1_SPM_POWERON_CONFIG_EN_KEY	\
	((MT6797_MD1_SPM_PROJECT_CODE << 16) | \
	 MT6797_MD1_SPM_POWERON_CONFIG_EN_BIT)
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
#define MT6797_MD1_CCIF_RCHNUM			0x010
#define MT6797_MD1_CCIF_ACK			0x014
#define MT6797_MD1_CCIF_CHDATA			0x100
#define MT6797_MD1_CCIF_CHANNELS		16
#define MT6797_MD1_CCIF_SRAM_BYTES		512

/* Vendor eccci/mt6797/cldma_reg.h AP-side read-only status offsets. */
#define MT6797_MD1_CLDMA_UL_STATUS		0x0050
#define MT6797_MD1_CLDMA_SO_STATUS		0x0ab8
#define MT6797_MD1_CLDMA_CODA_VERSION		0x0400
#define MT6797_MD1_CLDMA_L2TISAR0		0x0410
#define MT6797_MD1_CLDMA_L2RISAR0		0x0450
#define MT6797_MD1_CLDMA_L2RIMR0		0x0c58
#define MT6797_MD1_CLDMA_IP_BUSY		0x04b4

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
/*
 * Vendor cldma_platform.c spins unbounded per pll stage.  Bound each stage:
 * on expiry, dump register evidence once and refuse further release.
 */
#define MT6797_MD1_PLL_STAGE_MAX_POLLS		1000
#define MT6797_MD1_MAX_BOOT_OBSERVE_POLLS	1000
#define MT6797_MD1_BOOT_OBSERVE_DELAY_US	100
/* Five seconds at nominal cadence; scheduling stretch is recorded in ns. */
#define MT6797_MD1_PASSIVE_OBSERVE_POLLS	5000
#define MT6797_MD1_PASSIVE_OBSERVE_DELAY_US	1000

struct mt6797_md1_cold_owner_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

enum mt6797_md1_cold_owner_resource_id {
	MT6797_MD1_RES_INFRA,
	MT6797_MD1_RES_AP_CCIF,
	MT6797_MD1_RES_MD_CCIF,
	MT6797_MD1_RES_AP_CLDMA_AO,
	MT6797_MD1_RES_AP_CLDMA_PDN,
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
	[MT6797_MD1_RES_AP_CLDMA_AO] = { "ap-cldma-ao",
		MT6797_MD1_AP_CLDMA_AO_BASE, MT6797_MD1_CLDMA_SIZE },
	[MT6797_MD1_RES_AP_CLDMA_PDN] = { "ap-cldma-pdn",
		MT6797_MD1_AP_CLDMA_PDN_BASE, MT6797_MD1_CLDMA_SIZE },
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
	struct reset_control_bulk_data cldma_resets[2];
	bool cldma_reset_completed;
	void __iomem *base[MT6797_MD1_RES_COUNT];
	struct regmap *pmic;
	struct regmap *spm;
	struct mt6797_md1_pmic_snapshot pmic_snapshot;
	struct mt6797_md1_clkbuf_snapshot clkbuf_snapshot;
	enum mt6797_md1_cold_owner_pll_stage pll_stage;
	unsigned int pll_stage_polls;
	struct mt6797_md1_wdt_gate wdt_gate;
	struct work_struct passive_observe_work;
	struct mt6797_md1_post_vector_evidence passive_evidence;
};

static bool enable_experimental_md1_cold_owner;
static bool enable_experimental_md1_boot_vector_release;
static bool enable_experimental_md1_firmware_load;
/* A failed post-observation transaction may have changed MD1 state. */
static atomic_t mt6797_md1_cold_owner_fatal_latch = ATOMIC_INIT(0);
static DEFINE_MUTEX(mt6797_md1_post_vector_evidence_lock);
static struct mt6797_md1_post_vector_evidence mt6797_md1_post_vector_evidence;
static DECLARE_WAIT_QUEUE_HEAD(mt6797_md1_handoff_waitq);
/* One-shot boot ownership: terminal shutdown must never be republished ready. */
static int mt6797_md1_handoff_status = -EPROBE_DEFER;
static struct device *mt6797_md1_handoff_supplier;
static DEFINE_MUTEX(mt6797_md1_stage2_lock);
static struct device *mt6797_md1_stage2_dev;
static struct mt6797_md1_mpu_remap_plan mt6797_md1_stage2_plan;
static bool mt6797_md1_stage2_armed;
static bool mt6797_md1_stage2_complete;
module_param_named(enable_experimental_md1_cold_owner,
			   enable_experimental_md1_cold_owner, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_cold_owner,
	"run the one-shot, source-backed MT6797 MD1 cold owner (default: false)");
module_param_named(enable_experimental_md1_boot_vector_release,
			enable_experimental_md1_boot_vector_release, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_boot_vector_release,
	"allow the separately gated MT6797 MD1 WDT/boot-vector candidate (default: false)");
module_param_named(enable_experimental_md1_firmware_load,
			enable_experimental_md1_firmware_load, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_firmware_load,
	"allow validated request_firmware images to replace the LK-loaded MD1 image");

int mt6797_md1_cold_owner_post_vector_evidence_get(
	struct mt6797_md1_post_vector_evidence *evidence)
{
	if (!evidence)
		return -EINVAL;

	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	*evidence = mt6797_md1_post_vector_evidence;
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	return evidence->observed ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cold_owner_post_vector_evidence_get);

int mt6797_md1_cold_owner_wait_handoff(struct device *consumer,
				       struct mt6797_md1_post_vector_evidence *evidence)
{
	struct device_link *link;
	int ret;

	if (!consumer || !evidence)
		return -EINVAL;
	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	ret = mt6797_md1_handoff_status;
	if (ret != -EAGAIN && ret)
		goto unlock;
	/* Keep the supplier bound until this consumer has been torn down. */
	link = device_link_add(consumer, mt6797_md1_handoff_supplier,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto unlock;
	}
	/*
	 * device_links_busy() sets DL_DEV_UNBINDING under the same core lock
	 * used by device_link_add(). A late link is SUPPLIER_UNBIND, not a
	 * lease. Conversely CONSUMER_PROBE makes supplier unbind wait for
	 * this probe. Do not infer presence from our devres cleanup alone.
	 */
	if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);

	if (!wait_event_timeout(mt6797_md1_handoff_waitq,
				READ_ONCE(mt6797_md1_handoff_status) != -EAGAIN,
				msecs_to_jiffies(10000)))
		return -ETIMEDOUT;

	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	ret = mt6797_md1_handoff_status;
	if (!ret)
		*evidence = mt6797_md1_post_vector_evidence;
unlock:
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cold_owner_wait_handoff);

static void mt6797_md1_handoff_finish(int error)
{
	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	if (mt6797_md1_handoff_status != -ESHUTDOWN)
		WRITE_ONCE(mt6797_md1_handoff_status, error);
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	wake_up_all(&mt6797_md1_handoff_waitq);
}

void mt6797_md1_cold_owner_transport_fatal(int reason)
{
	if (reason < 0)
		atomic_set(&mt6797_md1_cold_owner_fatal_latch, 1);
}
EXPORT_SYMBOL_GPL(mt6797_md1_cold_owner_transport_fatal);

int mt6797_md1_cold_owner_hs1_stage2(void)
{
	struct mt6797_md1_mpu_plan_entry entry;
	int ret;

	mutex_lock(&mt6797_md1_stage2_lock);
	if (atomic_read(&mt6797_md1_cold_owner_fatal_latch)) {
		ret = -EUCLEAN;
		goto out;
	}
	if (!mt6797_md1_stage2_armed || !mt6797_md1_stage2_dev) {
		ret = -ENODATA;
		goto out;
	}
	if (mt6797_md1_stage2_complete) {
		ret = -EALREADY;
		goto out;
	}
	entry = mt6797_md1_stage2_plan.mpu[2];
	ret = mt6797_md1_emi_mpu_apply(mt6797_md1_stage2_dev,
		&mt6797_md1_stage2_plan, MT6797_MD1_EMI_MPU_STAGE2, &entry, 1);
	if (ret) {
		mt6797_md1_emi_mpu_latch_fatal();
		atomic_set(&mt6797_md1_cold_owner_fatal_latch, 1);
	} else {
		mt6797_md1_stage2_complete = true;
	}
out:
	mutex_unlock(&mt6797_md1_stage2_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cold_owner_hs1_stage2);

static void mt6797_md1_cold_post_vector_publish(
	const struct mt6797_md1_post_vector_evidence *evidence)
{
	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	if (mt6797_md1_handoff_status != -ESHUTDOWN) {
		mt6797_md1_post_vector_evidence = *evidence;
		if (evidence->passive_complete)
			WRITE_ONCE(mt6797_md1_handoff_status, 0);
	}
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	wake_up_all(&mt6797_md1_handoff_waitq);
}

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
	dev_warn(backend->dev,
		 "MD1 power %s wait timed out after %u polls: pwr_status=%#x pwr_status_2nd=%#x\n",
		 on ? "on" : "off", MT6797_MD1_MAX_POWER_POLLS, status,
		 status_2nd);
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
	dev_warn(backend->dev,
		 "MD1 bus protect %s wait timed out after %u polls: protect_sta1=%#x mask=%#x\n",
		 asserted ? "assert" : "release", MT6797_MD1_MAX_POWER_POLLS,
		 value, (u32)MT6797_MD1_INFRA_MD1_PROT_MASK);
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

static int mt6797_md1_cold_ccif_registers_observe(void *context,
	struct mt6797_md1_cold_owner_ccif *state)
{
	struct mt6797_md1_cold_owner_backend *backend = context;

	if (!backend || !state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));
	state->ap_busy = readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_BUSY);
	state->md_busy = readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_BUSY);
	state->ap_rchnum = readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_RCHNUM);
	state->md_rchnum = readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_RCHNUM);
	state->ap_ack = readl(backend->base[MT6797_MD1_RES_AP_CCIF] + MT6797_MD1_CCIF_ACK);
	state->md_ack = readl(backend->base[MT6797_MD1_RES_MD_CCIF] + MT6797_MD1_CCIF_ACK);
	return 0;
}

static int mt6797_md1_cold_cldma_registers_observe(void *context,
			struct mt6797_md1_cold_owner_cldma *state)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	void __iomem *ao, *pdn;

	if (!backend || !state)
		return -EINVAL;
	ao = backend->base[MT6797_MD1_RES_AP_CLDMA_AO];
	pdn = backend->base[MT6797_MD1_RES_AP_CLDMA_PDN];
	memset(state, 0, sizeof(*state));
	state->coda_version = readl(pdn + MT6797_MD1_CLDMA_CODA_VERSION);
	state->ul_status = readl(pdn + MT6797_MD1_CLDMA_UL_STATUS);
	state->so_status = readl(ao + MT6797_MD1_CLDMA_SO_STATUS);
	state->l2_tx_status = readl(pdn + MT6797_MD1_CLDMA_L2TISAR0);
	state->l2_rx_status = readl(pdn + MT6797_MD1_CLDMA_L2RISAR0);
	state->irq_mask = readl(ao + MT6797_MD1_CLDMA_L2RIMR0);
	state->ip_busy = readl(pdn + MT6797_MD1_CLDMA_IP_BUSY);
	return 0;
}

static int mt6797_md1_cold_ccif_observe(void *context,
	struct mt6797_md1_cold_owner_ccif *state)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	u32 word;
	int ret;

	ret = mt6797_md1_cold_ccif_registers_observe(context, state);
	if (ret)
		return ret;
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

static int mt6797_md1_cold_cldma_reset(struct mt6797_md1_cold_owner_backend *backend)
{
	struct mt6797_md1_cold_owner_cldma state;
	int ret;
	unsigned int i;

	backend->cldma_reset_completed = false;
	ret = mtk_wdt_require_boot_recovery(backend->dev);
	if (ret)
		return ret;
	/* Vendor md_cldma_hw_reset: AO pulse, then PD pulse, before power_on. */
	for (i = 0; i < ARRAY_SIZE(backend->cldma_resets); i++) {
		ret = reset_control_assert(backend->cldma_resets[i].rstc);
		if (ret)
			return ret;
		ret = reset_control_deassert(backend->cldma_resets[i].rstc);
		if (ret)
			return ret;
	}
	/* MT6797 vendor ENABLE_CLDMA_AP_SIDE uses the always-on AP engine. */
	ret = mt6797_md1_cold_cldma_registers_observe(backend, &state);
	if (ret)
		return ret;
	dev_info(backend->dev,
		 "AP CLDMA reset snapshot: coda=%#x ul=%#x so=%#x txirq=%#x rxirq=%#x busy=%#x\n",
		 state.coda_version, state.ul_status, state.so_status,
		 state.l2_tx_status, state.l2_rx_status, state.ip_busy);
	if (state.ul_status || state.so_status || state.l2_tx_status ||
	    state.l2_rx_status || state.ip_busy)
		return -EUCLEAN;
	backend->cldma_reset_completed = true;
	return 0;
}

static int mt6797_md1_cold_power_on(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	unsigned int pwr_con, poweron_config, status, status_2nd;
	u32 value;
	int ret;

	ret = mt6797_md1_cold_cldma_reset(backend);
	if (ret)
		return ret;
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

	/*
	 * Vendor spm_mtcmos_ctrl_md1() keys every mtcmos power sequence with
	 * the SPM project code; without it the MD1_PWR_CON power requests are
	 * inert and PWR_STATUS never sets.  Mirror vendor: program the key
	 * here and leave it asserted, as mainline mtk-scpsys does for other
	 * domains.
	 */
	ret = regmap_write(backend->spm, MT6797_MD1_SPM_POWERON_CONFIG_EN,
			   MT6797_MD1_SPM_POWERON_CONFIG_EN_KEY);
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

	/* Permanent evidence channel: request readbacks before the wait. */
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_POWERON_CONFIG_EN,
			  &poweron_config);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_MD1_PWR_CON, &pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS, &status);
	if (ret)
		return ret;
	ret = regmap_read(backend->spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
			  &status_2nd);
	if (ret)
		return ret;
	dev_info(backend->dev,
		 "MD1 power-on requested: poweron_config=%#x pwr_con=%#x pwr_status=%#x pwr_status_2nd=%#x\n",
		 poweron_config, pwr_con, status, status_2nd);
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
		/* Vendor md1_pll_init selects these sources before DFE/IMC wait. */
		if (!ret)
			ret = mt6797_md1_cold_write_value(clksw, 0x2c, 0x30302020);
		if (!ret)
			ret = mt6797_md1_cold_write_value(clksw, 0x44, 0x00002030);
		break;
	case MT6797_MD1_COLD_OWNER_PLL_DFE_IMC:
	case MT6797_MD1_COLD_OWNER_PLL_L1SYS:
	case MT6797_MD1_COLD_OWNER_PLL_FLEX1:
		break;
	case MT6797_MD1_COLD_OWNER_PLL_FLEX2:
		/* All three FLEX status polls precede the L1SYS switch in vendor. */
		ret = mt6797_md1_cold_write_or(clksw, 0x24, 0x3fe00);
		if (!ret)
			ret = mt6797_md1_cold_write_value(clksw, 0x28, 0x30203031);
		if (!ret)
			ret = mt6797_md1_cold_write_value(
				backend->base[MT6797_MD1_RES_DCM], 0x1000, 0x67550000);
		if (!ret)
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

static void mt6797_md1_cold_pll_timeout_dump(void *context);

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
		if (++backend->pll_stage_polls < MT6797_MD1_PLL_STAGE_MAX_POLLS) {
			udelay(1);
			return -EAGAIN;
		}
		backend->pll_stage_polls = 0;
		dev_warn(backend->dev,
			 "MD1 pll stage %d not ready after %u polls: offset=%#x mask=%#x last_value=%#x; refusing release\n",
			 backend->pll_stage, MT6797_MD1_PLL_STAGE_MAX_POLLS,
			 check->offset, check->mask, value);
		mt6797_md1_cold_pll_timeout_dump(backend);
		return -ETIMEDOUT;
	} else {
		backend->pll_stage_polls = 0;
	}
	ret = mt6797_md1_cold_pll_advance(backend);
	if (ret)
		return ret;
	return backend->pll_stage == MT6797_MD1_COLD_OWNER_PLL_DONE ? 0 : -EAGAIN;
}

/*
 * Read-only register evidence for a frozen pll ready bit, invoked after a
 * stage's poll budget expires (per-stage in pll_poll, or by the core when
 * the shared budget expires).  Mirrors the vendor MD_Clkctrl
 * DUMP_ADDR03/LEN03 region over md-top-pll; no writes.
 */
static void mt6797_md1_cold_pll_timeout_dump(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	void __iomem *pll = backend->base[MT6797_MD1_RES_TOP_PLL];
	void __iomem *clksw = backend->base[MT6797_MD1_RES_CLKSW];
	void __iomem *apmixed = backend->base[MT6797_MD1_RES_APMIXED];
	void __iomem *infra = backend->base[MT6797_MD1_RES_INFRA];
	unsigned int i;

	for (i = 0; i < MT6797_MD1_TOP_PLL_DUMP_WORDS; i += 8)
		dev_warn(backend->dev,
			 "MD1 pll dump pllmixed[%#04x..%#04x]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 i * 4, i * 4 + 0x1c,
			 readl(pll + i * 4 + 0x00), readl(pll + i * 4 + 0x04),
			 readl(pll + i * 4 + 0x08), readl(pll + i * 4 + 0x0c),
			 readl(pll + i * 4 + 0x10), readl(pll + i * 4 + 0x14),
			 readl(pll + i * 4 + 0x18), readl(pll + i * 4 + 0x1c));
	dev_warn(backend->dev,
		 "MD1 pll dump clksw: 0x20=%08x 0x24=%08x 0x2c=%08x 0x40=%08x 0x44=%08x\n",
		 readl(clksw + 0x20), readl(clksw + 0x24), readl(clksw + 0x2c),
		 readl(clksw + 0x40), readl(clksw + 0x44));
	dev_warn(backend->dev,
		 "MD1 pll dump apmixed: 0x000=%08x 0x2c8=%08x srcclkena[0x10001f0c]=%08x\n",
		 readl(apmixed + 0x000), readl(apmixed + 0x2c8),
		 readl(infra + MT6797_MD1_INFRA_SRC_CLKENA));
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
	.pll_timeout_dump = mt6797_md1_cold_pll_timeout_dump,
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

static int mt6797_md1_cold_firmware_names_get(struct device *dev,
				 struct mt6797_md1_firmware_names *names)
{
	const char *firmware[3] = {};
	int count, ret;

	count = of_property_count_strings(dev->of_node, "firmware-name");
	if (count < 1 || count > ARRAY_SIZE(firmware))
		return -EINVAL;
	ret = device_property_read_string_array(dev, "firmware-name", firmware,
						count);
	if (ret != count)
		return ret < 0 ? ret : -EINVAL;
	names->md = firmware[0];
	if (count > 1)
		names->dsp = firmware[1];
	if (count > 2)
		names->arm7 = firmware[2];
	return 0;
}

static int mt6797_md1_cold_firmware_verify(void *context)
{
	struct mt6797_md1_cold_owner_backend *backend = context;
	struct mt6797_md1_cold_state state;
	int ret;

	ret = mtk_wdt_require_boot_recovery(backend->dev);
	if (ret)
		return ret;
	return mt6797_md1_cold_verify(context, &state);
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

static bool mt6797_md1_cold_ccif_registers_changed(
	const struct mt6797_md1_cold_owner_ccif *before,
	const struct mt6797_md1_cold_owner_ccif *after)
{
	return before->ap_busy != after->ap_busy ||
		before->md_busy != after->md_busy ||
		before->ap_rchnum != after->ap_rchnum ||
		before->md_rchnum != after->md_rchnum ||
		before->ap_ack != after->ap_ack ||
		before->md_ack != after->md_ack;
}

static bool
mt6797_md1_cold_cldma_registers_changed(
	const struct mt6797_md1_cold_owner_cldma *before,
	const struct mt6797_md1_cold_owner_cldma *after)
{
	return memcmp(before, after, sizeof(*before));
}

/*
 * Deliberately independent of the immediate release oracle.  This worker
 * observes a much longer window without holding up boot and without writing
 * CCIF ACK/TCHNUM, enabling an IRQ, touching MD shared memory, or preparing
 * CLDMA descriptors.  RCHNUM and CLDMA status reads are passive; vendor code
 * separately acknowledges the status when it actually owns the interrupt
 * path.
 */
static void mt6797_md1_cold_passive_observe_work(struct work_struct *work)
{
	struct mt6797_md1_cold_owner_backend *backend = container_of(work,
		struct mt6797_md1_cold_owner_backend, passive_observe_work);
	struct mt6797_md1_post_vector_evidence evidence =
		backend->passive_evidence;
	struct mt6797_md1_cold_owner_ccif previous_ccif, ccif;
	struct mt6797_md1_cold_owner_cldma previous_cldma, cldma;
	u32 previous_stats0, previous_stats1, stats0, stats1;
	unsigned int attempt;
	int ret;

	previous_stats0 = readl(backend->base[MT6797_MD1_RES_BOOT_STATUS] +
		MT6797_MD1_BOOT_STATUS0);
	previous_stats1 = readl(backend->base[MT6797_MD1_RES_BOOT_STATUS] +
		MT6797_MD1_BOOT_STATUS1);
	ret = mt6797_md1_cold_ccif_registers_observe(backend, &previous_ccif);
	if (ret) {
		mt6797_md1_handoff_finish(ret);
		return;
	}
	ret = mt6797_md1_cold_cldma_registers_observe(backend, &previous_cldma);
	if (ret) {
		mt6797_md1_handoff_finish(ret);
		return;
	}
	evidence.passive_started_boottime_ns = ktime_get_boottime_ns();

	for (attempt = 0; attempt < MT6797_MD1_PASSIVE_OBSERVE_POLLS; attempt++) {
		if (READ_ONCE(mt6797_md1_handoff_status) == -ESHUTDOWN) {
			ret = -ESHUTDOWN;
			break;
		}
		usleep_range(MT6797_MD1_PASSIVE_OBSERVE_DELAY_US,
			     MT6797_MD1_PASSIVE_OBSERVE_DELAY_US + 250);
		stats0 = readl(backend->base[MT6797_MD1_RES_BOOT_STATUS] +
			MT6797_MD1_BOOT_STATUS0);
		stats1 = readl(backend->base[MT6797_MD1_RES_BOOT_STATUS] +
			MT6797_MD1_BOOT_STATUS1);
		ret = mt6797_md1_cold_ccif_registers_observe(backend, &ccif);
		if (ret)
			break;
		ret = mt6797_md1_cold_cldma_registers_observe(backend, &cldma);
		if (ret)
			break;

		if (stats0 != previous_stats0)
			evidence.boot_status0_transitions++;
		if (stats1 != previous_stats1)
			evidence.boot_status1_transitions++;
		if (mt6797_md1_cold_ccif_registers_changed(&previous_ccif, &ccif))
			evidence.ccif_register_transitions++;
		if (mt6797_md1_cold_owner_ccif_register_activity(&ccif)) {
			evidence.ccif_register_activity = true;
			if (!evidence.first_ccif_activity_poll)
				evidence.first_ccif_activity_poll = attempt + 1;
		}
		if (mt6797_md1_cold_cldma_registers_changed(&previous_cldma, &cldma))
			evidence.cldma_register_transitions++;
		if (mt6797_md1_cold_owner_cldma_register_activity(&cldma)) {
			evidence.cldma_register_activity = true;
			if (!evidence.first_cldma_activity_poll)
				evidence.first_cldma_activity_poll = attempt + 1;
		}

		previous_stats0 = stats0;
		previous_stats1 = stats1;
		previous_ccif = ccif;
		previous_cldma = cldma;
		evidence.passive_polls = attempt + 1;
	}

	evidence.boot_status0_after = previous_stats0;
	evidence.boot_status1_after = previous_stats1;
	evidence.ccif_after = previous_ccif;
	evidence.cldma_after = previous_cldma;
	evidence.passive_observed_boottime_ns = ktime_get_boottime_ns();
	evidence.passive_complete = !ret &&
		evidence.passive_polls == MT6797_MD1_PASSIVE_OBSERVE_POLLS;
	backend->passive_evidence = evidence;
	mt6797_md1_cold_post_vector_publish(&evidence);
	if (ret)
		mt6797_md1_handoff_finish(ret);

	dev_info(backend->dev,
			 "MD1 passive post-vector observation: complete=%u polls=%u t=%llu..%llu stats=%#08x/%#08x transitions=%u/%u CCIF busy=%#x/%#x rchnum=%#x/%#x ack=%#x/%#x transitions=%u first_activity_poll=%u; CLDMA coda=%#x ul=%#x so=%#x l2=%#x/%#x mask=%#x busy=%#x transitions=%u first_activity_poll=%u; register reads only, no writes, IRQ, descriptors, HWO or MD shared-memory access\n",
		 evidence.passive_complete, evidence.passive_polls,
		 (unsigned long long)evidence.passive_started_boottime_ns,
		 (unsigned long long)evidence.passive_observed_boottime_ns,
		 evidence.boot_status0_after, evidence.boot_status1_after,
		 evidence.boot_status0_transitions,
		 evidence.boot_status1_transitions,
		 evidence.ccif_after.ap_busy, evidence.ccif_after.md_busy,
		 evidence.ccif_after.ap_rchnum, evidence.ccif_after.md_rchnum,
		 evidence.ccif_after.ap_ack, evidence.ccif_after.md_ack,
			 evidence.ccif_register_transitions,
			 evidence.first_ccif_activity_poll,
			 evidence.cldma_after.coda_version,
			 evidence.cldma_after.ul_status,
			 evidence.cldma_after.so_status,
			 evidence.cldma_after.l2_tx_status,
			 evidence.cldma_after.l2_rx_status,
			 evidence.cldma_after.irq_mask,
			 evidence.cldma_after.ip_busy,
			 evidence.cldma_register_transitions,
			 evidence.first_cldma_activity_poll);
}

static void mt6797_md1_cold_cancel_passive_observer(void *data)
{
	struct mt6797_md1_cold_owner_backend *backend = data;
	bool published;

	/* Wake blocked probes before draining MMIO readers or releasing devres. */
	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	published = mt6797_md1_handoff_supplier == backend->dev;
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	if (!published) {
		cancel_work_sync(&backend->passive_observe_work);
		return;
	}
	mt6797_md1_handoff_finish(-ESHUTDOWN);
	atomic_set(&mt6797_md1_cold_owner_fatal_latch, 1);
	mutex_lock(&mt6797_md1_stage2_lock);
	mt6797_md1_stage2_armed = false;
	mt6797_md1_stage2_dev = NULL;
	mutex_unlock(&mt6797_md1_stage2_lock);
	cancel_work_sync(&backend->passive_observe_work);
}

static int mt6797_md1_cold_boot_vector_release(
	struct mt6797_md1_cold_owner_backend *backend,
	const struct mt6797_md1_boot_status_evidence *before)
{
	struct mt6797_md1_post_vector_evidence evidence = { };
	void __iomem *boot_vector = backend->base[MT6797_MD1_RES_BOOT_VECTOR];
	void __iomem *boot_status = backend->base[MT6797_MD1_RES_BOOT_STATUS];
	u32 vector, stats0, stats1;
	unsigned int attempt;
	int ret;

	evidence.boot_status0_before = readl(boot_status + MT6797_MD1_BOOT_STATUS0);
	evidence.boot_status1_before = readl(boot_status + MT6797_MD1_BOOT_STATUS1);
	ret = mt6797_md1_cold_ccif_observe(backend, &evidence.ccif_before);
	if (ret)
		return ret;
	evidence.baseline_boottime_ns = ktime_get_boottime_ns();
	if (evidence.boot_status0_before != before->stats0_final ||
	    evidence.boot_status1_before != before->stats1_final)
		dev_info(backend->dev,
			 "MD1 post-vector baseline: boot-status %#08x/%#08x -> %#08x/%#08x\n",
			 before->stats0_final, before->stats1_final,
			 evidence.boot_status0_before, evidence.boot_status1_before);

	/* Vendor cldma_platform.c::md_cd_let_md_go(): MD_BOOT_VECTOR_EN <- 1. */
	writel(MT6797_MD1_BOOT_VECTOR_ENABLE, boot_vector);
	vector = readl(boot_vector);
	evidence.boot_vector_readback = vector;
	if (vector != MT6797_MD1_BOOT_VECTOR_ENABLE) {
		evidence.boot_status0_after = evidence.boot_status0_before;
		evidence.boot_status1_after = evidence.boot_status1_before;
		evidence.ccif_after = evidence.ccif_before;
		evidence.observed_boottime_ns = ktime_get_boottime_ns();
		evidence.token = MT6797_MD1_POST_VECTOR_NO_GO;
		evidence.observed = true;
		mt6797_md1_cold_post_vector_publish(&evidence);
		dev_crit(backend->dev,
			 "MD1 post-vector oracle NO-GO: reg[0x%08x]=%#x expected=%#x; no IRQ, CLDMA or shared-memory access\n",
			 MT6797_MD1_BOOT_VECTOR_BASE, vector,
			 MT6797_MD1_BOOT_VECTOR_ENABLE);
		return -EIO;
	}

	for (attempt = 0; attempt < MT6797_MD1_MAX_BOOT_OBSERVE_POLLS; attempt++) {
		stats0 = readl(boot_status + MT6797_MD1_BOOT_STATUS0);
		stats1 = readl(boot_status + MT6797_MD1_BOOT_STATUS1);
		ret = mt6797_md1_cold_ccif_observe(backend, &evidence.ccif_after);
		if (ret)
			return ret;

		evidence.boot_status0_after = stats0;
		evidence.boot_status1_after = stats1;
		evidence.observed_boottime_ns = ktime_get_boottime_ns();
		evidence.polls = attempt + 1;
		evidence.boot_status_changed =
			stats0 != evidence.boot_status0_before ||
			stats1 != evidence.boot_status1_before;
		evidence.ccif_changed =
			mt6797_md1_cold_owner_post_vector_activity(
				&(struct mt6797_md1_post_vector_evidence) {
					.ccif_before = evidence.ccif_before,
					.ccif_after = evidence.ccif_after,
				});

		if (mt6797_md1_cold_owner_post_vector_activity(&evidence)) {
			evidence.token = MT6797_MD1_POST_VECTOR_GO;
			evidence.observed = true;
			mt6797_md1_cold_post_vector_publish(&evidence);
			dev_info(backend->dev,
				 "MD1 post-vector oracle GO: vector[0x%08x]=%#x polls=%u t=%llu..%llu stats[0x10201300]=%#08x->%#08x stats[0x10201304]=%#08x->%#08x CCIF busy=%#x/%#x->%#x/%#x ack=%#x/%#x->%#x/%#x sram=%u/%u->%u/%u; no IRQ, CLDMA or shared-memory access\n",
				 MT6797_MD1_BOOT_VECTOR_BASE, vector, evidence.polls,
				 (unsigned long long)evidence.baseline_boottime_ns,
				 (unsigned long long)evidence.observed_boottime_ns,
				 evidence.boot_status0_before, evidence.boot_status0_after,
				 evidence.boot_status1_before, evidence.boot_status1_after,
				 evidence.ccif_before.ap_busy, evidence.ccif_before.md_busy,
				 evidence.ccif_after.ap_busy, evidence.ccif_after.md_busy,
				 evidence.ccif_before.ap_ack, evidence.ccif_before.md_ack,
				 evidence.ccif_after.ap_ack, evidence.ccif_after.md_ack,
				 evidence.ccif_before.ap_sram_nonzero_words,
				 evidence.ccif_before.md_sram_nonzero_words,
				 evidence.ccif_after.ap_sram_nonzero_words,
				 evidence.ccif_after.md_sram_nonzero_words);
			return 0;
		}
		udelay(MT6797_MD1_BOOT_OBSERVE_DELAY_US);
	}

	evidence.token = MT6797_MD1_POST_VECTOR_NO_GO;
	evidence.observed = true;
	mt6797_md1_cold_post_vector_publish(&evidence);

	dev_crit(backend->dev,
		 "MD1 post-vector oracle NO-GO: vector[0x%08x]=%#x polls=%u t=%llu..%llu stats[0x10201300]=%#08x->%#08x stats[0x10201304]=%#08x->%#08x CCIF busy=%#x/%#x->%#x/%#x ack=%#x/%#x->%#x/%#x sram=%u/%u->%u/%u; no IRQ, CLDMA or shared-memory access\n",
		 MT6797_MD1_BOOT_VECTOR_BASE, vector, evidence.polls,
		 (unsigned long long)evidence.baseline_boottime_ns,
		 (unsigned long long)evidence.observed_boottime_ns,
		 evidence.boot_status0_before, evidence.boot_status0_after,
		 evidence.boot_status1_before, evidence.boot_status1_after,
		 evidence.ccif_before.ap_busy, evidence.ccif_before.md_busy,
		 evidence.ccif_after.ap_busy, evidence.ccif_after.md_busy,
		 evidence.ccif_before.ap_ack, evidence.ccif_before.md_ack,
		 evidence.ccif_after.ap_ack, evidence.ccif_after.md_ack,
		 evidence.ccif_before.ap_sram_nonzero_words,
		 evidence.ccif_before.md_sram_nonzero_words,
		 evidence.ccif_after.ap_sram_nonzero_words,
		 evidence.ccif_after.md_sram_nonzero_words);
	return -ETIMEDOUT;
}

static int mt6797_md1_cold_owner_probe(struct platform_device *pdev)
{
	struct mt6797_md1_boot_status_evidence boot_status;
	struct mt6797_md1_boot_status_preflight preflight;
	struct mt6797_md1_mpu_remap_layout layout;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_firmware_names firmware_names = {};
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
	backend->cldma_resets[0].id = "cldma-ao";
	backend->cldma_resets[1].id = "cldma-pdn";
	ret = devm_reset_control_bulk_get_exclusive(&pdev->dev,
			ARRAY_SIZE(backend->cldma_resets), backend->cldma_resets);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"exclusive AP CLDMA reset ownership unavailable\n");
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
	INIT_WORK(&backend->passive_observe_work,
		  mt6797_md1_cold_passive_observe_work);
	ret = devm_add_action_or_reset(&pdev->dev,
		mt6797_md1_cold_cancel_passive_observer,
		backend);
	if (ret)
		return ret;
	ret = mt6797_md1_cold_owner_init(owner, &mt6797_md1_cold_owner_ops, backend);
	if (ret)
		return ret;
	if (device_property_read_bool(&pdev->dev,
				      "mediatek,stage-mainline-firmware-load")) {
		struct mt6797_md1_mpu_remap_plan loaded_plan;

		if (!READ_ONCE(enable_experimental_md1_firmware_load))
			return dev_err_probe(&pdev->dev, -EPERM,
				"mainline firmware load needs its independent module gate\n");
		ret = mt6797_md1_cold_firmware_names_get(&pdev->dev,
						  &firmware_names);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
				"firmware-name must contain MD and any required DSP/ARM7 files\n");
		ret = mt6797_md1_lk_handoff_layout_get(&layout);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
				"validated MD1 memory layout unavailable\n");
		ret = mtk_wdt_require_boot_recovery(&pdev->dev);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
				"recovery watchdog required before firmware writes\n");
		ret = mt6797_md1_firmware_load(&pdev->dev, &firmware_names, &layout,
						&plan,
						mt6797_md1_cold_firmware_verify, backend,
						&loaded_plan);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
				"validated MD1 firmware load failed before release\n");
		plan = loaded_plan;
	}
	preflight = (struct mt6797_md1_boot_status_preflight) {
		.lk_image_loaded = true,
		.boot_status_observed = boot_status.observed,
		.stats0_samples = boot_status.stats0_samples,
		.stats1_samples = boot_status.stats1_samples,
		.stats0_final = boot_status.stats0_final,
		.stats1_final = boot_status.stats1_final,
	};

	/* Vendor CCCI: final LK remap, initial MPU, then CCIF/power sequence. */
	ret = mtk_wdt_require_boot_recovery(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"recovery watchdog required before MD1/EMI writes\n");
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
	ret = mt6797_md1_cold_owner_post_vector_evidence_get(
		&backend->passive_evidence);
	if (ret)
		goto out_fatal;
	mutex_lock(&mt6797_md1_stage2_lock);
	mt6797_md1_stage2_dev = &pdev->dev;
	mt6797_md1_stage2_plan = plan;
	mt6797_md1_stage2_complete = false;
	mt6797_md1_stage2_armed = true;
	mutex_unlock(&mt6797_md1_stage2_lock);
	mutex_lock(&mt6797_md1_post_vector_evidence_lock);
	mt6797_md1_handoff_supplier = &pdev->dev;
	WRITE_ONCE(mt6797_md1_handoff_status, -EAGAIN);
	mutex_unlock(&mt6797_md1_post_vector_evidence_lock);
	schedule_work(&backend->passive_observe_work);

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

static void mt6797_md1_cold_owner_shutdown(struct platform_device *pdev)
{
	struct mt6797_md1_cold_owner *owner = platform_get_drvdata(pdev);

	mt6797_md1_cold_cancel_passive_observer(owner->context);
}

static const struct of_device_id mt6797_md1_cold_owner_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-cold-owner" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_cold_owner_of_match);

static struct platform_driver mt6797_md1_cold_owner_driver = {
	.probe = mt6797_md1_cold_owner_probe,
	.shutdown = mt6797_md1_cold_owner_shutdown,
	.driver = {
		.name = "mt6797-md1-cold-owner",
		.of_match_table = mt6797_md1_cold_owner_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_cold_owner_driver);

MODULE_DESCRIPTION("Default-disabled MT6797 MD1 one-shot cold power/PLL/CCIF owner");
MODULE_LICENSE("GPL");
