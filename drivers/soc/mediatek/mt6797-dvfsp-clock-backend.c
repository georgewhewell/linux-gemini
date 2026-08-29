// SPDX-License-Identifier: GPL-2.0-only
/*
 * Disabled-only MT6797 MCUMIXED/DVFSP clock-window readback transport.
 *
 * The CPU PLL and divider window is shared by Linux, SPM, and ATF. Keep the
 * semaphore protocol here separate from the eventual calibrated state owner:
 * this file can read raw words, but cannot make OPP or voltage decisions.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

#define MT6797_DVFSP_CSPM_POWERON_EN		0x000
#define MT6797_DVFSP_SEMAPHORE			0x440
#define MT6797_DVFSP_SEMAPHORE_REQUEST		1
#define MT6797_DVFSP_SEMAPHORE_HELD		BIT(0)
#define MT6797_DVFSP_SEMAPHORE_POLL_US		10
#define MT6797_DVFSP_SEMAPHORE_RETRIES		200
#define MT6797_DVFSP_CSPM_POWERON_VALUE		0x0b160001

#define MT6797_ARMPLL_LL			0x200
#define MT6797_ARMPLL_L			0x210
#define MT6797_ARMPLL_CCI			0x220
#define MT6797_ARMPLL_WORDS			3
#define MT6797_ARMPLLDIV_MUXSEL			0x270
#define MT6797_ARMPLLDIV_CKDIV			0x274

/* Vendor mt_cpufreq_hybrid.c maps these CSPM words to live DVFS state. */
static const u32 mt6797_dvfsp_cspm_swctrl_offset[3] = {
	0x608, 0x60c, 0x610,
};
static const u32 mt6797_dvfsp_cspm_hwsta_offset[4] = {
	0x614, 0x618, 0x61c, 0x620,
};

struct mt6797_dvfsp_clock_backend {
	void __iomem *mcumixed;
	void __iomem *cspm;
	struct clk *i2c_clk;
	struct mutex operation_lock;
	spinlock_t semaphore_lock;
	bool faulted;
	u64 sample_generation;
};

static int
mt6797_dvfsp_clock_semaphore_acquire(struct mt6797_dvfsp_clock_backend *backend)
{
	unsigned int i;

	for (i = 0; i < MT6797_DVFSP_SEMAPHORE_RETRIES; i++) {
		writel(MT6797_DVFSP_SEMAPHORE_REQUEST,
		       backend->cspm + MT6797_DVFSP_SEMAPHORE);
		if (readl(backend->cspm + MT6797_DVFSP_SEMAPHORE) &
		    MT6797_DVFSP_SEMAPHORE_HELD)
			return 0;
		udelay(MT6797_DVFSP_SEMAPHORE_POLL_US);
	}

	return -ETIMEDOUT;
}

static int
mt6797_dvfsp_clock_semaphore_release(struct mt6797_dvfsp_clock_backend *backend)
{
	unsigned int i;

	for (i = 0; i < MT6797_DVFSP_SEMAPHORE_RETRIES; i++) {
		writel(MT6797_DVFSP_SEMAPHORE_REQUEST,
		       backend->cspm + MT6797_DVFSP_SEMAPHORE);
		if (!(readl(backend->cspm + MT6797_DVFSP_SEMAPHORE) &
		      MT6797_DVFSP_SEMAPHORE_HELD))
			return 0;
		udelay(MT6797_DVFSP_SEMAPHORE_POLL_US);
	}

	return -ETIMEDOUT;
}

static void
mt6797_dvfsp_clock_mark_fault(struct mt6797_dvfsp_clock_backend *backend)
{
	backend->faulted = true;
}

int mt6797_dvfsp_clock_backend_read(
	struct device *dev, struct mt6797_dvfsp_clock_readback *readback)
{
	struct mt6797_dvfsp_clock_backend *backend;
	unsigned long flags;
	unsigned int i;
	int ret;
	int release_ret;

	if (!dev || !readback)
		return -EINVAL;

	backend = dev_get_drvdata(dev);
	if (!backend)
		return -ENODEV;

