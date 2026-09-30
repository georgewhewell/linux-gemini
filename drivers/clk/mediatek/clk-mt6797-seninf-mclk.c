// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 SENINF camera sensor master clocks (CMMCLKn pads).
 *
 * Each SENINF instance (0x1000 apart from 0x1a040000) has a timing
 * generator whose PH_CNT (+0x600) and SEN_CK (+0x604) registers drive
 * one CMMCLK pad from the CAMTG clock.
 *
 * What is known, and from where:
 * - The Planet MT6797 3.18 kernel turns sensor clock n on by setting
 *   bits 31 and 29 of SENINFn + 0x600 and off by clearing bit 29
 *   (ISP_MCLK1_EN/ISP_MCLK2_EN in cameraisp/src/mt6797/camera_isp.c).
 * - The same kernel selects the CAMTG mux parent (48 MHz or 208 MHz)
 *   before enabling it (kdSetSensorMclk in imgsensor kd_sensorlist.c).
 * - The divider field CLKCNT is bits 21:16 of TG1_SEN_CK at +0x604 in
 *   the MediaTek ISP 3.0 SENINF driver posted upstream for MT8365;
 *   the TG clock is the source divided by CLKCNT + 1. The MT6797 vendor
 *   kernel leaves SEN_CK to its user-space HAL, so this layout is taken
 *   from the same IP family and is not confirmed on MT6797.
 *
 * - Its (compiled-out) MCLK reset path writes 0x00000001 to PH_CNT, and
 *   the MT8365 driver documents PH_CNT == 0 as "master clock: ISP_CLK",
 *   so bit 0 is taken to select the CAMTG source. Also an inference.
 *
 * Only the CLKCNT field of SEN_CK and bits 31/29/0 of PH_CNT are modified;
 * the raw values before and after are logged at every enable.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define SENINF_TG_STRIDE	0x1000
#define SENINF_TG_PH_CNT	0x600
#define SENINF_TG_SEN_CK	0x604

#define PH_CNT_EN		BIT(31)
#define PH_CNT_CLK_EN		BIT(29)
#define PH_CNT_SRC_CAMTG	BIT(0)
#define SEN_CK_CLKCNT		GENMASK(21, 16)
#define SEN_CK_MAX_DIV		(FIELD_MAX(SEN_CK_CLKCNT) + 1)

#define MT6797_SENINF_NUM_MCLK	2

struct mt6797_seninf_mclk;

struct mt6797_mclk {
	struct clk_hw hw;
	struct mt6797_seninf_mclk *priv;
	void __iomem *tg;
	unsigned int id;
	unsigned int div;
	bool on;
};

struct mt6797_seninf_mclk {
	struct device *dev;
	struct clk_bulk_data clks[2];
	struct mt6797_mclk mclk[MT6797_SENINF_NUM_MCLK];
	struct clk_hw_onecell_data *data;
};

static inline struct mt6797_mclk *to_mclk(struct clk_hw *hw)
{
	return container_of(hw, struct mt6797_mclk, hw);
}

static void mt6797_mclk_write_div(struct mt6797_mclk *m)
{
	u32 ck = readl(m->tg + SENINF_TG_SEN_CK);

	ck &= ~SEN_CK_CLKCNT;
	ck |= FIELD_PREP(SEN_CK_CLKCNT, m->div - 1);
	writel(ck, m->tg + SENINF_TG_SEN_CK);
}

static int mt6797_mclk_prepare(struct clk_hw *hw)
{
	struct mt6797_seninf_mclk *priv = to_mclk(hw)->priv;

	/* The clock core already holds our runtime PM (ISP domain). */
	return clk_bulk_prepare_enable(ARRAY_SIZE(priv->clks), priv->clks);
}

static void mt6797_mclk_unprepare(struct clk_hw *hw)
{
	struct mt6797_seninf_mclk *priv = to_mclk(hw)->priv;

	clk_bulk_disable_unprepare(ARRAY_SIZE(priv->clks), priv->clks);
}

/*
 * The timing generator registers only take writes while CAMTG, our parent,
 * is running: programmed from .prepare, when the parent is merely prepared,
 * PH_CNT and SEN_CK read back 0. The core enables the parent before .enable.
 */
