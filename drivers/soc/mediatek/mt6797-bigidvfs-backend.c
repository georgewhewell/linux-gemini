// SPDX-License-Identifier: GPL-2.0-only
/*
 * Disabled-only MT6797 BigiDVFS secure-register readback transport.
 *
 * The retained firmware audit proves only REG_READ (0xc200035f) for this
 * boundary. Keep the exact whitelist and raw sample separate from the future
 * calibrated state owner; advertised getter FIDs are not assumed present.
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>

struct mt6797_bigidvfs_backend {
	struct mutex operation_lock;
	bool faulted;
	u64 sample_generation;
};

static bool mt6797_bigidvfs_address_allowed(u32 address)
{
	switch (address) {
	case MT6797_BIGIDVFS_PLL_PCW:
	case MT6797_BIGIDVFS_PLL_ENABLE_POSDIV:
	case MT6797_BIGIDVFS_SRAM_SELECTOR:
	case MT6797_BIGIDVFS_CONTROL:
		return true;
	default:
		return false;
	}
}

static int mt6797_bigidvfs_secure_read(u32 address, u32 *value)
{
	struct arm_smccc_res result;

	if (!value || !mt6797_bigidvfs_address_allowed(address))
		return -EINVAL;

	arm_smccc_smc(MT6797_BIGIDVFS_FID_READ, address, 0, 0, 0, 0, 0, 0,
		      &result);
	/* REG_READ returns a zero-extended raw word; reject all error forms. */
	if (result.a0 >> 32)
		return -EIO;

	*value = (u32)result.a0;
	return 0;
}

static void mt6797_bigidvfs_mark_fault(
	struct mt6797_bigidvfs_backend *backend)
{
	backend->faulted = true;
}

int mt6797_bigidvfs_backend_read(
	struct device *dev, struct mt6797_bigidvfs_readback *readback)
{
	struct mt6797_bigidvfs_backend *backend;
	int ret;

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

	ret = mt6797_bigidvfs_secure_read(MT6797_BIGIDVFS_PLL_PCW,
					  &readback->pll_pcw);
	if (ret)
		goto fault;
	ret = mt6797_bigidvfs_secure_read(MT6797_BIGIDVFS_PLL_ENABLE_POSDIV,
					  &readback->pll_enable_posdiv);
	if (ret)
		goto fault;
	ret = mt6797_bigidvfs_secure_read(MT6797_BIGIDVFS_SRAM_SELECTOR,
					  &readback->sram_selector);
	if (ret)
		goto fault;
	ret = mt6797_bigidvfs_secure_read(MT6797_BIGIDVFS_CONTROL,
					  &readback->control);
	if (ret)
		goto fault;

	if (++backend->sample_generation == 0)
		backend->sample_generation = 1;
	readback->abi = MT6797_BIGIDVFS_BACKEND_ABI;
	readback->reserved = 0;
	readback->sample_generation = backend->sample_generation;
	goto out_unlock;

fault:
	mt6797_bigidvfs_mark_fault(backend);
out_unlock:
	mutex_unlock(&backend->operation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bigidvfs_backend_read);

static int mt6797_bigidvfs_backend_probe(struct platform_device *pdev)
{
	struct mt6797_bigidvfs_backend *backend;
	const char *method;

	if (!pdev->dev.of_node ||
	    of_property_read_string(pdev->dev.of_node, "method", &method) ||
	    strcmp(method, "smc"))
		return -EINVAL;

	backend = devm_kzalloc(&pdev->dev, sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;

	mutex_init(&backend->operation_lock);
	platform_set_drvdata(pdev, backend);
	dev_info(&pdev->dev,
		 "secure readback transport ready; owner unregistered\n");

	return 0;
}

static const struct of_device_id mt6797_bigidvfs_backend_of_match[] = {
	{ .compatible = "mediatek,mt6797-bigidvfs-backend" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_bigidvfs_backend_of_match);

static struct platform_driver mt6797_bigidvfs_backend_driver = {
	.probe = mt6797_bigidvfs_backend_probe,
	.driver = {
		.name = "mt6797-bigidvfs-backend",
		.of_match_table = mt6797_bigidvfs_backend_of_match,
	},
};
module_platform_driver(mt6797_bigidvfs_backend_driver);

MODULE_DESCRIPTION("Disabled MT6797 BigiDVFS secure readback transport");
MODULE_LICENSE("GPL");
