// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 BTIF/APDMA resource-only inventory.
 *
 * This is deliberately not a transport driver. It retains only the Device
 * Tree inventory required by a future, separately reviewed BTIF/STP owner.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct mt6797_btif_inventory {
	const struct resource *core;
	const struct resource *tx_dma;
	const struct resource *rx_dma;
	struct clk *btifc_clk;
	struct clk *apdmac_clk;
	struct device_node *consys_np;
	int core_irq;
	int tx_irq;
	int rx_irq;
};

static void mt6797_btif_inventory_put_consys(void *data)
{
	of_node_put(data);
}

static int mt6797_btif_get_resource(struct platform_device *pdev,
				    const char *name,
				    const struct resource **resource)
{
	*resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!*resource)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing %s MMIO resource\n", name);

	return 0;
}

static int mt6797_btif_inventory_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_btif_inventory *inventory;
	int ret;

	inventory = devm_kzalloc(dev, sizeof(*inventory), GFP_KERNEL);
	if (!inventory)
		return -ENOMEM;

	ret = mt6797_btif_get_resource(pdev, "core", &inventory->core);
	if (ret)
		return ret;

	ret = mt6797_btif_get_resource(pdev, "tx-dma", &inventory->tx_dma);
	if (ret)
		return ret;

	ret = mt6797_btif_get_resource(pdev, "rx-dma", &inventory->rx_dma);
	if (ret)
		return ret;

	inventory->btifc_clk = devm_clk_get(dev, "btifc");
	if (IS_ERR(inventory->btifc_clk))
		return dev_err_probe(dev, PTR_ERR(inventory->btifc_clk),
				     "failed to get btifc clock\n");

	inventory->apdmac_clk = devm_clk_get(dev, "apdmac");
	if (IS_ERR(inventory->apdmac_clk))
		return dev_err_probe(dev, PTR_ERR(inventory->apdmac_clk),
				     "failed to get apdmac clock\n");

	inventory->core_irq = platform_get_irq_byname(pdev, "core");
	if (inventory->core_irq < 0)
		return dev_err_probe(dev, inventory->core_irq,
				     "failed to get core IRQ\n");

	inventory->tx_irq = platform_get_irq_byname(pdev, "tx");
	if (inventory->tx_irq < 0)
		return dev_err_probe(dev, inventory->tx_irq,
				     "failed to get TX DMA IRQ\n");

	inventory->rx_irq = platform_get_irq_byname(pdev, "rx");
	if (inventory->rx_irq < 0)
		return dev_err_probe(dev, inventory->rx_irq,
				     "failed to get RX DMA IRQ\n");

	inventory->consys_np = of_parse_phandle(dev->of_node,
						"mediatek,consys", 0);
	if (!inventory->consys_np)
		return dev_err_probe(dev, -EINVAL, "missing CONSYS phandle\n");

	ret = devm_add_action_or_reset(dev, mt6797_btif_inventory_put_consys,
				       inventory->consys_np);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, inventory);
	dev_info(dev, "resource inventory ready; hardware remains inactive\n");

	return 0;
}

static const struct of_device_id mt6797_btif_inventory_of_match[] = {
	{ .compatible = "mediatek,mt6797-btif-inventory" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_btif_inventory_of_match);

static struct platform_driver mt6797_btif_inventory_driver = {
	.probe = mt6797_btif_inventory_probe,
	.driver = {
		.name = "mt6797-btif-inventory",
		.of_match_table = mt6797_btif_inventory_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_btif_inventory_driver);

MODULE_DESCRIPTION("MT6797 BTIF/APDMA resource-only inventory");
MODULE_LICENSE("GPL");
