// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Julien Etienne
 */

#include <linux/bits.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/mt6351-regulator.h>
#include <linux/regulator/of_regulator.h>

#define MT6351_BUCK_QI		BIT(13)
#define MT6351_BUCK_VOSEL_CTRL	BIT(1)
#define MT6351_LDO_EN		BIT(1)

struct mt6351_regulator_info {
	struct regulator_desc desc;
	u16 qi;
	u16 vsel_on_reg;
	u16 vsel_ctrl_reg;
};

static const struct regulator_ops mt6351_buck_ops;
static const struct regulator_ops mt6351_ldo_ops;
static const struct regulator_ops mt6351_fixed_ops;

#define MT6351_BUCK(_match, _name, _range, _max_sel, _ctrl, _en, _vsel, _on, _mask) \
[MT6351_ID_##_name] = { \
	.desc = { \
		.name = #_name, \
		.of_match = of_match_ptr(_match), \
		.ops = &mt6351_buck_ops, \
		.type = REGULATOR_VOLTAGE, \
		.id = MT6351_ID_##_name, \
		.owner = THIS_MODULE, \
		.n_voltages = (_max_sel) + 1, \
		.linear_ranges = _range, \
		.n_linear_ranges = ARRAY_SIZE(_range), \
		.vsel_reg = _vsel, \
		.vsel_mask = _mask, \
		.enable_reg = _en, \
		.enable_mask = BIT(0), \
	}, \
	.qi = MT6351_BUCK_QI, \
	.vsel_on_reg = _on, \
	.vsel_ctrl_reg = _ctrl, \
}

#define MT6351_LDO(_match, _name, _table, _en, _vsel, _mask) \
[MT6351_ID_##_name] = { \
	.desc = { \
		.name = #_name, \
		.of_match = of_match_ptr(_match), \
		.ops = &mt6351_ldo_ops, \
		.type = REGULATOR_VOLTAGE, \
		.id = MT6351_ID_##_name, \
		.owner = THIS_MODULE, \
		.n_voltages = ARRAY_SIZE(_table), \
		.volt_table = _table, \
		.vsel_reg = _vsel, \
		.vsel_mask = _mask, \
		.enable_reg = _en, \
		.enable_mask = MT6351_LDO_EN, \
	}, \
}

#define MT6351_FIXED(_match, _name, _en, _voltage) \
[MT6351_ID_##_name] = { \
	.desc = { \
		.name = #_name, \
		.of_match = of_match_ptr(_match), \
		.ops = &mt6351_fixed_ops, \
		.type = REGULATOR_VOLTAGE, \
		.id = MT6351_ID_##_name, \
		.owner = THIS_MODULE, \
		.n_voltages = 1, \
		.min_uV = _voltage, \
		.enable_reg = _en, \
		.enable_mask = MT6351_LDO_EN, \
	}, \
}

static const struct linear_range mt6351_buck_range[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 0x7f, 6250),
};

static const struct linear_range mt6351_vpa_range[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 0x3f, 6250),
};

/* Tables are indexed by the raw hardware selector. */
static const unsigned int mt6351_ldo_voltages_rev4[] = {
	2800000, 2375000, 2200000, 1800000,
};

static const unsigned int mt6351_ldo_voltages_vcama[] = {
	1500000, 1800000, 2500000, 2800000,
};

static const unsigned int mt6351_ldo_voltages_sim[] = {
	1200000, 1300000, 1700000, 1800000,
	1860000, 2760000, 3000000, 3100000,
};

static const unsigned int mt6351_ldo_voltages_3v[] = {
	3000000, 3300000,
};

static const unsigned int mt6351_ldo_voltages_vibr[] = {
	1200000, 1300000, 1500000, 1800000,
	2000000, 2800000, 3000000, 3300000,
};

static const unsigned int mt6351_ldo_voltages_vcamd[] = {
	900000, 950000, 1000000, 1050000,
	1100000, 1200000, 1210000,
};

static const unsigned int mt6351_ldo_voltages_rf[] = {
	1000000, 1050000, 1100000, 1220000,
	1300000, 1500000, 1800000, 1810000,
};

static const unsigned int mt6351_ldo_voltages_1v8[] = {
	900000, 950000, 1000000, 1050000,
	1200000, 1500000, 1800000,
};

static const unsigned int mt6351_ldo_voltages_vcn33[] = {
	3300000, 3400000, 3500000, 3600000,
};

static const unsigned int mt6351_ldo_voltages_vmc[] = {
	1200000, 1300000, 1500000, 1800000,
	2000000, 2900000, 3000000, 3300000,
};

static int mt6351_buck_get_status(struct regulator_dev *rdev)
{
	struct mt6351_regulator_info *info = rdev_get_drvdata(rdev);
	unsigned int value;
	int ret;

	ret = regmap_read(rdev->regmap, info->desc.enable_reg, &value);
	if (ret)
		return ret;

	return value & info->qi ? REGULATOR_STATUS_ON : REGULATOR_STATUS_OFF;
}

