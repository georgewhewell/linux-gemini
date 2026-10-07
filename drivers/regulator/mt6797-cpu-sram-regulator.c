// SPDX-License-Identifier: GPL-2.0-only
/* Eight CPU SRAM LDOs, programmed as one rail by the MT6797 DVFS policy. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/soc/mediatek/mt6797-cpu-rails.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

#define MT6797_CPULDO_CTRL0	0xf98
#define MT6797_CPULDO_CTRL1	0xf9c
#define MT6797_CPULDO_CTRL2	0xfa0
#define MT6797_CPULDO_ENABLE	0xff
#define MT6797_CPULDO_CODES	0x0f0f0f0f
#define MT6797_CPULDO_STEP_UV	25000
#define MT6797_CPULDO_SELECTORS	9

struct mt6797_cpu_sram {
	struct regmap *infracfg;
	struct mt6797_dvfsp_handoff *handoff;
	struct clk *i2c_clk;
};

struct mt6797_cpu_sram_request {
	struct mt6797_cpu_sram *sram;
	bool write;
	unsigned int selector;
};

/* Code zero is an alternative encoding of 1.05 V, not the minimum. */
static int mt6797_cpu_sram_decode(const u32 registers[3])
{
	unsigned int bank, lane, code, uv, voltage = 0;

	if ((registers[0] & MT6797_CPULDO_ENABLE) != MT6797_CPULDO_ENABLE)
		return -EPROTO;
	for (bank = 1; bank < 3; bank++) {
		for (lane = 0; lane < 4; lane++) {
			code = (registers[bank] >> (lane * 8)) & 0xf;
			if (!code)
				uv = 1050000;
			else if (code >= 7)
				uv = 900000 + (code - 3) * MT6797_CPULDO_STEP_UV;
			else
				return -EPROTO;
			if (voltage && voltage != uv)
				return -EPROTO;
			voltage = uv;
		}
	}
	return (voltage - MT6797_CPU_VSRAM_MIN_UV) / MT6797_CPULDO_STEP_UV;
}

static int mt6797_cpu_sram_read(struct mt6797_cpu_sram *sram, u32 registers[3])
{
	u32 second[3];
	unsigned int i;
	int ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(sram->infracfg, MT6797_CPULDO_CTRL0 + i * 4,
				  &registers[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 3; i++) {
		ret = regmap_read(sram->infracfg, MT6797_CPULDO_CTRL0 + i * 4,
				  &second[i]);
		if (ret)
			return ret;
	}
	return memcmp(registers, second, sizeof(second)) ? -EAGAIN : 0;
}

static int mt6797_cpu_sram_transaction(void __iomem *cspm, void *context)
{
	struct mt6797_cpu_sram_request *request = context;
	struct mt6797_cpu_sram *sram = request->sram;
	u32 before[3], after[3], code, value;
	unsigned int i, delta_uv, settle_us;
	int ret, current_selector, release_ret;

	ret = mt6797_dvfsp_cspm_acquire(cspm);
	if (ret)
		return ret;
	ret = mt6797_cpu_sram_read(sram, before);
	if (ret)
		goto release;
	ret = mt6797_cpu_sram_decode(before);
	if (ret < 0)
		goto release;
	current_selector = ret;
	if (!request->write) {
		request->selector = current_selector;
		ret = 0;
		goto release;
	}
	if (request->selector == current_selector) {
		ret = 0;
		goto release;
	}

	delta_uv = abs((int)request->selector - current_selector) *
		   MT6797_CPULDO_STEP_UV;
	/* Vendor SRAM settling: up 12.5 mV/us, down 2 * delta / 6.25 mV. */
	settle_us = request->selector > current_selector ?
		DIV_ROUND_UP(delta_uv, 12500) + 5 :
		DIV_ROUND_UP(2 * delta_uv, 6250) + 5;
	settle_us = max(settle_us, 25U);
	code = (request->selector + 7) * 0x01010101U;
	for (i = 1; i < 3; i++) {
		value = (before[i] & ~MT6797_CPULDO_CODES) | code;
		ret = regmap_write(sram->infracfg, MT6797_CPULDO_CTRL0 + i * 4,
				   value);
		/* A failed bus operation may still have reached the hardware. */
		udelay(settle_us);
		if (ret)
			goto uncertain;
		ret = regmap_read(sram->infracfg, MT6797_CPULDO_CTRL0 + i * 4,
				  &after[i]);
		if (ret || after[i] != value)
			goto uncertain;
	}
	ret = mt6797_cpu_sram_read(sram, after);
	if (ret || after[0] != before[0] ||
	    mt6797_cpu_sram_decode(after) != request->selector)
		goto uncertain;
	ret = 0;
	goto release;

uncertain:
	/* Fence every CSPM/I2C admission; never retry a partly changed rail. */
	ret = -EUCLEAN;
release:
	release_ret = mt6797_dvfsp_cspm_release(cspm);
	return release_ret ? -EUCLEAN : ret;
}

static int mt6797_cpu_sram_run(struct mt6797_cpu_sram_request *request)
{
	struct mt6797_cpu_sram *sram = request->sram;
	int ret;

	/* CCF callbacks take the CSPM lock: never enter CCF while holding it. */
	ret = clk_prepare_enable(sram->i2c_clk);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_cspm_transaction(sram->handoff,
					 mt6797_cpu_sram_transaction, request);
	clk_disable_unprepare(sram->i2c_clk);
	return ret;
}

static int mt6797_cpu_sram_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	struct mt6797_cpu_sram_request request = { .sram = sram };
	int ret;

	ret = mt6797_cpu_sram_run(&request);
	return ret ? ret : (int)request.selector;
}

