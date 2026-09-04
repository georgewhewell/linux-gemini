// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 BigiDVFS secure SRAM regulator. */

#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>

#include "mt6797-bigidvfs-sram.h"

#define MT6797_BIGIDVFS_FID_SRAM_SET	0xc20003bfUL
#define MT6797_BIGIDVFS_FID_READ		0xc200035fUL
#define MT6797_BIGIDVFS_SRAM_SELECTOR	0x102222b0U
#define MT6797_BIGIDVFS_SRAM_VSEL_MASK	0xf

struct mt6797_bigidvfs_sram {
	struct device *dev;
	/* Serialize every read/set/read transaction issued by this provider. */
	struct mutex lock;
	bool faulted;
};

static const unsigned int mt6797_bigidvfs_sram_voltages[] = {
	1050000, 600000, 700000, 900000,
	925000, 950000, 975000, 1000000,
	1025000, 1050000, 1075000, 1100000,
	1125000, 1150000, 1175000, 1200000,
};

static int mt6797_bigidvfs_sram_secure_read(unsigned int *selector)
{
	struct arm_smccc_res result;
	u32 raw;

	if (!selector)
		return -EINVAL;

	arm_smccc_smc(MT6797_BIGIDVFS_FID_READ,
		      MT6797_BIGIDVFS_SRAM_SELECTOR, 0, 0, 0, 0, 0, 0,
		      &result);
	raw = result.a0;
	if (raw == U32_MAX)
		return -EOPNOTSUPP;
	if (result.a0 >> 32)
		return result.a0 == SMCCC_RET_NOT_SUPPORTED ? -EOPNOTSUPP : -EIO;

	*selector = raw & MT6797_BIGIDVFS_SRAM_VSEL_MASK;
	return 0;
}

static int mt6797_bigidvfs_sram_secure_set(unsigned int firmware_uv_div10)
{
	struct arm_smccc_res result;
	s32 status;

	arm_smccc_smc(MT6797_BIGIDVFS_FID_SRAM_SET, firmware_uv_div10,
		      0, 0, 0, 0, 0, 0, &result);
	status = result.a0;
	if (status < 0)
		return status == SMCCC_RET_NOT_SUPPORTED ? -EOPNOTSUPP : -EIO;
	if (result.a0 >> 32)
		return -EIO;

	return 0;
}

static int mt6797_bigidvfs_sram_io_read(void *context,
					unsigned int *selector)
{
	if (!context)
		return -EINVAL;

	return mt6797_bigidvfs_sram_secure_read(selector);
}

static int mt6797_bigidvfs_sram_io_set(void *context,
				       unsigned int firmware_uv_div10)
{
	if (!context)
		return -EINVAL;

	return mt6797_bigidvfs_sram_secure_set(firmware_uv_div10);
}

static const struct mt6797_sram_io mt6797_bigidvfs_sram_io = {
	.read_selector = mt6797_bigidvfs_sram_io_read,
	.set_voltage = mt6797_bigidvfs_sram_io_set,
};

int mt6797_sram_set_transaction(const struct mt6797_sram_io *io,
				void *context, const unsigned int *voltages,
				unsigned int count, unsigned int selector)
{
	unsigned int before;
	unsigned int observed;
	int primary_ret;
	int ret;

	if (!io || !io->read_selector || !io->set_voltage || !voltages ||
	    count != MT6797_BIGIDVFS_SRAM_SELECTOR_COUNT || selector >= count)
		return -EINVAL;

	ret = io->read_selector(context, &before);
	if (ret)
		return ret;
	if (before >= count)
		return -ERANGE;
	if (before == selector)
		return 0;

	ret = io->set_voltage(context, voltages[selector] / 10);
	if (ret == -EOPNOTSUPP)
		return ret;
	if (ret) {
		primary_ret = ret;
		goto rollback;
	}

	ret = io->read_selector(context, &observed);
	if (!ret && observed == selector)
		return 0;

	primary_ret = ret ?: -EIO;

rollback:
	ret = io->set_voltage(context, voltages[before] / 10);
	if (ret)
		return -EUCLEAN;

	ret = io->read_selector(context, &observed);
	if (ret || observed != before)
		return -EUCLEAN;

	return primary_ret;
}

static int mt6797_bigidvfs_sram_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mt6797_bigidvfs_sram *sram = rdev_get_drvdata(rdev);
	unsigned int selector;
	int ret;

	mutex_lock(&sram->lock);
	if (sram->faulted) {
		ret = -EUCLEAN;
		goto out_unlock;
	}
	ret = mt6797_bigidvfs_sram_secure_read(&selector);
