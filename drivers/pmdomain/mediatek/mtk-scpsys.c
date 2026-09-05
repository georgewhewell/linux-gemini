// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6797-gpuldo-owner.h>
#include <linux/soc/mediatek/infracfg.h>

#include <dt-bindings/power/mt2701-power.h>
#include <dt-bindings/power/mt2712-power.h>
#include <dt-bindings/power/mt6797-power.h>
#include <dt-bindings/power/mt7622-power.h>
#include <dt-bindings/power/mt7623a-power.h>
#include <dt-bindings/power/mt8173-power.h>

#include "mtk-scpsys-internal.h"
#include "mt6797-gpuldo-owner.h"

static bool mt6797_gpuldo_owner_enable;
module_param_named(mt6797_gpuldo_owner, mt6797_gpuldo_owner_enable, bool, 0400);
MODULE_PARM_DESC(mt6797_gpuldo_owner, "Experimental Gemini exclusive fixed-profile GPULDO supply owner");
#define MT6797_GPULDO_DOMAIN_MASK GENMASK(13, 8)

#define MTK_POLL_DELAY_US   10
#define MTK_POLL_TIMEOUT    USEC_PER_SEC

#define MTK_SCPD_ACTIVE_WAKEUP		BIT(0)
#define MTK_SCPD_FWAIT_SRAM		BIT(1)
#define MTK_SCPD_KEEP_DEFAULT_OFF	BIT(2)
#define MTK_SCPD_CAPS(_scpd, _x)	((_scpd)->data->caps & (_x))

#define SPM_VDE_PWR_CON			0x0210
#define SPM_MFG_PWR_CON			0x0214
#define SPM_VEN_PWR_CON			0x0230
#define SPM_ISP_PWR_CON			0x0238
#define SPM_DIS_PWR_CON			0x023c
#define SPM_CONN_PWR_CON		0x0280
#define SPM_VEN2_PWR_CON		0x0298
#define SPM_AUDIO_PWR_CON		0x029c	/* MT8173, MT2712 */
#define SPM_BDP_PWR_CON			0x029c	/* MT2701 */
#define SPM_ETH_PWR_CON			0x02a0
#define SPM_HIF_PWR_CON			0x02a4
#define SPM_IFR_MSC_PWR_CON		0x02a8
#define SPM_MFG_2D_PWR_CON		0x02c0
#define SPM_MFG_ASYNC_PWR_CON		0x02c4
#define SPM_USB_PWR_CON			0x02cc
#define SPM_USB2_PWR_CON		0x02d4	/* MT2712 */
#define SPM_ETHSYS_PWR_CON		0x02e0	/* MT7622 */
#define SPM_HIF0_PWR_CON		0x02e4	/* MT7622 */
#define SPM_HIF1_PWR_CON		0x02e8	/* MT7622 */
#define SPM_WB_PWR_CON			0x02ec	/* MT7622 */

#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B_BIT			BIT(0)
#define PWR_ISO_BIT			BIT(1)
#define PWR_ON_BIT			BIT(2)
#define PWR_ON_2ND_BIT			BIT(3)
#define PWR_CLK_DIS_BIT			BIT(4)

#define PWR_STATUS_CONN			BIT(1)
#define PWR_STATUS_DISP			BIT(3)
#define PWR_STATUS_MFG			BIT(4)
#define PWR_STATUS_ISP			BIT(5)
#define PWR_STATUS_VDEC			BIT(7)
#define PWR_STATUS_BDP			BIT(14)
#define PWR_STATUS_ETH			BIT(15)
#define PWR_STATUS_HIF			BIT(16)
#define PWR_STATUS_IFR_MSC		BIT(17)
#define PWR_STATUS_USB2			BIT(19)	/* MT2712 */
#define PWR_STATUS_VENC_LT		BIT(20)
#define PWR_STATUS_VENC			BIT(21)
#define PWR_STATUS_MFG_2D		BIT(22)	/* MT8173 */
#define PWR_STATUS_MFG_ASYNC		BIT(23)	/* MT8173 */
#define PWR_STATUS_AUDIO		BIT(24)	/* MT8173, MT2712 */
#define PWR_STATUS_USB			BIT(25)	/* MT8173, MT2712 */
#define PWR_STATUS_ETHSYS		BIT(24)	/* MT7622 */
#define PWR_STATUS_HIF0			BIT(25)	/* MT7622 */
#define PWR_STATUS_HIF1			BIT(26)	/* MT7622 */
#define PWR_STATUS_WB			BIT(27)	/* MT7622 */

enum clk_id {
	CLK_NONE,
	CLK_MM,
	CLK_MFG,
	CLK_MFG_52M,
	CLK_VENC,
	CLK_VENC_LT,
	CLK_ETHIF,
	CLK_VDEC,
	CLK_HIFSEL,
	CLK_JPGDEC,
	CLK_AUDIO,
	CLK_MAX,
};

static const char * const clk_names[] = {
	NULL,
	"mm",
	"mfg",
	"mfg_52m",
	"venc",
	"venc_lt",
	"ethif",
	"vdec",
	"hif_sel",
	"jpgdec",
	"audio",
	NULL,
};

#define MAX_CLKS	3

/**
 * struct scp_domain_data - scp domain data for power on/off flow
 * @name: The domain name.
 * @sta_mask: The mask for power on/off status bit.
 * @ctl_offs: The offset for main power control register.
 * @sram_pdn_ctl_offs: Optional offset for SRAM power-down control bits.
 * @sram_pdn_ack_offs: Optional offset for SRAM power-down acknowledge bits.
 * @sram_pdn_bits: The mask for sram power control bits.
 * @sram_pdn_ack_bits: The mask for sram power control acked bits.
 * @bus_prot_mask: The mask for single step bus protection.
 * @bus_prot_group_mask: The power status mask for domains sharing protection.
 * @clk_id: The basic clocks required by this power domain.
 * @caps: The flag for active wake-up action.
 */
