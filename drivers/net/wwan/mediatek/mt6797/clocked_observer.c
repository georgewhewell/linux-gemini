// SPDX-License-Identifier: GPL-2.0-only
/* Clock-bounded ownership observer for the MT6797 MD1 transport. */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "clocked_observer_core.h"

#define MT6797_MD1_SPM_PWR_STATUS	0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND	0x184
#define MT6797_MD1_POWER_STATUS		BIT(0)
#define MT6797_MD1_POWER_SAMPLES		3

#define MT6797_MD1_INFRA_CG_STA1	0x94
#define MT6797_MD1_INFRA_CLOCK_GATES	(BIT(16) | BIT(23) | BIT(26))

#define MT6797_MD1_CLDMA_UL_START	0x004
#define MT6797_MD1_CLDMA_UL_STATUS	0x050
#define MT6797_MD1_CLDMA_DL_START	0xa78
#define MT6797_MD1_CLDMA_DL_STATUS	0xab8
#define MT6797_MD1_CCIF_BUSY		0x004
#define MT6797_MD1_CCIF_RCHNUM		0x010

enum mt6797_md1_observer_reg {
	MT6797_MD1_OBSERVER_AP_AO,
	MT6797_MD1_OBSERVER_AP_PDN,
	MT6797_MD1_OBSERVER_AP_CCIF,
};

struct mt6797_md1_observer {
	struct device *dev;
	void __iomem *regs[3];
	struct clk *clocks[MT6797_MD1_OBSERVER_CLOCKS];
	struct regmap *spm;
	struct regmap *infracfg;
};

static const char * const mt6797_md1_observer_reg_names[] = {
	"ap-ao",
	"ap-pdn",
	"ap-ccif",
};

static const char * const mt6797_md1_observer_clock_names[] = {
	"ccif-ap",
	"ccif-md",
	"cldma",
};

static int
mt6797_md1_observer_powered(struct mt6797_md1_observer *observer)
{
	unsigned int status;
	unsigned int status_2nd;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < MT6797_MD1_POWER_SAMPLES; attempt++) {
		ret = regmap_read(observer->spm, MT6797_MD1_SPM_PWR_STATUS,
				  &status);
		if (ret)
			return ret;
		ret = regmap_read(observer->spm,
				  MT6797_MD1_SPM_PWR_STATUS_2ND, &status_2nd);
		if (ret)
			return ret;

		if (!!(status & MT6797_MD1_POWER_STATUS) ==
		    !!(status_2nd & MT6797_MD1_POWER_STATUS))
			return status & MT6797_MD1_POWER_STATUS ? 1 : 0;

		cpu_relax();
	}

	return -EAGAIN;
}

static int mt6797_md1_observer_enable_clock(void *context, unsigned int index)
{
	struct mt6797_md1_observer *observer = context;
	int ret;

	ret = clk_prepare_enable(observer->clocks[index]);
	if (ret)
		dev_err(observer->dev, "failed to enable %s clock: %d\n",
			mt6797_md1_observer_clock_names[index], ret);
	return ret;
}

static void mt6797_md1_observer_disable_clock(void *context,
					      unsigned int index)
{
	struct mt6797_md1_observer *observer = context;

	clk_disable_unprepare(observer->clocks[index]);
}

static int md1_clocks_gated(struct mt6797_md1_observer *observer, bool gated)
{
	unsigned int value;
	int ret;

	ret = regmap_read(observer->infracfg, MT6797_MD1_INFRA_CG_STA1,
			  &value);
	if (ret)
		return ret;

	if (gated)
		return (value & MT6797_MD1_INFRA_CLOCK_GATES) ==
		       MT6797_MD1_INFRA_CLOCK_GATES ? 0 : -EBUSY;

	return value & MT6797_MD1_INFRA_CLOCK_GATES ? -EIO : 0;
}

static int mt6797_md1_observer_take_snapshot(void *context,
					     unsigned int index,
					     struct mt6797_md1_snapshot *snapshot)
{
	struct mt6797_md1_observer *observer = context;
	unsigned int i;
	int ret;

	(void)index;
	ret = md1_clocks_gated(observer, false);
	if (ret) {
		dev_err(observer->dev,
			"clock gates are not all open at snapshot %u: %d\n",
			index, ret);
		return ret;
	}

	for (i = 0; i < MT6797_MD1_CLDMA_QUEUES; i++) {
		snapshot->tx_start[i] =
			readl(observer->regs[MT6797_MD1_OBSERVER_AP_PDN] +
			      MT6797_MD1_CLDMA_UL_START + i * 4);
		snapshot->rx_start[i] =
			readl(observer->regs[MT6797_MD1_OBSERVER_AP_AO] +
			      MT6797_MD1_CLDMA_DL_START + i * 4);
	}