static int mt6797_cpu_sram_set_voltage_sel(struct regulator_dev *rdev,
					 unsigned int selector)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	struct mt6797_cpu_sram_request request = {
		.sram = sram, .write = true, .selector = selector,
	};

	if (selector >= MT6797_CPULDO_SELECTORS)
		return -EINVAL;
	return mt6797_cpu_sram_run(&request);
}

static int mt6797_cpu_sram_is_enabled(struct regulator_dev *rdev)
{
	int ret = mt6797_cpu_sram_get_voltage_sel(rdev);

	return ret < 0 ? ret : 1;
}

static const struct regulator_ops mt6797_cpu_sram_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = mt6797_cpu_sram_get_voltage_sel,
	.set_voltage_sel = mt6797_cpu_sram_set_voltage_sel,
	.is_enabled = mt6797_cpu_sram_is_enabled,
};

static const struct regulator_desc mt6797_cpu_sram_desc = {
	.name = "mt6797-cpu-sram",
	.ops = &mt6797_cpu_sram_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.n_voltages = MT6797_CPULDO_SELECTORS,
	.min_uV = MT6797_CPU_VSRAM_MIN_UV,
	.uV_step = MT6797_CPULDO_STEP_UV,
};

static int mt6797_cpu_sram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { .dev = dev, .of_node = dev->of_node };
	struct mt6797_cpu_sram_request request = { };
	struct mt6797_cpu_sram *sram;
	struct regulator_dev *rdev;
	int ret;

	sram = devm_kzalloc(dev, sizeof(*sram), GFP_KERNEL);
	if (!sram)
		return -ENOMEM;
	sram->handoff = mt6797_dvfsp_handoff_get(dev);
	if (IS_ERR(sram->handoff))
		return dev_err_probe(dev, PTR_ERR(sram->handoff), "CSPM owner unavailable\n");
	ret = mt6797_dvfsp_handoff_require_ready(sram->handoff);
	if (ret)
		return dev_err_probe(dev, ret, "CSPM owner denied access\n");
	sram->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(sram->infracfg))
		return dev_err_probe(dev, PTR_ERR(sram->infracfg), "INFRACFG unavailable\n");
	sram->i2c_clk = devm_clk_get(dev, "i2c");
	if (IS_ERR(sram->i2c_clk))
		return dev_err_probe(dev, PTR_ERR(sram->i2c_clk), "CSPM clock unavailable\n");

	request.sram = sram;
	ret = mt6797_cpu_sram_run(&request);
	if (ret)
		return dev_err_probe(dev, ret, "cannot adopt CPU SRAM LDOs\n");
	config.driver_data = sram;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node,
						    &mt6797_cpu_sram_desc);
	if (!config.init_data)
		return -EINVAL;
	rdev = devm_regulator_register(dev, &mt6797_cpu_sram_desc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id mt6797_cpu_sram_match[] = {
	{ .compatible = "mediatek,mt6797-cpu-sram-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_cpu_sram_match);

static struct platform_driver mt6797_cpu_sram_driver = {
	.probe = mt6797_cpu_sram_probe,
	.driver = {
		.name = "mt6797-cpu-sram-regulator",
		.of_match_table = mt6797_cpu_sram_match,
	},
};
module_platform_driver(mt6797_cpu_sram_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CPU SRAM LDO regulator");
MODULE_LICENSE("GPL");