struct scp_domain_data {
	const char *name;
	u32 sta_mask;
	int ctl_offs;
	int sram_pdn_ctl_offs;
	int sram_pdn_ack_offs;
	u32 sram_pdn_bits;
	u32 sram_pdn_ack_bits;
	u32 bus_prot_mask;
	u32 bus_prot_group_mask;
	enum clk_id clk_id[MAX_CLKS];
	u8 caps;
};

struct scp;

struct scp_domain {
	struct generic_pm_domain genpd;
	struct scp *scp;
	struct clk *clk[MAX_CLKS];
	const struct scp_domain_data *data;
	struct regulator *supply;
};

struct scp_ctrl_reg {
	int pwr_sta_offs;
	int pwr_sta2nd_offs;
};

struct scp {
	struct scp_domain *domains;
	struct genpd_onecell_data pd_data;
	struct device *dev;
	void __iomem *base;
	struct regmap *infracfg;
	/* Serialize transitions of domains sharing one protection mask. */
	struct mutex bus_prot_lock;
	struct scp_ctrl_reg ctrl_reg;
	bool bus_prot_reg_update;
	bool gpuldo_mode;
	struct mutex gpuldo_lock;
	struct mt6797_gpuldo_owner gpuldo;
	struct device_node *gpuldo_gpu;
};

/* Built-in, non-removable provider; publish only after registration. */
static struct scp *gpuldo_provider;

int mtk_scpsys_gpuldo_gpu_admit(struct device_node *gpu)
{
	static const char * const names[] = { "core0", "core1", "core2", "core3" };
	struct scp *scp = smp_load_acquire(&gpuldo_provider);
	struct of_phandle_args args;
	const char *name;
	unsigned int i;
	int ret;

	if (!scp)
		return -EPROBE_DEFER;
	if (gpu != scp->gpuldo_gpu ||
	    !of_property_read_bool(gpu, "mediatek,gemini-gpuldo-owner") ||
	    of_find_property(gpu, "mali-supply", NULL) ||
	    of_count_phandle_with_args(gpu, "power-domains", "#power-domain-cells") != 4 ||
	    of_property_count_strings(gpu, "power-domain-names") != 4)
		return -EINVAL;
	for (i = 0; i < 4; ++i) {
		ret = of_parse_phandle_with_args(gpu, "power-domains",
					 "#power-domain-cells", i, &args);
		if (ret)
			return ret;
		ret = args.np != scp->dev->of_node || args.args_count != 1 ||
			args.args[0] != MT6797_POWER_DOMAIN_MFG_CORE0 + i;
		of_node_put(args.np);
		if (ret || of_property_read_string_index(gpu, "power-domain-names", i, &name) ||
		    strcmp(name, names[i]))
			return -EINVAL;
	}
	mutex_lock(&scp->gpuldo_lock);
	ret = scp->gpuldo_mode && scp->gpuldo.admitted && !scp->gpuldo.failed ? 0 : -EPERM;
	mutex_unlock(&scp->gpuldo_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_scpsys_gpuldo_gpu_admit);

struct scp_subdomain {
	int origin;
	int subdomain;
};

struct scp_soc_data {
	const struct scp_domain_data *domains;
	int num_domains;
	const struct scp_subdomain *subdomains;
	int num_subdomains;
	const struct scp_ctrl_reg regs;
	bool bus_prot_reg_update;
};

static int scpsys_domain_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	u32 status = readl(scp->base + scp->ctrl_reg.pwr_sta_offs) &
						scpd->data->sta_mask;
	u32 status2 = readl(scp->base + scp->ctrl_reg.pwr_sta2nd_offs) &
						scpd->data->sta_mask;

	/*
	 * A domain is on when both status bits are set. If only one is set
	 * return an error. This happens while powering up a domain
	 */

	if (status && status2)
		return true;
	if (!status && !status2)
		return false;

	return -EINVAL;
}

static bool scpsys_gpuldo_domain(struct scp_domain *scpd)
{
	return scpd->scp->gpuldo_mode &&
		(scpd->data->sta_mask & MT6797_GPULDO_DOMAIN_MASK);
}

static int scpsys_gpuldo_all_off(void *ctx)
{
	struct scp *scp = ctx;
	u32 a = readl(scp->base + scp->ctrl_reg.pwr_sta_offs);
	u32 b = readl(scp->base + scp->ctrl_reg.pwr_sta2nd_offs);

	return !((a | b) & MT6797_GPULDO_DOMAIN_MASK);
}

static int scpsys_gpuldo_write(void *ctx, unsigned int reg, unsigned int value)
{
	struct scp *scp = ctx;

	return regmap_write(scp->infracfg, reg, value);
}

static int scpsys_gpuldo_supply_on(void *ctx)
{
	struct scp *scp = ctx;
	struct regulator *supply = scp->domains[MT6797_POWER_DOMAIN_MFG_ASYNC].supply;
	int ret;

	/* Existing G3 single-OPP interval; not an inferred internal LDO voltage. */
	ret = regulator_set_voltage(supply, 846700, 850000);
	return ret ? ret : regulator_enable(supply);
}

static int scpsys_gpuldo_supply_off(void *ctx)
{
	struct scp *scp = ctx;

	return regulator_disable(scp->domains[MT6797_POWER_DOMAIN_MFG_ASYNC].supply);
}