static int mt6797_mclk_enable(struct clk_hw *hw)
{
	struct mt6797_mclk *m = to_mclk(hw);
	u32 ph = readl(m->tg + SENINF_TG_PH_CNT);

	mt6797_mclk_write_div(m);
	writel(ph | PH_CNT_EN | PH_CNT_CLK_EN | PH_CNT_SRC_CAMTG,
	       m->tg + SENINF_TG_PH_CNT);
	m->on = true;
	dev_dbg(m->priv->dev, "cmmclk%u on: PH_CNT %#010x SEN_CK %#010x div %u\n",
		m->id, readl(m->tg + SENINF_TG_PH_CNT),
		readl(m->tg + SENINF_TG_SEN_CK), m->div);
	return 0;
}

static void mt6797_mclk_disable(struct clk_hw *hw)
{
	struct mt6797_mclk *m = to_mclk(hw);
	u32 ph = readl(m->tg + SENINF_TG_PH_CNT);

	/* The vendor kernel clears only bit 29 to stop the pad clock. */
	writel(ph & ~PH_CNT_CLK_EN, m->tg + SENINF_TG_PH_CNT);
	m->on = false;
}

static unsigned int mt6797_mclk_div(unsigned long parent, unsigned long rate)
{
	if (!rate)
		return SEN_CK_MAX_DIV;

	return clamp_t(unsigned long, DIV_ROUND_CLOSEST(parent, rate), 1,
		       SEN_CK_MAX_DIV);
}

static unsigned long mt6797_mclk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	return parent_rate / to_mclk(hw)->div;
}

static int mt6797_mclk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	req->rate = req->best_parent_rate /
		    mt6797_mclk_div(req->best_parent_rate, req->rate);

	return 0;
}

static int mt6797_mclk_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct mt6797_mclk *m = to_mclk(hw);

	m->div = mt6797_mclk_div(parent_rate, rate);
	if (m->on)
		mt6797_mclk_write_div(m);

	return 0;
}

static const struct clk_ops mt6797_mclk_ops = {
	.prepare = mt6797_mclk_prepare,
	.unprepare = mt6797_mclk_unprepare,
	.enable = mt6797_mclk_enable,
	.disable = mt6797_mclk_disable,
	.recalc_rate = mt6797_mclk_recalc_rate,
	.determine_rate = mt6797_mclk_determine_rate,
	.set_rate = mt6797_mclk_set_rate,
};

static int mt6797_seninf_mclk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_seninf_mclk *priv;
	struct resource *res;
	void __iomem *base;
	unsigned int i;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	if (resource_size(res) < MT6797_SENINF_NUM_MCLK * SENINF_TG_STRIDE)
		return dev_err_probe(dev, -EINVAL, "register window too small\n");

	priv->clks[0].id = "seninf";
	priv->clks[1].id = "camsys";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(priv->clks), priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	/* Must precede clock registration so the core tracks our PM. */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	priv->data = devm_kzalloc(dev, struct_size(priv->data, hws,
						   MT6797_SENINF_NUM_MCLK),
				  GFP_KERNEL);
	if (!priv->data)
		return -ENOMEM;
	priv->data->num = MT6797_SENINF_NUM_MCLK;

	for (i = 0; i < MT6797_SENINF_NUM_MCLK; i++) {
		struct mt6797_mclk *m = &priv->mclk[i];
		struct clk_parent_data parent = { .fw_name = "camtg" };
		struct clk_init_data init = {
			.ops = &mt6797_mclk_ops,
			.parent_data = &parent,
			.num_parents = 1,
		};

		init.name = devm_kasprintf(dev, GFP_KERNEL, "seninf_cmmclk%u", i);
		if (!init.name)
			return -ENOMEM;

		m->priv = priv;
		m->id = i;
		m->tg = base + i * SENINF_TG_STRIDE;
		m->div = SEN_CK_MAX_DIV;
		m->hw.init = &init;

		ret = devm_clk_hw_register(dev, &m->hw);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register %s\n",
					     init.name);
		priv->data->hws[i] = &m->hw;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   priv->data);
}

static const struct of_device_id mt6797_seninf_mclk_of_match[] = {
	{ .compatible = "mediatek,mt6797-seninf-mclk" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_seninf_mclk_of_match);

static struct platform_driver mt6797_seninf_mclk_driver = {
	.probe = mt6797_seninf_mclk_probe,
	.driver = {
		.name = "clk-mt6797-seninf-mclk",
		.of_match_table = mt6797_seninf_mclk_of_match,
	},
};
module_platform_driver(mt6797_seninf_mclk_driver);

MODULE_DESCRIPTION("MediaTek MT6797 SENINF camera sensor master clocks");
MODULE_LICENSE("GPL");
