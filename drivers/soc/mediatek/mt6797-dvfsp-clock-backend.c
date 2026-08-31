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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pstore_ram.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

#include "mt6797-protected-readback-internal.h"

#define MT6797_DVFSP_CSPM_POWERON_EN		0x000
#define MT6797_DVFSP_SEMAPHORE			0x440
#define MT6797_DVFSP_SEMAPHORE_REQUEST		1
#define MT6797_DVFSP_SEMAPHORE_HELD		BIT(0)
#define MT6797_DVFSP_SEMAPHORE_POLL_US		10
#define MT6797_DVFSP_SEMAPHORE_RETRIES		200
#define MT6797_DVFSP_SEMAPHORE_SETTLE_NS	200
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

struct mt6797_dvfsp_clock_access {
	void __iomem *mcumixed;
	void __iomem *cspm;
};

struct mt6797_dvfsp_clock_backend {
	void __iomem *mcumixed;
	struct mt6797_dvfsp_handoff *handoff;
	struct clk *i2c_clk;
	struct mutex operation_lock;
	spinlock_t semaphore_lock;
	bool faulted;
	u64 sample_generation;
};

struct mt6797_dvfsp_clock_call {
	struct mt6797_dvfsp_clock_backend *backend;
	struct mt6797_dvfsp_clock_readback *readback;
	bool snapshot_attempted;
};

static void mt6797_clock_write(void *context, u32 window, u32 offset,
			       u32 value)
{
	struct mt6797_dvfsp_clock_access *access = context;
	void __iomem *base = access->cspm;

	if (window == MT6797_DVFSP_CLOCK_WINDOW_MCUMIXED)
		base = access->mcumixed;
	writel(value, base + offset);
}

static u32 mt6797_clock_read(void *context, u32 window, u32 offset)
{
	struct mt6797_dvfsp_clock_access *access = context;
	void __iomem *base = access->cspm;

	if (window == MT6797_DVFSP_CLOCK_WINDOW_MCUMIXED)
		base = access->mcumixed;
	return readl(base + offset);
}

static void mt6797_clock_delay_us(void *context, unsigned int usec)
{
	udelay(usec);
}

static void mt6797_clock_settle_ns(void *context, unsigned int nsec)
{
	ndelay(nsec);
}

static const struct mt6797_dvfsp_clock_transport_ops
mt6797_clock_ops = {
	.write = mt6797_clock_write,
	.read = mt6797_clock_read,
	.delay_us = mt6797_clock_delay_us,
	.settle_ns = mt6797_clock_settle_ns,
};

static int
mt6797_clock_acquire(const struct mt6797_dvfsp_clock_transport_ops *ops,
		     void *context)
{
	const u32 cspm = MT6797_DVFSP_CLOCK_WINDOW_CSPM;
	unsigned int i;

	for (i = 0; i < MT6797_DVFSP_SEMAPHORE_RETRIES; i++) {
		ops->write(context, cspm,
			   MT6797_DVFSP_SEMAPHORE,
			   MT6797_DVFSP_SEMAPHORE_REQUEST);
		if (ops->read(context, cspm,
			      MT6797_DVFSP_SEMAPHORE) &
		    MT6797_DVFSP_SEMAPHORE_HELD)
			return 0;
		ops->delay_us(context, MT6797_DVFSP_SEMAPHORE_POLL_US);
	}

	return -ETIMEDOUT;
}

static int
mt6797_clock_release(const struct mt6797_dvfsp_clock_transport_ops *ops,
		     void *context)
{
	const u32 cspm = MT6797_DVFSP_CLOCK_WINDOW_CSPM;
	unsigned int i;
	u32 semaphore;

	for (i = 0; i < MT6797_DVFSP_SEMAPHORE_RETRIES; i++) {
		ops->write(context, cspm,
			   MT6797_DVFSP_SEMAPHORE,
			   MT6797_DVFSP_SEMAPHORE_REQUEST);
		semaphore = ops->read(context, cspm,
				      MT6797_DVFSP_SEMAPHORE);
		if (!(semaphore & MT6797_DVFSP_SEMAPHORE_HELD))
			return 0;
		ops->delay_us(context, MT6797_DVFSP_SEMAPHORE_POLL_US);
	}

	return -ETIMEDOUT;
}