static int scpsys_gpuldo_supply_enabled(void *ctx)
{
	struct scp *scp = ctx;

	return regulator_is_enabled(scp->domains[MT6797_POWER_DOMAIN_MFG_ASYNC].supply);
}

static const struct mt6797_gpuldo_ops scpsys_gpuldo_ops = {
	.write = scpsys_gpuldo_write,
	.supply_enable = scpsys_gpuldo_supply_on,
	.supply_disable = scpsys_gpuldo_supply_off,
	.supply_is_enabled = scpsys_gpuldo_supply_enabled,
	.all_domains_off = scpsys_gpuldo_all_off,
};

static int scpsys_regulator_enable(struct scp_domain *scpd)
{
	if (scpsys_gpuldo_domain(scpd) && scpd->data->sta_mask == BIT(13))
		return mt6797_gpuldo_enable(&scpd->scp->gpuldo,
				&scpsys_gpuldo_ops, scpd->scp);
	if (!scpd->supply)
		return 0;

	return regulator_enable(scpd->supply);
}

static int scpsys_regulator_disable(struct scp_domain *scpd)
{
	if (scpsys_gpuldo_domain(scpd) && scpd->data->sta_mask == BIT(13))
		return mt6797_gpuldo_disable(&scpd->scp->gpuldo,
				&scpsys_gpuldo_ops, scpd->scp);
	if (!scpd->supply)
		return 0;

	return regulator_disable(scpd->supply);
}

static void scpsys_clk_disable(struct clk *clk[], int max_num)
{
	int i;

	for (i = max_num - 1; i >= 0; i--)
		clk_disable_unprepare(clk[i]);
}

static int scpsys_clk_enable(struct clk *clk[], int max_num)
{
	int i, ret = 0;

	for (i = 0; i < max_num && clk[i]; i++) {
		ret = clk_prepare_enable(clk[i]);
		if (ret) {
			scpsys_clk_disable(clk, i);
			break;
		}
	}

	return ret;
}

