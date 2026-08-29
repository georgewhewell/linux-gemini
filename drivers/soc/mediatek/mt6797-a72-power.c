// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MT6797 Cortex-A72 power resource observer
 *
 * Copyright (c) 2026 Julien Etienne
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>

#define SPM_POWERON_CONFIG_EN		0x218
#define SPM_EXT_BUCK_ISO		0x290

#define MCUCFG_MP2_SYNC_DCM		0x274
#define MCUCFG_BIG_ARMPLL_CON0		0x4a0

struct mt6797_a72_power {
	struct regulator *vproc_big;
	struct reset_control *pwrap_reset;
	struct regmap *spm;
	void __iomem *mcucfg;
	int vproc_enabled;
	int vproc_uv;
	u32 spm_poweron_config;
	u32 spm_ext_buck_iso;
	u32 mp2_sync_dcm;
	u32 big_armpll_con0;
	unsigned int cpus[2];
	cpumask_t cpu_mask;
	bool resources_ready;
};

static ssize_t ready_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(ready);

static ssize_t resources_ready_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mt6797_a72_power *power = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", power->resources_ready);
}
static DEVICE_ATTR_RO(resources_ready);

static ssize_t abi_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return sysfs_emit(buf, "observer-v1\n");
}
static DEVICE_ATTR_RO(abi);

static ssize_t hooks_armed_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(hooks_armed);

static ssize_t provider_mode_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "observe-only\n");
}
static DEVICE_ATTR_RO(provider_mode);

static ssize_t snapshot_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mt6797_a72_power *power = dev_get_drvdata(dev);
	ssize_t len = 0;

	len += sysfs_emit_at(buf, len, "cpus=%u,%u pwrap_reset_acquired=1\n",
			     power->cpus[0], power->cpus[1]);
	len += sysfs_emit_at(buf, len, "vproc_enabled=%d vproc_uv=%d\n",
			     power->vproc_enabled, power->vproc_uv);
	len += sysfs_emit_at(buf, len, "spm_poweron=0x%08x spm_ext_buck_iso=0x%08x\n",
			     power->spm_poweron_config,
			     power->spm_ext_buck_iso);
	len += sysfs_emit_at(buf, len, "mp2_sync_dcm=0x%08x big_armpll=0x%08x\n",
			     power->mp2_sync_dcm, power->big_armpll_con0);

	return len;
}
static DEVICE_ATTR_RO(snapshot);

static struct attribute *mt6797_a72_power_attrs[] = {
	&dev_attr_ready.attr,
	&dev_attr_resources_ready.attr,
	&dev_attr_abi.attr,
	&dev_attr_hooks_armed.attr,
	&dev_attr_provider_mode.attr,
	&dev_attr_snapshot.attr,
	NULL,
};

static const struct attribute_group mt6797_a72_power_group = {
	.attrs = mt6797_a72_power_attrs,
};

static int mt6797_a72_power_snapshot(struct mt6797_a72_power *power)
{
	int ret;

	power->vproc_enabled = regulator_is_enabled(power->vproc_big);
	if (power->vproc_enabled < 0)
		return power->vproc_enabled;

	power->vproc_uv = regulator_get_voltage(power->vproc_big);
	if (power->vproc_uv < 0)
		return power->vproc_uv;

	ret = regmap_read(power->spm, SPM_POWERON_CONFIG_EN,
			  &power->spm_poweron_config);
	if (ret)
		return ret;

	ret = regmap_read(power->spm, SPM_EXT_BUCK_ISO,
			  &power->spm_ext_buck_iso);
	if (ret)
		return ret;

	power->mp2_sync_dcm = readl(power->mcucfg + MCUCFG_MP2_SYNC_DCM);
	power->big_armpll_con0 = readl(power->mcucfg + MCUCFG_BIG_ARMPLL_CON0);

	return 0;
}

static int mt6797_a72_power_parse_cpus(struct device *dev,
				       struct mt6797_a72_power *power)
{
	struct device_node *cpu_node;
	int count;
	int cpu;
	int i;

	count = of_count_phandle_with_args(dev->of_node, "cpus", NULL);
	if (count != ARRAY_SIZE(power->cpus))
		return dev_err_probe(dev, -EINVAL,
				     "expected exactly two CPU phandles\n");

	cpumask_clear(&power->cpu_mask);
	for (i = 0; i < count; i++) {
		cpu_node = of_parse_phandle(dev->of_node, "cpus", i);
		if (!cpu_node)
			return dev_err_probe(dev, -EINVAL,
					     "failed to parse CPU phandle %d\n", i);
		cpu = of_cpu_node_to_id(cpu_node);
		of_node_put(cpu_node);
		if (cpu < 0 || cpu >= nr_cpu_ids)
			return dev_err_probe(dev, -EINVAL,
					     "invalid logical CPU for phandle %d\n", i);
		if (cpumask_test_and_set_cpu(cpu, &power->cpu_mask))
			return dev_err_probe(dev, -EINVAL,
					     "duplicate CPU phandle %d\n", i);
		power->cpus[i] = cpu;
	}

	return 0;
}

static int mt6797_a72_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_a72_power *power;
	int ret;

	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	ret = mt6797_a72_power_parse_cpus(dev, power);
	if (ret)
		return ret;

	power->vproc_big = devm_regulator_get_exclusive(dev, "vproc-big");
	if (IS_ERR(power->vproc_big))
		return dev_err_probe(dev, PTR_ERR(power->vproc_big),
				     "failed to get big-cluster regulator\n");

	power->pwrap_reset = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(power->pwrap_reset))
		return dev_err_probe(dev, PTR_ERR(power->pwrap_reset),
				     "failed to get PWRAP reset\n");

	power->spm = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "mediatek,spm");
	if (IS_ERR(power->spm))
		return dev_err_probe(dev, PTR_ERR(power->spm),
				     "failed to get SPM syscon\n");

	power->mcucfg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(power->mcucfg))
		return PTR_ERR(power->mcucfg);

	ret = mt6797_a72_power_snapshot(power);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read resource snapshot\n");

	power->resources_ready = true;
	platform_set_drvdata(pdev, power);
	ret = devm_device_add_group(dev, &mt6797_a72_power_group);
	if (ret)
		return ret;

	dev_info(dev,
		 "observer resources ready: vproc=%duV enabled=%d; CPU_ON denied\n",
		 power->vproc_uv, power->vproc_enabled);

	return 0;
}

static const struct of_device_id mt6797_a72_power_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_a72_power_of_match);

static struct platform_driver mt6797_a72_power_driver = {
	.probe = mt6797_a72_power_probe,
	.driver = {
		.name = "mt6797-a72-power",
		.of_match_table = mt6797_a72_power_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_power_driver);

MODULE_DESCRIPTION("MediaTek MT6797 Cortex-A72 power resource observer");
MODULE_LICENSE("GPL");
