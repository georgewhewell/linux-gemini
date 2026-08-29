// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 connectivity subsystem resource inventory.
 *
 * This driver is deliberately fail-closed: it only retains and validates
 * Device Tree resource metadata for a later CONSYS owner. It never maps or
 * accesses MMIO; enables or disables a regulator, clock, reset, interrupt,
 * or power domain; loads firmware; or maps the shared reserved memory.
 */

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/sizes.h>

#define MT6797_CONSYS_SHARED_MEM_SIZE	SZ_2M

struct mt6797_consys_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

struct mt6797_consys {
	const struct resource *mcu;
	const struct resource *ap_rgu;
	const struct resource *topckgen;
	const struct resource *spm;
	struct resource shared_mem;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regulator *vcn33_wifi;
	struct clk *conn_clk;
	struct reset_control *conn_mcu_reset;
	int bgf_irq;
	int wdt_irq;
};

static const struct mt6797_consys_resource mt6797_consys_resources[] = {
	{
		.name = "mcu",
		.start = 0x18070000,
		.size = SZ_512,
	}, {
		.name = "ap-rgu",
		.start = 0x10007000,
		.size = SZ_256,
	}, {
		.name = "topckgen",
		.start = 0x10000000,
		.size = SZ_8K,
	}, {
		.name = "spm",
		.start = 0x10006000,
		.size = SZ_4K,
	},
};

static int mt6797_consys_get_resource(struct platform_device *pdev,
				      const struct mt6797_consys_resource *expected,
				      const struct resource **resource)
{
	const struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing %s resource\n", expected->name);

	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid %s resource\n", expected->name);

	*resource = res;
	return 0;
}

static int mt6797_consys_get_supply(struct device *dev, const char *name,
				    struct regulator **supply)
{
	*supply = devm_regulator_get_optional(dev, name);
	if (IS_ERR(*supply)) {
		if (PTR_ERR(*supply) == -ENODEV) {
			*supply = NULL;
			return 0;
		}

		return dev_err_probe(dev, PTR_ERR(*supply),
				     "failed to acquire %s supply\n", name);
	}

	return 0;
}

static int
mt6797_consys_validate_shared_mem(struct device *dev,
				  struct resource *shared_mem)
{
	struct device_node *memory_region;
	int ret;

	memory_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!memory_region)
		return dev_err_probe(dev, -EINVAL, "missing shared memory region\n");

	if (!of_property_read_bool(memory_region, "no-map")) {
		of_node_put(memory_region);
		return dev_err_probe(dev, -EINVAL,
				     "shared memory region must be no-map\n");
	}

	of_node_put(memory_region);

	ret = of_reserved_mem_region_to_resource_byname(dev->of_node, "shared", shared_mem);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire shared memory\n");

	if (resource_size(shared_mem) != MT6797_CONSYS_SHARED_MEM_SIZE ||
	    !IS_ALIGNED(shared_mem->start, MT6797_CONSYS_SHARED_MEM_SIZE))
		return dev_err_probe(dev, -EINVAL,
				     "invalid shared memory geometry\n");

	return 0;
}

static int mt6797_consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_consys *consys;
	int ret;

	consys = devm_kzalloc(dev, sizeof(*consys), GFP_KERNEL);
	if (!consys)
		return -ENOMEM;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[0],
					 &consys->mcu);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[1],
					 &consys->ap_rgu);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[2],
					 &consys->topckgen);
	if (ret)
		return ret;

	ret = mt6797_consys_get_resource(pdev, &mt6797_consys_resources[3],
					 &consys->spm);
	if (ret)
		return ret;

	ret = mt6797_consys_validate_shared_mem(dev, &consys->shared_mem);
	if (ret)
		return ret;

	ret = mt6797_consys_get_supply(dev, "vcn18", &consys->vcn18);
	if (ret)
		return ret;

	ret = mt6797_consys_get_supply(dev, "vcn28", &consys->vcn28);
	if (ret)
		return ret;

	ret = mt6797_consys_get_supply(dev, "vcn33-bt", &consys->vcn33_bt);
	if (ret)
		return ret;

	ret = mt6797_consys_get_supply(dev, "vcn33-wifi", &consys->vcn33_wifi);
	if (ret)
		return ret;

	consys->conn_clk = devm_clk_get_optional(dev, "conn");
	if (IS_ERR(consys->conn_clk))
		return dev_err_probe(dev, PTR_ERR(consys->conn_clk),
				     "failed to acquire conn clock\n");

	consys->conn_mcu_reset =
		devm_reset_control_get_optional_exclusive(dev, "conn-mcu");
	if (IS_ERR(consys->conn_mcu_reset))
		return dev_err_probe(dev, PTR_ERR(consys->conn_mcu_reset),
				     "failed to acquire CONN MCU reset\n");

	consys->bgf_irq = platform_get_irq_byname(pdev, "bgf");
	if (consys->bgf_irq < 0)
		return dev_err_probe(dev, consys->bgf_irq,
				     "failed to acquire BGF IRQ\n");

	consys->wdt_irq = platform_get_irq_byname(pdev, "wdt");
	if (consys->wdt_irq < 0)
		return dev_err_probe(dev, consys->wdt_irq,
				     "failed to acquire WDT IRQ\n");

	platform_set_drvdata(pdev, consys);
	dev_info(dev, "resource inventory only; hardware remains untouched\n");

	return 0;
}

static const struct of_device_id mt6797_consys_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_of_match);

static struct platform_driver mt6797_consys_driver = {
	.probe = mt6797_consys_probe,
	.driver = {
		.name = "mt6797-consys",
		.of_match_table = mt6797_consys_of_match,
	},
};
module_platform_driver(mt6797_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CONSYS resource inventory");
MODULE_LICENSE("GPL");