static int scpsys_sram_enable(struct scp_domain *scpd, void __iomem *pdn_addr,
				      void __iomem *ack_addr)
{
	u32 val;
	u32 pdn_ack = scpd->data->sram_pdn_ack_bits;
	int tmp;

	val = readl(pdn_addr);
	val &= ~scpd->data->sram_pdn_bits;
	writel(val, pdn_addr);

	/* Either wait until SRAM_PDN_ACK all 0 or have a force wait */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_FWAIT_SRAM)) {
		/*
		 * Currently, MTK_SCPD_FWAIT_SRAM is necessary only for
		 * MT7622_POWER_DOMAIN_WB and thus just a trivial setup
		 * is applied here.
		 */
		usleep_range(12000, 12100);
	} else {
		/* Either wait until SRAM_PDN_ACK all 1 or 0 */
		int ret = readl_poll_timeout(ack_addr, tmp,
				(tmp & pdn_ack) == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int scpsys_sram_disable(struct scp_domain *scpd, void __iomem *pdn_addr,
				       void __iomem *ack_addr)
{
	u32 val;
	u32 pdn_ack = scpd->data->sram_pdn_ack_bits;
	int tmp;

	val = readl(pdn_addr);
	val |= scpd->data->sram_pdn_bits;
	writel(val, pdn_addr);

	/* Either wait until SRAM_PDN_ACK all 1 or 0 */
	return readl_poll_timeout(ack_addr, tmp,
			(tmp & pdn_ack) == pdn_ack,
			MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
}

static int scpsys_bus_protect_enable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	if (!scpd->data->bus_prot_mask)
		return 0;

	return mtk_infracfg_set_bus_protection(scp->infracfg,
			scpd->data->bus_prot_mask,
			scp->bus_prot_reg_update);
}

static int scpsys_bus_protect_disable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	u32 group_mask = scpd->data->bus_prot_group_mask;
	u32 status, status2;

	if (!scpd->data->bus_prot_mask)
		return 0;

	if (group_mask) {
		status = readl(scp->base + scp->ctrl_reg.pwr_sta_offs);
		status2 = readl(scp->base + scp->ctrl_reg.pwr_sta2nd_offs);
		if (!mtk_scpsys_bus_prot_group_is_on(status, status2, group_mask))
			return 0;
	}

	return mtk_infracfg_clear_bus_protection(scp->infracfg,
			scpd->data->bus_prot_mask,
			scp->bus_prot_reg_update);
}

static int scpsys_power_on_raw(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	const char *stage;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	void __iomem *sram_pdn_addr = scp->base + (scpd->data->sram_pdn_ctl_offs ?
		scpd->data->sram_pdn_ctl_offs : scpd->data->ctl_offs);
	void __iomem *sram_ack_addr = scp->base + (scpd->data->sram_pdn_ack_offs ?
		scpd->data->sram_pdn_ack_offs : scpd->data->ctl_offs);
	bool shared_bus_protection = scpd->data->bus_prot_group_mask;
	u32 val;
	int ret, tmp;

	if (shared_bus_protection) {
		stage = "bus protection engage";
		mutex_lock(&scp->bus_prot_lock);
		ret = scpsys_bus_protect_enable(scpd);
		if (ret < 0)
			goto err_regulator;
	}

	stage = "regulator enable";
	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		goto err_regulator;

	stage = "clock enable";
	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto err_clk;

	/* subsys power on */
	val = readl(ctl_addr);
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);
	val |= PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 1 */
	stage = "power acknowledge";
	ret = readx_poll_timeout(scpsys_domain_is_on, scpd, tmp, tmp > 0,
				 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_pwr_ack;

	val &= ~PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ISO_BIT;
	writel(val, ctl_addr);

	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	stage = "SRAM acknowledge";
	ret = scpsys_sram_enable(scpd, sram_pdn_addr, sram_ack_addr);
	if (ret < 0)
		goto err_pwr_ack;

	stage = "bus protection release";
	ret = scpsys_bus_protect_disable(scpd);
	if (ret < 0) {
		tmp = scpsys_bus_protect_enable(scpd);
		if (tmp < 0)
			dev_err(scp->dev,
				"Failed to restore bus protection for domain %s: %d\n",
				genpd->name, tmp);
		goto err_pwr_ack;
	}

	if (shared_bus_protection)
		mutex_unlock(&scp->bus_prot_lock);

	return 0;

err_pwr_ack:
	/* Unknown state: retain clocks, rail and LDO until separate rescue boot. */
	if (scpsys_gpuldo_domain(scpd))
		goto err_regulator;
	scpsys_clk_disable(scpd->clk, MAX_CLKS);
err_clk:
	if (scpsys_gpuldo_domain(scpd))
		goto err_regulator;
	scpsys_regulator_disable(scpd);
err_regulator:

	dev_err(scp->dev, "Failed to power on domain %s at %s: %d\n",
		genpd->name, stage, ret);
	if (shared_bus_protection)
		mutex_unlock(&scp->bus_prot_lock);

	return ret;
}

static int scpsys_power_off_raw(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	const char *stage = "bus protection engage";
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	void __iomem *sram_pdn_addr = scp->base + (scpd->data->sram_pdn_ctl_offs ?
		scpd->data->sram_pdn_ctl_offs : scpd->data->ctl_offs);
	void __iomem *sram_ack_addr = scp->base + (scpd->data->sram_pdn_ack_offs ?
		scpd->data->sram_pdn_ack_offs : scpd->data->ctl_offs);
	bool shared_bus_protection = scpd->data->bus_prot_group_mask;
	u32 val;
	int ret, tmp;

	if (shared_bus_protection)
		mutex_lock(&scp->bus_prot_lock);

	ret = scpsys_bus_protect_enable(scpd);
	if (ret < 0)
		goto out;

	stage = "SRAM acknowledge";
	ret = scpsys_sram_disable(scpd, sram_pdn_addr, sram_ack_addr);
	if (ret < 0)
		goto out;

	/* subsys power off */
	val = readl(ctl_addr);
	val |= PWR_ISO_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val |= PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	stage = "power acknowledge";
	ret = readx_poll_timeout(scpsys_domain_is_on, scpd, tmp, tmp == 0,
				 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto out;

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	stage = "regulator disable";
	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto out;

	if (shared_bus_protection)
		mutex_unlock(&scp->bus_prot_lock);

	return 0;

out:
	dev_err(scp->dev, "Failed to power off domain %s at %s: %d\n",
		genpd->name, stage, ret);
	if (shared_bus_protection)
		mutex_unlock(&scp->bus_prot_lock);

	return ret;
}

/* Serialize siblings; a child failure latches before any parent supply-off. */
static int scpsys_gpuldo_transition(struct generic_pm_domain *genpd, bool on)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	int ret;

	if (!scpsys_gpuldo_domain(scpd))
		return on ? scpsys_power_on_raw(genpd) : scpsys_power_off_raw(genpd);
	mutex_lock(&scp->gpuldo_lock);
	if (scp->gpuldo.failed || !scp->gpuldo.admitted ||
	    (scpd->data->sta_mask != BIT(13) && !scp->gpuldo.active)) {
		ret = -EPERM;
		goto out;
	}
	ret = on ? scpsys_power_on_raw(genpd) : scpsys_power_off_raw(genpd);
	if (ret)
		mt6797_gpuldo_fail(&scp->gpuldo, ret);
out:
	mutex_unlock(&scp->gpuldo_lock);
	return ret;
}

static int scpsys_power_on(struct generic_pm_domain *genpd)
{
	return scpsys_gpuldo_transition(genpd, true);
}

static int scpsys_power_off(struct generic_pm_domain *genpd)
{
	return scpsys_gpuldo_transition(genpd, false);
}

static void init_clks(struct platform_device *pdev, struct clk **clk)
{
	int i;

	for (i = CLK_NONE + 1; i < CLK_MAX; i++)
		clk[i] = devm_clk_get(&pdev->dev, clk_names[i]);
}

static int scpsys_gpuldo_validate_dt(struct device *dev)
{
	struct device_node *np = dev->of_node, *infra, *gpu, *supply, *table, *opp;
	struct device_node *node;
	struct property *prop;
	struct resource res;
	struct device_node *other, *parent;
	const char *name;
	u32 addr;
	u32 uv[3];
	u64 hz;
	int ret = -EINVAL, count = 0;

	if (!of_property_read_bool(np, "mediatek,gemini-gpuldo-owner"))
		return 0;
	if (!mt6797_gpuldo_owner_enable ||
	    !of_machine_is_compatible("planet,gemini-pda") ||
	    !of_machine_is_compatible("mediatek,mt6797") ||
	    !of_device_is_compatible(np, "mediatek,mt6797-scpsys"))
		return -EPERM;
	infra = of_parse_phandle(np, "infracfg", 0);
	if (!infra)
		return -EINVAL;
	ret = of_address_to_resource(infra, 0, &res);
	if (!ret && (!of_device_is_compatible(infra, "mediatek,mt6797-infracfg") ||
	    !of_device_is_compatible(infra, "syscon") ||
	    of_find_property(infra, "clocks", NULL) ||
	    of_find_property(infra, "resets", NULL) ||
	    !of_device_is_available(infra) || res.start != 0x10001000 ||
	    resource_size(&res) != 0x1000))
		ret = -EINVAL;
	other = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-infracfg");
	if (other != infra)
		ret = -EINVAL;
	other = of_find_compatible_node(other, NULL, "mediatek,mt6797-infracfg");
	if (other)
		ret = -EINVAL;
	of_node_put(other);
	of_node_put(infra);
	if (ret)
		return ret;
	ret = -EINVAL;
	supply = of_parse_phandle(np, "mfg_async-supply", 0);
	if (!supply)
		return ret;
	if (!of_device_is_compatible(supply, "richtek,rt5735") ||
	    !of_device_is_available(supply) ||
	    of_property_count_u32_elems(supply, "reg") != 1 ||
	    of_property_read_u32(supply, "reg", &addr) || addr != 0x1c ||
	    of_property_read_string(supply, "regulator-name", &name) ||
	    strcmp(name, "vgpu") ||
	    of_property_read_bool(supply, "regulator-always-on") ||
	    of_property_read_bool(supply, "regulator-boot-on"))
		goto put_supply;
	parent = of_get_parent(supply);
	if (!parent)
		goto put_supply;
	if (!of_device_is_available(parent) ||
	    !of_device_is_compatible(parent, "mediatek,mt6797-i2c") ||
	    of_address_to_resource(parent, 0, &res) ||
	    res.start != 0x11010000 || resource_size(&res) != 0x1000 ||
	    of_address_to_resource(parent, 1, &res) ||
	    res.start != 0x11000580 || resource_size(&res) != 0x80) {
		of_node_put(parent);
		goto put_supply;
	}
	of_node_put(parent);
	/* No upstream supply side effects from exclusive-get on inherited state. */
	for_each_property_of_node(supply, prop)
		if (strends(prop->name, "-supply"))
			goto put_supply;
	/* Reject every duplicate reference, including the former MFG/OPP votes. */
	for_each_of_allnodes(node) {
		for_each_property_of_node(node, prop) {
			struct device_node *target;

			if (!strends(prop->name, "-supply"))
				continue;
			target = of_parse_phandle(node, prop->name, 0);
			if (target == supply &&
			    (node != np || strcmp(prop->name, "mfg_async-supply"))) {
				of_node_put(target);
				of_node_put(node);
				goto put_supply;
			}
			of_node_put(target);
		}
	}
	gpu = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-mali");
	if (!gpu)
		goto put_supply;
	other = of_find_compatible_node(of_node_get(gpu), NULL, "mediatek,mt6797-mali");
	if (other) {
		of_node_put(other);
		goto put_gpu;
	}
	if (!of_device_is_available(gpu) ||
	    of_address_to_resource(gpu, 0, &res) ||
	    res.start != 0x13040000 || resource_size(&res) != 0x4000 ||
	    !of_property_read_bool(gpu, "mediatek,gemini-gpuldo-owner"))
		goto put_gpu;
	table = of_parse_phandle(gpu, "operating-points-v2", 0);
	if (!table)
		goto put_gpu;
	for_each_available_child_of_node(table, opp) {
		if (++count != 1 ||
		    of_property_count_u64_elems(opp, "opp-hz") != 1 ||
		    of_property_count_u32_elems(opp, "opp-microvolt") != 3 ||
		    of_property_read_u64(opp, "opp-hz", &hz) ||
		    hz != 238000000 ||
		    of_property_read_u32_array(opp, "opp-microvolt", uv, 3) ||
		    uv[0] != 846700 || uv[1] != 846700 || uv[2] != 850000) {
			of_node_put(opp);
			count = -1;
			break;
		}
	}
	of_node_put(table);
	if (count == 1)
		ret = 1;
put_gpu:
	of_node_put(gpu);
put_supply:
	of_node_put(supply);
	return ret;
}

static struct scp *init_scp(struct platform_device *pdev,
			const struct scp_domain_data *scp_domain_data, int num,
			const struct scp_ctrl_reg *scp_ctrl_reg,
			bool bus_prot_reg_update)
{
	struct genpd_onecell_data *pd_data;
	int i, j;
	struct scp *scp;
	struct clk *clk[CLK_MAX];

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return ERR_PTR(-ENOMEM);

	scp->ctrl_reg.pwr_sta_offs = scp_ctrl_reg->pwr_sta_offs;
	scp->ctrl_reg.pwr_sta2nd_offs = scp_ctrl_reg->pwr_sta2nd_offs;

	scp->bus_prot_reg_update = bus_prot_reg_update;
	mutex_init(&scp->bus_prot_lock);
	mutex_init(&scp->gpuldo_lock);
	i = scpsys_gpuldo_validate_dt(&pdev->dev);
	if (i < 0)
		return ERR_PTR(i);
	scp->gpuldo_mode = i == 1;

	scp->dev = &pdev->dev;

	scp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(scp->base))
		return ERR_CAST(scp->base);

	scp->domains = devm_kcalloc(&pdev->dev,
				num, sizeof(*scp->domains), GFP_KERNEL);
	if (!scp->domains)
		return ERR_PTR(-ENOMEM);

	pd_data = &scp->pd_data;

	pd_data->domains = devm_kcalloc(&pdev->dev,
			num, sizeof(*pd_data->domains), GFP_KERNEL);
	if (!pd_data->domains)
		return ERR_PTR(-ENOMEM);

	scp->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
			"infracfg");
	if (IS_ERR(scp->infracfg)) {
		dev_err(&pdev->dev, "Cannot find infracfg controller: %ld\n",
				PTR_ERR(scp->infracfg));
		return ERR_CAST(scp->infracfg);
	}

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		const struct scp_domain_data *data = &scp_domain_data[i];

		if (scp->gpuldo_mode && i == MT6797_POWER_DOMAIN_MFG_ASYNC)
			scpd->supply = devm_regulator_get_exclusive(&pdev->dev, data->name);
		else
			scpd->supply = devm_regulator_get_optional(&pdev->dev, data->name);
		if (IS_ERR(scpd->supply)) {
			if (PTR_ERR(scpd->supply) == -ENODEV &&
			    !(scp->gpuldo_mode && i == MT6797_POWER_DOMAIN_MFG_ASYNC))
				scpd->supply = NULL;
			else
				return ERR_CAST(scpd->supply);
		}
	}

	pd_data->num_domains = num;

	init_clks(pdev, clk);

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		const struct scp_domain_data *data = &scp_domain_data[i];

		pd_data->domains[i] = genpd;
		scpd->scp = scp;

		scpd->data = data;

		for (j = 0; j < MAX_CLKS && data->clk_id[j]; j++) {
			struct clk *c = clk[data->clk_id[j]];

			if (IS_ERR(c)) {
				dev_err(&pdev->dev, "%s: clk unavailable\n",
					data->name);
				return ERR_CAST(c);
			}

			scpd->clk[j] = c;
		}

		genpd->name = data->name;
		genpd->power_off = scpsys_power_off;
		genpd->power_on = scpsys_power_on;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_ACTIVE_WAKEUP))
			genpd->flags |= GENPD_FLAG_ACTIVE_WAKEUP;
	}

	if (scp->gpuldo_mode) {
		i = mt6797_gpuldo_admit(&scp->gpuldo, &scpsys_gpuldo_ops, scp);
		if (i)
			return ERR_PTR(i);
		/* Static non-removable provider retains this validated node identity. */
		scp->gpuldo_gpu = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-mali");
	}
	return scp;
}