static const struct regulator_ops mt6351_buck_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6351_buck_get_status,
};

static const struct regulator_ops mt6351_ldo_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_ops mt6351_fixed_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static struct mt6351_regulator_info mt6351_regulators[] = {
	MT6351_BUCK("buck-vcore", VCORE, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VCORE_CON0, MT6351_BUCK_VCORE_CON2,
		    MT6351_BUCK_VCORE_CON4, MT6351_BUCK_VCORE_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vgpu", VGPU, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VGPU_CON0, MT6351_BUCK_VGPU_CON2,
		    MT6351_BUCK_VGPU_CON4, MT6351_BUCK_VGPU_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vmodem", VMODEM, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VMODEM_CON0, MT6351_BUCK_VMODEM_CON2,
		    MT6351_BUCK_VMODEM_CON4, MT6351_BUCK_VMODEM_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vmd1", VMD1, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VMD1_CON0, MT6351_BUCK_VMD1_CON2,
		    MT6351_BUCK_VMD1_CON4, MT6351_BUCK_VMD1_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vsram-md", VSRAM_MD, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VSRAM_MD_CON0, MT6351_BUCK_VSRAM_MD_CON2,
		    MT6351_BUCK_VSRAM_MD_CON4, MT6351_BUCK_VSRAM_MD_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vs1", VS1, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VS1_CON0, MT6351_BUCK_VS1_CON2,
		    MT6351_BUCK_VS1_CON4, MT6351_BUCK_VS1_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vs2", VS2, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VS2_CON0, MT6351_BUCK_VS2_CON2,
		    MT6351_BUCK_VS2_CON4, MT6351_BUCK_VS2_CON5, GENMASK(6, 0)),
	MT6351_BUCK("buck-vpa", VPA, mt6351_vpa_range, 0x3f,
		    MT6351_BUCK_VPA_CON0, MT6351_BUCK_VPA_CON2,
		    MT6351_BUCK_VPA_CON4, MT6351_BUCK_VPA_CON5, GENMASK(5, 0)),
	MT6351_BUCK("buck-vsram-proc", VSRAM_PROC, mt6351_buck_range, 0x7f,
		    MT6351_BUCK_VSRAM_PROC_CON0, MT6351_BUCK_VSRAM_PROC_CON2,
		    MT6351_BUCK_VSRAM_PROC_CON4, MT6351_BUCK_VSRAM_PROC_CON5,
		    GENMASK(6, 0)),
	MT6351_LDO("ldo-va18", VA18, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VA18_CON0, MT6351_LDO_VA18_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vtcxo24", VTCXO24, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VTCXO24_CON0, MT6351_LDO_VTCXO24_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vtcxo28", VTCXO28, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VTCXO28_CON0, MT6351_LDO_VTCXO28_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vcn28", VCN28, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VCN28_CON0, MT6351_LDO_VCN28_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vcama", VCAMA, mt6351_ldo_voltages_vcama,
		   MT6351_LDO_VCAMA_CON0, MT6351_LDO_VCAMA_VOSEL, GENMASK(10, 8)),
	MT6351_FIXED("ldo-vusb33", VUSB33, MT6351_LDO_VUSB33_CON0, 3300000),
	MT6351_LDO("ldo-vsim1", VSIM1, mt6351_ldo_voltages_sim,
		   MT6351_LDO_VSIM1_CON0, MT6351_LDO_VSIM1_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vsim2", VSIM2, mt6351_ldo_voltages_sim,
		   MT6351_LDO_VSIM2_CON0, MT6351_LDO_VSIM2_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vemc", VEMC, mt6351_ldo_voltages_3v,
		   MT6351_LDO_VEMC_CON0, MT6351_LDO_VEMC_VOSEL, BIT(8)),
	MT6351_LDO("ldo-vmch", VMCH, mt6351_ldo_voltages_3v,
		   MT6351_LDO_VMCH_CON0, MT6351_LDO_VMCH_VOSEL, BIT(8)),
	MT6351_FIXED("ldo-vio28", VIO28, MT6351_LDO_VIO28_CON0, 2800000),
	MT6351_LDO("ldo-vibr", VIBR, mt6351_ldo_voltages_vibr,
		   MT6351_LDO_VIBR_CON0, MT6351_LDO_VIBR_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vcamd", VCAMD, mt6351_ldo_voltages_vcamd,
		   MT6351_LDO_VCAMD_CON0, MT6351_LDO_VCAMD_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vrf18", VRF18, mt6351_ldo_voltages_rf,
		   MT6351_LDO_VRF18_CON0, MT6351_LDO_VRF18_VOSEL, GENMASK(10, 8)),
	MT6351_FIXED("ldo-vio18", VIO18, MT6351_LDO_VIO18_CON0, 1800000),
	MT6351_LDO("ldo-vcn18", VCN18, mt6351_ldo_voltages_1v8,
		   MT6351_LDO_VCN18_CON0, MT6351_LDO_VCN18_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vcamio", VCAMIO, mt6351_ldo_voltages_1v8,
		   MT6351_LDO_VCAMIO_CON0, MT6351_LDO_VCAMIO_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vxo22", VXO22, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VXO22_CON0, MT6351_LDO_VXO22_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vrf12", VRF12, mt6351_ldo_voltages_1v8,
		   MT6351_LDO_VRF12_CON0, MT6351_LDO_VRF12_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-va10", VA10, mt6351_ldo_voltages_1v8,
		   MT6351_LDO_VA10_CON0, MT6351_LDO_VA10_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vdram", VDRAM, mt6351_ldo_voltages_vcamd,
		   MT6351_LDO_VDRAM_CON0, MT6351_LDO_VDRAM_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vmipi", VMIPI, mt6351_ldo_voltages_1v8,
		   MT6351_LDO_VMIPI_CON0, MT6351_LDO_VMIPI_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vgp3", VGP3, mt6351_ldo_voltages_rf,
		   MT6351_LDO_VGP3_CON0, MT6351_LDO_VGP3_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vbif28", VBIF28, mt6351_ldo_voltages_rev4,
		   MT6351_LDO_VBIF28_CON0, MT6351_LDO_VBIF28_VOSEL, GENMASK(9, 8)),
	MT6351_LDO("ldo-vefuse", VEFUSE, mt6351_ldo_voltages_sim,
		   MT6351_LDO_VEFUSE_CON0, MT6351_LDO_VEFUSE_VOSEL, GENMASK(10, 8)),
	MT6351_LDO("ldo-vcn33-bt", VCN33_BT, mt6351_ldo_voltages_vcn33,
		   MT6351_LDO_VCN33_BT_CON0, MT6351_LDO_VCN33_VOSEL, GENMASK(10, 9)),
	MT6351_LDO("ldo-vcn33-wifi", VCN33_WIFI, mt6351_ldo_voltages_vcn33,
		   MT6351_LDO_VCN33_WIFI_CON0, MT6351_LDO_VCN33_VOSEL, GENMASK(10, 9)),
	MT6351_FIXED("ldo-vldo28", VLDO28, MT6351_LDO_VLDO28_CON0, 2800000),
	MT6351_LDO("ldo-vmc", VMC, mt6351_ldo_voltages_vmc,
		   MT6351_LDO_VMC_CON0, MT6351_LDO_VMC_VOSEL, GENMASK(10, 8)),
	MT6351_FIXED("ldo-vldo28-1", VLDO28_1, MT6351_LDO_VLDO28_1_CON0, 2800000),
};