int mt6797_clock_snapshot(const struct mt6797_dvfsp_clock_transport_ops *ops,
			  void *context,
			  struct mt6797_dvfsp_clock_readback *readback)
{
	struct mt6797_dvfsp_clock_readback observed = { };
	const u32 cspm = MT6797_DVFSP_CLOCK_WINDOW_CSPM;
	const u32 mcumixed = MT6797_DVFSP_CLOCK_WINDOW_MCUMIXED;
	unsigned int i;
	int ret;

	if (!readback)
		return -EINVAL;
	memset(readback, 0, sizeof(*readback));
	if (!ops || !ops->write || !ops->read || !ops->delay_us ||
	    !ops->settle_ns)
		return -EINVAL;

	ops->write(context, cspm,
		   MT6797_DVFSP_CSPM_POWERON_EN,
		   MT6797_DVFSP_CSPM_POWERON_VALUE);
	ops->read(context, cspm,
		  MT6797_DVFSP_CSPM_POWERON_EN);

	ret = mt6797_clock_acquire(ops, context);
	if (ret)
		return ret;

	/* The recovered owner requires this boundary before MCUMIXED access. */
	ops->settle_ns(context, MT6797_DVFSP_SEMAPHORE_SETTLE_NS);
	observed.armplldiv_muxsel = ops->read(context, mcumixed,
					      MT6797_ARMPLLDIV_MUXSEL);
	observed.armplldiv_ckdiv = ops->read(context, mcumixed,
					     MT6797_ARMPLLDIV_CKDIV);
	for (i = 0; i < MT6797_ARMPLL_WORDS; i++) {
		observed.pll_ll[i] = ops->read(context, mcumixed,
			MT6797_ARMPLL_LL + i * sizeof(u32));
		observed.pll_l[i] = ops->read(context, mcumixed,
			MT6797_ARMPLL_L + i * sizeof(u32));
		observed.pll_cci[i] = ops->read(context, mcumixed,
			MT6797_ARMPLL_CCI + i * sizeof(u32));
	}

	for (i = 0; i < ARRAY_SIZE(mt6797_dvfsp_cspm_swctrl_offset); i++)
		observed.cspm_swctrl[i] = ops->read(context, cspm,
			mt6797_dvfsp_cspm_swctrl_offset[i]);
	for (i = 0; i < ARRAY_SIZE(mt6797_dvfsp_cspm_hwsta_offset); i++)
		observed.cspm_hwsta[i] = ops->read(context, cspm,
			mt6797_dvfsp_cspm_hwsta_offset[i]);

	ret = mt6797_clock_release(ops, context);
	if (ret)
		return ret;

	*readback = observed;
	return 0;
}

static int mt6797_dvfsp_clock_execute(void *context, void __iomem *cspm)
{
	struct mt6797_dvfsp_clock_call *call = context;
	struct mt6797_dvfsp_clock_backend *backend = call->backend;
	struct mt6797_dvfsp_clock_access access = {
		.mcumixed = backend->mcumixed,
		.cspm = cspm,
	};
	unsigned long flags;
	int ret;

	ret = clk_prepare_enable(backend->i2c_clk);
	if (ret)
		return ret;

	call->snapshot_attempted = true;
	local_irq_save(flags);
	spin_lock(&backend->semaphore_lock);
	ret = mt6797_clock_snapshot(&mt6797_clock_ops, &access,
				    call->readback);
	spin_unlock(&backend->semaphore_lock);
	local_irq_restore(flags);
	clk_disable_unprepare(backend->i2c_clk);

	return ret;
}

static void
mt6797_dvfsp_clock_mark_fault(struct mt6797_dvfsp_clock_backend *backend)
{
	backend->faulted = true;
}

int mt6797_dvfsp_clock_backend_read(struct device *dev,
				    struct mt6797_dvfsp_clock_readback *readback)
{
	struct mt6797_dvfsp_clock_readback observed = { };
	struct mt6797_dvfsp_clock_call call = { };
	struct mt6797_dvfsp_clock_backend *backend;
	int ret;

	if (!readback)
		return -EINVAL;
	memset(readback, 0, sizeof(*readback));
	if (!dev)
		return -EINVAL;