static int mtk_register_power_domains(struct platform_device *pdev,
				struct scp *scp, int num)
{
	struct genpd_onecell_data *pd_data;
	int i, ret, first_error = 0;

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		bool on;

		/*
		 * Initially turn on all domains to make the domains usable
		 * with !CONFIG_PM and to get the hardware in sync with the
		 * software.  The unused domains will be switched off during
		 * late_init time.
		 */
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_KEEP_DEFAULT_OFF))
			on = false;
		else
			on = !WARN_ON(genpd->power_on(genpd) < 0);

		ret = pm_genpd_init(genpd, NULL, !on);
		if (ret && !first_error)
			first_error = ret;
	}

	/*
	 * We are not allowed to fail here since there is no way to unregister
	 * a power domain. Once registered above we have to keep the domains
	 * valid.
	 */

	pd_data = &scp->pd_data;

	/* Keep initialized objects alive, but never expose a partial owner. */
	if (scp->gpuldo_mode && first_error)
		return first_error;
	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, pd_data);
	if (ret)
		dev_err(&pdev->dev, "Failed to add OF provider: %d\n", ret);
	return ret;
}

/*
 * MT2701 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt2701[] = {
	[MT2701_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_CONN_M |
				 MT2701_TOP_AXI_PROT_EN_CONN_S,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_MM_M0,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MFG},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_BDP] = {
		.name = "bdp",
		.sta_mask = PWR_STATUS_BDP,
		.ctl_offs = SPM_BDP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = PWR_STATUS_ETH,
		.ctl_offs = SPM_ETH_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = PWR_STATUS_HIF,
		.ctl_offs = SPM_HIF_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = PWR_STATUS_IFR_MSC,
		.ctl_offs = SPM_IFR_MSC_PWR_CON,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

/*
 * MT2712 power domain support
 */