	mutex_lock(&backend->operation_lock);
	if (backend->faulted) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = clk_prepare_enable(backend->i2c_clk);
	if (ret)
		goto out_unlock;

	local_irq_save(flags);
	spin_lock(&backend->semaphore_lock);

	/* The historical owner enables the CSPM internal clock first. */
	writel(MT6797_DVFSP_CSPM_POWERON_VALUE,
	       backend->cspm + MT6797_DVFSP_CSPM_POWERON_EN);
	readl(backend->cspm + MT6797_DVFSP_CSPM_POWERON_EN);

	ret = mt6797_dvfsp_clock_semaphore_acquire(backend);
	if (ret) {
		mt6797_dvfsp_clock_mark_fault(backend);
		goto out_spin;
	}

	memset(readback, 0, sizeof(*readback));
	readback->abi = MT6797_DVFSP_CLOCK_BACKEND_ABI;
	readback->sample_generation = ++backend->sample_generation;
	readback->armplldiv_muxsel = readl(backend->mcumixed +
						MT6797_ARMPLLDIV_MUXSEL);
	readback->armplldiv_ckdiv = readl(backend->mcumixed +
						MT6797_ARMPLLDIV_CKDIV);
	for (i = 0; i < MT6797_ARMPLL_WORDS; i++) {
		readback->pll_ll[i] = readl(backend->mcumixed +
					   MT6797_ARMPLL_LL + i * sizeof(u32));
		readback->pll_l[i] = readl(backend->mcumixed +
					  MT6797_ARMPLL_L + i * sizeof(u32));
		readback->pll_cci[i] = readl(backend->mcumixed +
					   MT6797_ARMPLL_CCI + i * sizeof(u32));
	}

	for (i = 0; i < 3; i++)
		readback->cspm_swctrl[i] = readl(backend->cspm +
						 mt6797_dvfsp_cspm_swctrl_offset[i]);
	for (i = 0; i < 4; i++)
		readback->cspm_hwsta[i] = readl(backend->cspm +
						 mt6797_dvfsp_cspm_hwsta_offset[i]);
	release_ret = mt6797_dvfsp_clock_semaphore_release(backend);
	if (release_ret) {
		mt6797_dvfsp_clock_mark_fault(backend);
		ret = release_ret;
	}

out_spin:
	spin_unlock(&backend->semaphore_lock);
	local_irq_restore(flags);
	clk_disable_unprepare(backend->i2c_clk);

out_unlock:
	mutex_unlock(&backend->operation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_clock_backend_read);

static int mt6797_dvfsp_clock_backend_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_clock_backend *backend;
	int ret;

	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;

	backend->mcumixed = devm_platform_ioremap_resource_byname(
		pdev, "mcumixed");
	if (IS_ERR(backend->mcumixed))
		return PTR_ERR(backend->mcumixed);

	backend->cspm = devm_platform_ioremap_resource_byname(pdev, "cspm");
	if (IS_ERR(backend->cspm))
		return PTR_ERR(backend->cspm);

	backend->i2c_clk = devm_clk_get(&pdev->dev, "i2c");
	if (IS_ERR(backend->i2c_clk))
		return PTR_ERR(backend->i2c_clk);

	mutex_init(&backend->operation_lock);
	spin_lock_init(&backend->semaphore_lock);
	platform_set_drvdata(pdev, backend);
	dev_info(&pdev->dev,
		 "protected clock readback transport ready; owner unregistered\n");

	ret = 0;
	return ret;
}

static const struct of_device_id mt6797_dvfsp_clock_backend_of_match[] = {
	{ .compatible = "mediatek,mt6797-dvfsp-clock-backend" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_dvfsp_clock_backend_of_match);

static struct platform_driver mt6797_dvfsp_clock_backend_driver = {
	.probe = mt6797_dvfsp_clock_backend_probe,
	.driver = {
		.name = "mt6797-dvfsp-clock-backend",
		.of_match_table = mt6797_dvfsp_clock_backend_of_match,
	},
};
module_platform_driver(mt6797_dvfsp_clock_backend_driver);

MODULE_DESCRIPTION("MT6797 protected CPU clock readback transport");
MODULE_LICENSE("GPL");