	snapshot->tx_active =
		readl(observer->regs[MT6797_MD1_OBSERVER_AP_PDN] +
		      MT6797_MD1_CLDMA_UL_STATUS);
	snapshot->rx_active =
		readl(observer->regs[MT6797_MD1_OBSERVER_AP_AO] +
		      MT6797_MD1_CLDMA_DL_STATUS);
	snapshot->ccif_busy =
		readl(observer->regs[MT6797_MD1_OBSERVER_AP_CCIF] +
		      MT6797_MD1_CCIF_BUSY);
	snapshot->ccif_pending =
		readl(observer->regs[MT6797_MD1_OBSERVER_AP_CCIF] +
		      MT6797_MD1_CCIF_RCHNUM);

	return 0;
}

static int mt6797_md1_observer_verify_unwind(void *context)
{
	struct mt6797_md1_observer *observer = context;
	int ret;

	ret = md1_clocks_gated(observer, true);
	if (ret)
		dev_err(observer->dev,
			"clock gates did not close after reverse unwind: %d\n",
			ret);
	return ret;
}

static const struct mt6797_md1_clocked_ops mt6797_md1_observer_ops = {
	.enable_clock = mt6797_md1_observer_enable_clock,
	.disable_clock = mt6797_md1_observer_disable_clock,
	.take_snapshot = mt6797_md1_observer_take_snapshot,
	.verify_unwind = mt6797_md1_observer_verify_unwind,
};

static int mt6797_md1_clocked_observer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_md1_observer *observer;
	enum mt6797_md1_inherited_state state;
	unsigned int i;
	int powered;
	int ret;

	observer = devm_kzalloc(dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;
	observer->dev = dev;

	observer->spm =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(observer->spm))
		return dev_err_probe(dev, PTR_ERR(observer->spm),
				     "failed to acquire SPM syscon\n");
	observer->infracfg =
		syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(observer->infracfg))
		return dev_err_probe(dev, PTR_ERR(observer->infracfg),
				     "failed to acquire infracfg syscon\n");

	powered = mt6797_md1_observer_powered(observer);
	if (powered < 0)
		return dev_err_probe(dev, powered,
				     "MD1 SPM power status did not stabilize\n");
	if (!powered) {
		dev_info(dev,
			 "MD1_CLOCKED_OBSERVER state=power-off result=stop clocks=untouched\n");
		return -ENODEV;
	}

	ret = md1_clocks_gated(observer, true);
	if (ret)
		return dev_err_probe(dev, ret,
				     "MD1 clocks were not gated before observation\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_observer_reg_names); i++) {
		const char *name = mt6797_md1_observer_reg_names[i];

		observer->regs[i] =
			devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(observer->regs[i]))
			return PTR_ERR(observer->regs[i]);
	}

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_observer_clock_names); i++) {
		const char *name = mt6797_md1_observer_clock_names[i];

		observer->clocks[i] = devm_clk_get(dev, name);
		if (IS_ERR(observer->clocks[i]))
			return dev_err_probe(dev, PTR_ERR(observer->clocks[i]),
					     "failed to acquire %s clock\n",
					     name);
	}

	ret = mt6797_md1_clocked_observe(&mt6797_md1_observer_ops, observer, &state);
	if (ret)
		return dev_err_probe(dev, ret,
				     "clocked observation or unwind failed\n");

	if (state != MT6797_MD1_INHERITED_QUIESCENT) {
		dev_err(dev,
			"MD1_CLOCKED_OBSERVER state=%s result=reject clocks=unwound\n",
			mt6797_md1_clocked_state_name(state));
		return -EBUSY;
	}

	dev_info(dev,
		 "MD1_CLOCKED_OBSERVER state=%s result=pass clocks=unwound\n",
		 mt6797_md1_clocked_state_name(state));
	return 0;
}

static const struct of_device_id mt6797_md1_clocked_observer_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-clocked-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_clocked_observer_of_match);

static struct platform_driver mt6797_md1_clocked_observer_driver = {
	.probe = mt6797_md1_clocked_observer_probe,
	.driver = {
		.name = "mt6797-md1-clocked-observer",
		.of_match_table = mt6797_md1_clocked_observer_of_match,
	},
};
module_platform_driver(mt6797_md1_clocked_observer_driver);

MODULE_DESCRIPTION("MediaTek MT6797 MD1 clocked ownership observer");
MODULE_LICENSE("GPL");