static const struct scp_domain_data scp_domain_data_mt2712[] = {
	[MT2712_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM, CLK_VDEC},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = PWR_STATUS_VENC,
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC, CLK_JPGDEC},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_AUDIO},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_USB] = {
		.name = "usb",
		.sta_mask = PWR_STATUS_USB,
		.ctl_offs = SPM_USB_PWR_CON,
		.sram_pdn_bits = GENMASK(10, 8),
		.sram_pdn_ack_bits = GENMASK(14, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_USB2] = {
		.name = "usb2",
		.sta_mask = PWR_STATUS_USB2,
		.ctl_offs = SPM_USB2_PWR_CON,
		.sram_pdn_bits = GENMASK(10, 8),
		.sram_pdn_ack_bits = GENMASK(14, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_MFG},
		.bus_prot_mask = BIT(14) | BIT(21) | BIT(23),
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC1] = {
		.name = "mfg_sc1",
		.sta_mask = BIT(22),
		.ctl_offs = 0x02c0,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC2] = {
		.name = "mfg_sc2",
		.sta_mask = BIT(23),
		.ctl_offs = 0x02c4,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC3] = {
		.name = "mfg_sc3",
		.sta_mask = BIT(30),
		.ctl_offs = 0x01f8,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

static const struct scp_subdomain scp_subdomain_mt2712[] = {
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_VDEC},
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_VENC},
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_ISP},
	{MT2712_POWER_DOMAIN_MFG, MT2712_POWER_DOMAIN_MFG_SC1},
	{MT2712_POWER_DOMAIN_MFG_SC1, MT2712_POWER_DOMAIN_MFG_SC2},
	{MT2712_POWER_DOMAIN_MFG_SC2, MT2712_POWER_DOMAIN_MFG_SC3},
};

/*
 * MT6797 power domain support
 */

#define SPM_MFG_SRAM_CON_MT6797		0x033c
#define MT6797_MFG_BUS_PROT_MASK	(BIT(21) | BIT(23))
#define MT6797_MFG_CORE_STATUS_MASK	GENMASK(11, 8)