out_unlock:
	mutex_unlock(&sram->lock);
	if (ret)
		return ret;

	return selector;
}

static int mt6797_sram_set_voltage_sel(struct regulator_dev *rdev,
				       unsigned int selector)
{
	struct mt6797_bigidvfs_sram *sram = rdev_get_drvdata(rdev);
	const struct mt6797_sram_io *io = &mt6797_bigidvfs_sram_io;
	const unsigned int *voltages = mt6797_bigidvfs_sram_voltages;
	unsigned int count = ARRAY_SIZE(mt6797_bigidvfs_sram_voltages);
	int ret;

	mutex_lock(&sram->lock);
	if (sram->faulted) {
		ret = -EUCLEAN;
		goto out_unlock;
	}
	ret = mt6797_sram_set_transaction(io, sram, voltages, count, selector);
	if (ret == -EUCLEAN)
		sram->faulted = true;
out_unlock:
	mutex_unlock(&sram->lock);

	return ret;
}

static int mt6797_bigidvfs_sram_status_unsupported(struct regulator_dev *rdev)
{
	/*
	 * The deployed ATF's 0xc2000380 handler returns success without touching
	 * hardware, and the pinned vendor disable function is a no-op.  The
	 * selector at 0x102222b0 is therefore not promoted to lifecycle state.
	 */
	return -EOPNOTSUPP;
}

static int
mt6797_bigidvfs_sram_map_voltage(struct regulator_dev *rdev, int min_uv,
				 int max_uv)
{
	unsigned int selector;

	/* Selector 0 is readable but the firmware setter cannot recreate it. */
	for (selector = 1;
	     selector < ARRAY_SIZE(mt6797_bigidvfs_sram_voltages); selector++)
		if (mt6797_bigidvfs_sram_voltages[selector] >= min_uv &&
		    mt6797_bigidvfs_sram_voltages[selector] <= max_uv)
			return selector;

	return -EINVAL;
}

static const struct regulator_ops mt6797_bigidvfs_sram_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = mt6797_bigidvfs_sram_map_voltage,
	.get_voltage_sel = mt6797_bigidvfs_sram_get_voltage_sel,
	.set_voltage_sel = mt6797_sram_set_voltage_sel,
	.is_enabled = mt6797_bigidvfs_sram_status_unsupported,
	.enable = mt6797_bigidvfs_sram_status_unsupported,
	.disable = mt6797_bigidvfs_sram_status_unsupported,
};

static const struct regulator_desc mt6797_bigidvfs_sram_desc = {
	.name = "mt6797-bigidvfs-sram",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &mt6797_bigidvfs_sram_ops,
	.n_voltages = ARRAY_SIZE(mt6797_bigidvfs_sram_voltages),
	.volt_table = mt6797_bigidvfs_sram_voltages,
};

static int mt6797_bigidvfs_sram_probe(struct platform_device *pdev)
{
	struct regulator_config config = { };
	struct mt6797_bigidvfs_sram *sram;
	struct regulator_dev *rdev;

	sram = devm_kzalloc(&pdev->dev, sizeof(*sram), GFP_KERNEL);
	if (!sram)
		return -ENOMEM;

	sram->dev = &pdev->dev;
	mutex_init(&sram->lock);
	platform_set_drvdata(pdev, sram);

	config.dev = &pdev->dev;
	config.driver_data = sram;
	config.of_node = pdev->dev.of_node;
	rdev = devm_regulator_register(&pdev->dev,
				       &mt6797_bigidvfs_sram_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "failed to register SRAM regulator\n");

	dev_info(&pdev->dev,
		 "voltage read/set ready; enable, status, and disable unsupported\n");
	return 0;
}

static const struct of_device_id mt6797_bigidvfs_sram_of_match[] = {
	{ .compatible = "mediatek,mt6797-bigidvfs-sram" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_bigidvfs_sram_of_match);

static struct platform_driver mt6797_bigidvfs_sram_driver = {
	.probe = mt6797_bigidvfs_sram_probe,
	.driver = {
		.name = "mt6797-bigidvfs-sram",
		.of_match_table = mt6797_bigidvfs_sram_of_match,
	},
};
module_platform_driver(mt6797_bigidvfs_sram_driver);

MODULE_DESCRIPTION("MediaTek MT6797 BigiDVFS SRAM regulator");
MODULE_LICENSE("GPL");