static int mt6351_select_buck_vsel_regs(struct device *dev,
					struct regmap *regmap)
{
	unsigned int value;
	int i, ret;

	for (i = 0; i < MT6351_ID_VREG_MAX; i++) {
		struct mt6351_regulator_info *info = &mt6351_regulators[i];

		if (!info->vsel_ctrl_reg)
			continue;

		ret = regmap_read(regmap, info->vsel_ctrl_reg, &value);
		if (ret) {
			dev_err(dev, "failed to read %s voltage control: %d\n",
				info->desc.name, ret);
			return ret;
		}

		if (value & MT6351_BUCK_VOSEL_CTRL)
			info->desc.vsel_reg = info->vsel_on_reg;
	}

	return 0;
}

static int mt6351_regulator_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6351 = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = {};
	struct regulator_dev *rdev;
	unsigned int swcid;
	int i, ret;

	ret = regmap_read(mt6351->regmap, MT6351_SWCID, &swcid);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read chip revision\n");

	if (swcid != 0x5120) {
		dev_err(&pdev->dev, "unsupported MT6351 revision 0x%04x\n", swcid);
		return -EINVAL;
	}

	ret = mt6351_select_buck_vsel_regs(&pdev->dev, mt6351->regmap);
	if (ret)
		return ret;

	config.dev = &pdev->dev;
	config.regmap = mt6351->regmap;
	for (i = 0; i < MT6351_ID_VREG_MAX; i++) {
		config.driver_data = &mt6351_regulators[i];
		rdev = devm_regulator_register(&pdev->dev,
					       &mt6351_regulators[i].desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     mt6351_regulators[i].desc.name);
	}

	return 0;
}

static const struct platform_device_id mt6351_platform_ids[] = {
	{ "mt6351-regulator", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6351_platform_ids);

static struct platform_driver mt6351_regulator_driver = {
	.driver = {
		.name = "mt6351-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = mt6351_regulator_probe,
	.id_table = mt6351_platform_ids,
};
module_platform_driver(mt6351_regulator_driver);

MODULE_AUTHOR("Julien Etienne <julien.etienne@gmail.com>");
MODULE_DESCRIPTION("Regulator driver for MediaTek MT6351 PMIC");
MODULE_LICENSE("GPL");