/*
 * MFG protection covers all four independently controlled cores. Assert it
 * before any core powers down and release it only after every core is on.
 */

/*
 * The GPU and its MFGSYS clock provider are disabled in the Gemini DT.  Do
 * not turn this whole hierarchy on merely while registering the SCPSYS
 * provider: the generic registration path does that before it installs the
 * MFG parent links.  Leave the boot firmware's state alone until a future GPU
 * consumer explicitly votes for the hierarchy.
 */
#define MT6797_MFG_DEFAULT_OFF		MTK_SCPD_KEEP_DEFAULT_OFF

static const struct scp_domain_data scp_domain_data_mt6797[] = {
	[MT6797_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = 0x300,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_VDEC},
	},
	[MT6797_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = BIT(21),
		.ctl_offs = 0x304,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(5),
		.ctl_offs = 0x308,
		.sram_pdn_bits = GENMASK(9, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = BIT(3),
		.ctl_offs = 0x30C,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.bus_prot_mask = (BIT(1) | BIT(2)),
	},
	[MT6797_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(24),
		.ctl_offs = 0x314,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(13),
		.ctl_offs = 0x334,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(12),
		.ctl_offs = 0x338,
		.sram_pdn_ctl_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_ack_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_bits = GENMASK(1, 0),
		.sram_pdn_ack_bits = GENMASK(17, 16),
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE0] = {
		.name = "mfg_core0",
		.sta_mask = BIT(11),
		.ctl_offs = 0x340,
		.sram_pdn_ctl_offs = 0x340,
		.sram_pdn_ack_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(20),
		.bus_prot_mask = MT6797_MFG_BUS_PROT_MASK,
		.bus_prot_group_mask = MT6797_MFG_CORE_STATUS_MASK,
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE1] = {
		.name = "mfg_core1",
		.sta_mask = BIT(10),
		.ctl_offs = 0x344,
		.sram_pdn_ctl_offs = 0x344,
		.sram_pdn_ack_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(21),
		.bus_prot_mask = MT6797_MFG_BUS_PROT_MASK,
		.bus_prot_group_mask = MT6797_MFG_CORE_STATUS_MASK,
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE2] = {
		.name = "mfg_core2",
		.sta_mask = BIT(9),
		.ctl_offs = 0x348,
		.sram_pdn_ctl_offs = 0x348,
		.sram_pdn_ack_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(22),
		.bus_prot_mask = MT6797_MFG_BUS_PROT_MASK,
		.bus_prot_group_mask = MT6797_MFG_CORE_STATUS_MASK,
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MFG_CORE3] = {
		.name = "mfg_core3",
		.sta_mask = BIT(8),
		.ctl_offs = 0x34c,
		.sram_pdn_ctl_offs = 0x34c,
		.sram_pdn_ack_offs = SPM_MFG_SRAM_CON_MT6797,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(23),
		.bus_prot_mask = MT6797_MFG_BUS_PROT_MASK,
		.bus_prot_group_mask = MT6797_MFG_CORE_STATUS_MASK,
		.clk_id = {CLK_MFG_52M},
		.caps = MT6797_MFG_DEFAULT_OFF,
	},
	[MT6797_POWER_DOMAIN_MJC] = {
		.name = "mjc",
		.sta_mask = BIT(20),
		.ctl_offs = 0x310,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_NONE},
	},
	/*
	 * The vendor CCF path uses SPM 0x32c, both PWR_STATUS registers bit 1,
	 * and infracfg protection bits 17 and 18. There is no SRAM operation.
	 *
	 * Do not power CONN while registering SCPSYS. Its first vote must be
	 * made by an explicitly enabled, recoverable identity experiment after
	 * CONMCU reset has been asserted.
	 */
	[MT6797_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = BIT(1),
		.ctl_offs = 0x32c,
		.bus_prot_mask = BIT(17) | BIT(18),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
	},
};

#define SPM_PWR_STATUS_MT6797		0x0180
#define SPM_PWR_STATUS_2ND_MT6797	0x0184

static const struct scp_subdomain scp_subdomain_mt6797[] = {
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_VDEC},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_ISP},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_VENC},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_MJC},
	{MT6797_POWER_DOMAIN_MFG_ASYNC, MT6797_POWER_DOMAIN_MFG},
	{MT6797_POWER_DOMAIN_MFG, MT6797_POWER_DOMAIN_MFG_CORE0},
	{MT6797_POWER_DOMAIN_MFG, MT6797_POWER_DOMAIN_MFG_CORE1},
	{MT6797_POWER_DOMAIN_MFG, MT6797_POWER_DOMAIN_MFG_CORE2},
	{MT6797_POWER_DOMAIN_MFG, MT6797_POWER_DOMAIN_MFG_CORE3},
};

/*
 * MT7622 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt7622[] = {
	[MT7622_POWER_DOMAIN_ETHSYS] = {
		.name = "ethsys",
		.sta_mask = PWR_STATUS_ETHSYS,
		.ctl_offs = SPM_ETHSYS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_ETHSYS,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_HIF0] = {
		.name = "hif0",
		.sta_mask = PWR_STATUS_HIF0,
		.ctl_offs = SPM_HIF0_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_HIFSEL},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_HIF0,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_HIF1] = {
		.name = "hif1",
		.sta_mask = PWR_STATUS_HIF1,
		.ctl_offs = SPM_HIF1_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_HIFSEL},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_HIF1,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_WB] = {
		.name = "wb",
		.sta_mask = PWR_STATUS_WB,
		.ctl_offs = SPM_WB_PWR_CON,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_WB,
		.caps = MTK_SCPD_ACTIVE_WAKEUP | MTK_SCPD_FWAIT_SRAM,
	},
	[MT7622_POWER_DOMAIN_AUDIO] = {
		.name = "audsys",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_HIF1,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

/*
 * MT7623A power domain support
 */