	backend = dev_get_drvdata(dev);
	if (!backend)
		return -ENODEV;

	mutex_lock(&backend->operation_lock);
	if (backend->faulted) {
		ret = -EIO;
		goto out_unlock;
	}

	call.backend = backend;
	call.readback = &observed;
	ret = mt6797_dvfsp_cspm_execute(backend->handoff,
					mt6797_dvfsp_clock_execute, &call);
	if (ret && call.snapshot_attempted)
		mt6797_dvfsp_clock_mark_fault(backend);
	if (ret)
		goto out_unlock;

	if (++backend->sample_generation == 0)
		backend->sample_generation = 1;
	observed.abi = MT6797_DVFSP_CLOCK_BACKEND_ABI;
	observed.sample_generation = backend->sample_generation;
	*readback = observed;

out_unlock:
	mutex_unlock(&backend->operation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_clock_backend_read);

static int mt6797_dvfsp_clock_backend_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_clock_backend *backend;
	int ret;

#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_ENTRY_LEDGER
	if (!gemini_protected_readback_ledger_checkpoint(1))
		return dev_err_probe(&pdev->dev, -EIO,
				     "probe-enter ledger checkpoint failed\n");
#endif
#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION
	dev_info(&pdev->dev,
		 "GEMINI_CLOCK_BACKEND_FIRST_DMESG_LIVE_V1 "
		 "stage=probe-enter writes=2 protected=0 bigidvfs=0 cpu=0\n");
#endif

	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;

	backend->mcumixed = devm_platform_ioremap_resource_byname(pdev, "mcumixed");
	if (IS_ERR(backend->mcumixed))
		return PTR_ERR(backend->mcumixed);

	backend->handoff = mt6797_dvfsp_handoff_get(&pdev->dev);
	if (IS_ERR(backend->handoff))
		return dev_err_probe(&pdev->dev, PTR_ERR(backend->handoff),
				     "CSPM handoff supplier unavailable\n");

	backend->i2c_clk = devm_clk_get(&pdev->dev, "i2c");
	if (IS_ERR(backend->i2c_clk))
		return PTR_ERR(backend->i2c_clk);

	mutex_init(&backend->operation_lock);
	spin_lock_init(&backend->semaphore_lock);
	platform_set_drvdata(pdev, backend);
	dev_info(&pdev->dev,
		 "protected clock readback transport ready; "
		 "CSPM owner=handoff; state owner unregistered\n");

#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION
	dev_info(&pdev->dev,
		 "GEMINI_CLOCK_BACKEND_FIRST_DMESG_LIVE_V1 "
		 "stage=probe-complete writes=2 protected=0 bigidvfs=0 cpu=0\n");
	dev_info(&pdev->dev, "%s %s\n",
		 "GEMINI_CLOCK_BACKEND_CSPM_COEXISTENCE_V1",
		 "state=ready cspm_owner=handoff protected=0 bigidvfs=0 cpu=0");
#endif
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

#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_ENTRY_LEDGER
static int __init mt6797_dvfsp_clock_backend_driver_init(void)
{
	if (!gemini_protected_readback_ledger_checkpoint(0))
		return -EIO;

#ifdef CONFIG_PSTORE_GEMINI_CLOCK_BACKEND_FIRST_DMESG_ENTRY_QUALIFICATION
	pr_info("GEMINI_CLOCK_BACKEND_FIRST_DMESG_LIVE_V1 "
		"stage=driver-init writes=1 protected=0 bigidvfs=0 cpu=0\n");
#endif

	return platform_driver_register(&mt6797_dvfsp_clock_backend_driver);
}
module_init(mt6797_dvfsp_clock_backend_driver_init);

static void __exit mt6797_dvfsp_clock_backend_driver_exit(void)
{
	platform_driver_unregister(&mt6797_dvfsp_clock_backend_driver);
}
module_exit(mt6797_dvfsp_clock_backend_driver_exit);
#else
module_platform_driver(mt6797_dvfsp_clock_backend_driver);
#endif

MODULE_DESCRIPTION("MT6797 protected CPU clock readback transport");
MODULE_LICENSE("GPL");
