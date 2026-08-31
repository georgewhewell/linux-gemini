// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot MT6797 protected-readback runtime observer.
 *
 * This candidate-only observer waits for both read-only transports, performs
 * one call to each, and publishes their complete raw records. It is not a
 * state owner and does not classify or authorize an A72 transition.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

#define MT6797_READBACK_TAG "GEMINI_PROTECTED_READBACK_V1"

static struct platform_device *
mt6797_readback_get_backend(struct device *dev, const char *property)
{
	struct platform_device *backend;
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	backend = of_find_device_by_node(node);
	of_node_put(node);
	if (!backend)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_is_bound(&backend->dev)) {
		put_device(&backend->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return backend;
}

static void
mt6797_readback_log_clock(struct device *dev, int ret,
			  const struct mt6797_dvfsp_clock_readback *record)
{
	dev_info(dev,
		 MT6797_READBACK_TAG
		 " clock ret=%d abi=%u generation=%llu"
		 " muxsel=0x%08x ckdiv=0x%08x"
		 " pll_ll=0x%08x,0x%08x,0x%08x"
		 " pll_l=0x%08x,0x%08x,0x%08x"
		 " pll_cci=0x%08x,0x%08x,0x%08x"
		 " cspm_swctrl=0x%08x,0x%08x,0x%08x"
		 " cspm_hwsta=0x%08x,0x%08x,0x%08x,0x%08x\n",
		 ret, record->abi,
		 (unsigned long long)record->sample_generation,
		 record->armplldiv_muxsel, record->armplldiv_ckdiv,
		 record->pll_ll[0], record->pll_ll[1], record->pll_ll[2],
		 record->pll_l[0], record->pll_l[1], record->pll_l[2],
		 record->pll_cci[0], record->pll_cci[1], record->pll_cci[2],
		 record->cspm_swctrl[0], record->cspm_swctrl[1],
		 record->cspm_swctrl[2], record->cspm_hwsta[0],
		 record->cspm_hwsta[1], record->cspm_hwsta[2],
		 record->cspm_hwsta[3]);
}

#ifndef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_RAW_ENTRY_LEDGER
static void
mt6797_readback_log_bigidvfs(struct device *dev, int ret,
			     const struct mt6797_bigidvfs_readback *record)
{
	dev_info(dev,
		 MT6797_READBACK_TAG
		 " bigidvfs ret=%d abi=%u generation=%llu"
		 " pll_pcw=0x%08x pll_enable_posdiv=0x%08x"
		 " sram_selector=0x%08x control=0x%08x\n",
		 ret, record->abi,
		 (unsigned long long)record->sample_generation,
		 record->pll_pcw, record->pll_enable_posdiv,
		 record->sram_selector, record->control);
}
#endif

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_RAW_ENTRY_LEDGER
static int mt6797_readback_observer_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_clock_readback clock = { };
	struct platform_device *clock_backend;
	struct device *dev = &pdev->dev;
	int clock_ret;
	int ret;

	clock_backend = mt6797_readback_get_backend(dev,
						    "mediatek,clock-backend");
	if (IS_ERR(clock_backend))
		return dev_err_probe(dev, PTR_ERR(clock_backend),
				     "clock backend unavailable\n");
	if (!gemini_protected_readback_ledger_checkpoint(0)) {
		ret = dev_err_probe(dev, -EIO,
				    "before-clock ledger checkpoint failed\n");
		goto put_clock;
	}

	clock_ret = mt6797_dvfsp_clock_backend_read(&clock_backend->dev,
						    &clock);
	if (!gemini_protected_readback_ledger_checkpoint(1)) {
		ret = dev_err_probe(dev, -EIO,
				    "after-clock ledger checkpoint failed\n");
		goto put_clock;
	}

	mt6797_readback_log_clock(dev, clock_ret, &clock);
	dev_info(dev,
		 MT6797_READBACK_TAG
		 " state=complete attempts=1 clock_calls=1 bigidvfs_calls=0"
		 " cpu_requests=0 owner_registration=0\n");
	ret = 0;

put_clock:
	put_device(&clock_backend->dev);
	return ret;
}
#else
static int mt6797_readback_observer_probe(struct platform_device *pdev)
{
	struct mt6797_dvfsp_clock_readback clock = { };
	struct mt6797_bigidvfs_readback bigidvfs = { };
	struct platform_device *clock_backend;
	struct platform_device *bigidvfs_backend;
	struct device *dev = &pdev->dev;
	int clock_ret;
	int bigidvfs_ret;
	int ret;

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER
	if (!gemini_protected_readback_ledger_checkpoint(0))
		return dev_err_probe(dev, -EIO,
				     "probe-enter ledger checkpoint failed\n");
#endif

	clock_backend = mt6797_readback_get_backend(dev,
						    "mediatek,clock-backend");
	if (IS_ERR(clock_backend))
		return dev_err_probe(dev, PTR_ERR(clock_backend),
				     "clock backend unavailable\n");
	bigidvfs_backend = mt6797_readback_get_backend(dev,
						       "mediatek,bigidvfs-backend");
	if (IS_ERR(bigidvfs_backend)) {
		ret = dev_err_probe(dev, PTR_ERR(bigidvfs_backend),
				    "BigiDVFS backend unavailable\n");
		goto put_clock;
	}

#ifdef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER
	if (!gemini_protected_readback_ledger_checkpoint(1)) {
		ret = dev_err_probe(dev, -EIO,
				    "gate-passed ledger checkpoint failed\n");
		goto put_bigidvfs;
	}
#else
	if (!gemini_protected_readback_ledger_checkpoint(0)) {
		ret = dev_err_probe(dev, -EIO,
				    "before-clock ledger checkpoint failed\n");
		goto put_bigidvfs;
	}
#endif
	clock_ret = mt6797_dvfsp_clock_backend_read(&clock_backend->dev,
						    &clock);
#ifndef CONFIG_PSTORE_GEMINI_PROTECTED_READBACK_PROBE_GATE_LEDGER
	if (!gemini_protected_readback_ledger_checkpoint(1)) {
		ret = dev_err_probe(dev, -EIO,
				    "after-clock ledger checkpoint failed\n");
		goto put_bigidvfs;
	}
#endif
	bigidvfs_ret = mt6797_bigidvfs_backend_read(&bigidvfs_backend->dev,
						    &bigidvfs);
	mt6797_readback_log_clock(dev, clock_ret, &clock);
	mt6797_readback_log_bigidvfs(dev, bigidvfs_ret, &bigidvfs);
	dev_info(dev,
		 MT6797_READBACK_TAG
		 " state=complete attempts=1 clock_calls=1 bigidvfs_calls=1"
		 " cpu_requests=0 owner_registration=0\n");
	ret = 0;

put_bigidvfs:
	put_device(&bigidvfs_backend->dev);
put_clock:
	put_device(&clock_backend->dev);
	return ret;
}
#endif

static const struct of_device_id mt6797_readback_observer_of_match[] = {
	{ .compatible = "mediatek,mt6797-protected-readback-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_readback_observer_of_match);

static struct platform_driver mt6797_readback_observer_driver = {
	.probe = mt6797_readback_observer_probe,
	.driver = {
		.name = "mt6797-protected-readback-observer",
		.of_match_table = mt6797_readback_observer_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_readback_observer_driver);

MODULE_DESCRIPTION("MT6797 one-shot protected-readback observer");
MODULE_LICENSE("GPL");