static const struct scp_domain_data scp_domain_data_mt7623a[] = {
	[MT7623A_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_CONN_M |
				 MT2701_TOP_AXI_PROT_EN_CONN_S,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = PWR_STATUS_ETH,
		.ctl_offs = SPM_ETH_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = PWR_STATUS_HIF,
		.ctl_offs = SPM_HIF_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = PWR_STATUS_IFR_MSC,
		.ctl_offs = SPM_IFR_MSC_PWR_CON,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

/*
 * MT8173 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt8173[] = {
	[MT8173_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = PWR_STATUS_VENC,
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC},
	},
	[MT8173_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MM_M0 |
			MT8173_TOP_AXI_PROT_EN_MM_M1,
	},
	[MT8173_POWER_DOMAIN_VENC_LT] = {
		.name = "venc_lt",
		.sta_mask = PWR_STATUS_VENC_LT,
		.ctl_offs = SPM_VEN2_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC_LT},
	},
	[MT8173_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_USB] = {
		.name = "usb",
		.sta_mask = PWR_STATUS_USB,
		.ctl_offs = SPM_USB_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT8173_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = PWR_STATUS_MFG_ASYNC,
		.ctl_offs = SPM_MFG_ASYNC_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_MFG},
	},
	[MT8173_POWER_DOMAIN_MFG_2D] = {
		.name = "mfg_2d",
		.sta_mask = PWR_STATUS_MFG_2D,
		.ctl_offs = SPM_MFG_2D_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(13, 8),
		.sram_pdn_ack_bits = GENMASK(21, 16),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MFG_S |
			MT8173_TOP_AXI_PROT_EN_MFG_M0 |
			MT8173_TOP_AXI_PROT_EN_MFG_M1 |
			MT8173_TOP_AXI_PROT_EN_MFG_SNOOP_OUT,
	},
};

static const struct scp_subdomain scp_subdomain_mt8173[] = {
	{MT8173_POWER_DOMAIN_MFG_ASYNC, MT8173_POWER_DOMAIN_MFG_2D},
	{MT8173_POWER_DOMAIN_MFG_2D, MT8173_POWER_DOMAIN_MFG},
};

static const struct scp_soc_data mt2701_data = {
	.domains = scp_domain_data_mt2701,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt2701),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt2712_data = {
	.domains = scp_domain_data_mt2712,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt2712),
	.subdomains = scp_subdomain_mt2712,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt2712),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = false,
};

static const struct scp_soc_data mt6797_data = {
	.domains = scp_domain_data_mt6797,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt6797),
	.subdomains = scp_subdomain_mt6797,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt6797),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS_MT6797,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND_MT6797
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt7622_data = {
	.domains = scp_domain_data_mt7622,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt7622),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt7623a_data = {
	.domains = scp_domain_data_mt7623a,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt7623a),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt8173_data = {
	.domains = scp_domain_data_mt8173,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt8173),
	.subdomains = scp_subdomain_mt8173,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt8173),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

/*
 * scpsys driver init
 */

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt2701-scpsys",
		.data = &mt2701_data,
	}, {
		.compatible = "mediatek,mt2712-scpsys",
		.data = &mt2712_data,
	}, {
		.compatible = "mediatek,mt6797-scpsys",
		.data = &mt6797_data,
	}, {
		.compatible = "mediatek,mt7622-scpsys",
		.data = &mt7622_data,
	}, {
		.compatible = "mediatek,mt7623a-scpsys",
		.data = &mt7623a_data,
	}, {
		.compatible = "mediatek,mt8173-scpsys",
		.data = &mt8173_data,
	}, {
		/* sentinel */
	}
};

static int scpsys_probe(struct platform_device *pdev)
{
	const struct scp_subdomain *sd;
	const struct scp_soc_data *soc;
	struct scp *scp;
	struct genpd_onecell_data *pd_data;
	int i, ret;

	soc = of_device_get_match_data(&pdev->dev);

	scp = init_scp(pdev, soc->domains, soc->num_domains, &soc->regs,
			soc->bus_prot_reg_update);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	ret = mtk_register_power_domains(pdev, scp, soc->num_domains);
	if (ret && scp->gpuldo_mode) {
		mt6797_gpuldo_fail(&scp->gpuldo, ret);
		/* Success retains devres backing non-removable domains. */
		return 0;
	}

	pd_data = &scp->pd_data;

	for (i = 0, sd = soc->subdomains; i < soc->num_subdomains; i++, sd++) {
		ret = pm_genpd_add_subdomain(pd_data->domains[sd->origin],
					     pd_data->domains[sd->subdomain]);
		if (ret && IS_ENABLED(CONFIG_PM))
			dev_err(&pdev->dev, "Failed to add subdomain: %d\n",
				ret);
		if (ret && scp->gpuldo_mode)
			mt6797_gpuldo_fail(&scp->gpuldo, ret);
	}
	if (scp->gpuldo_mode && !scp->gpuldo.failed)
		smp_store_release(&gpuldo_provider, scp);

	return 0;
}

static struct platform_driver scpsys_drv = {
	.probe = scpsys_probe,
	.driver = {
		.name = "mtk-scpsys",
		.suppress_bind_attrs = true,
		.of_match_table = of_scpsys_match_tbl,
	},
};
builtin_platform_driver(scpsys_drv);
