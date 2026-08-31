// SPDX-License-Identifier: GPL-2.0-only
/*
 * Strictly gated MT6797 MD1 CLDMA resource preparation.
 *
 * This consumes only the vendor's inert topology and LK-attested AP leases.
 * It intentionally leaves every RX GPD HWO bit clear, leaves all IRQs masked,
 * and never maps CLDMA MMIO, writes shared MD memory, starts a DMA queue,
 * releases MD1, or touches RF/SIM state.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mt6797-md1-cldma.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>

#define MT6797_MD1_CLDMA_CONTROL_GPD_COUNT 16U
#define MT6797_MD1_CLDMA_CONTROL_RX_SIZE SZ_4K

static const char *const mt6797_md1_cldma_reg_names[] = {
	"ap-cldma-ao",	"md-cldma-ao", "ap-cldma-pdn",
	"md-cldma-pdn", "ap-ccif",     "md-ccif",
};

/* Raw GIC specifiers, validated before platform_get_irq() creates virqs. */
static const u32 mt6797_md1_cldma_irq_spec[] = {
	0, 265, IRQ_TYPE_LEVEL_HIGH,   0, 147, IRQ_TYPE_LEVEL_LOW,
	0, 266, IRQ_TYPE_EDGE_FALLING,
};

static bool enable_experimental_md1_cldma_prepare;
module_param_named(enable_experimental_md1_cldma_prepare,
		   enable_experimental_md1_cldma_prepare, bool, 0400);
MODULE_PARM_DESC(
	enable_experimental_md1_cldma_prepare,
	"allow inert MT6797 MD1 CLDMA resource/DMA preparation (default: false)");

struct mt6797_md1_cldma_prepare {
	struct device *dev;
	struct dma_pool *gpd_pool;
	struct mt6797_md1_cldma_tgpd *tx_gpd[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	struct mt6797_md1_cldma_rgpd *rx_gpd[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	dma_addr_t tx_gpd_dma[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	dma_addr_t rx_gpd_dma[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	void *rx_buffer[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	dma_addr_t rx_dma[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	u16 rx_size[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	bool rx_mapped[MT6797_MD1_CLDMA_CONTROL_GPD_COUNT];
	int irq[MT6797_MD1_CLDMA_IRQ_COUNT];
};

static irqreturn_t mt6797_md1_cldma_inert_irq(int irq, void *context)
{
	/* IRQF_NO_AUTOEN keeps this unreachable; no CLDMA/CCIF register is read. */
	return IRQ_NONE;
}

static void mt6797_md1_cldma_prepare_release(void *context)
{
	struct mt6797_md1_cldma_prepare *prepare = context;
	unsigned int i;

	for (i = 0; i < MT6797_MD1_CLDMA_CONTROL_GPD_COUNT; i++) {
		if (prepare->rx_mapped[i])
			dma_unmap_single(prepare->dev, prepare->rx_dma[i],
					 prepare->rx_size[i], DMA_FROM_DEVICE);
		if (prepare->tx_gpd[i])
			dma_pool_free(prepare->gpd_pool, prepare->tx_gpd[i],
				      prepare->tx_gpd_dma[i]);
		if (prepare->rx_gpd[i])
			dma_pool_free(prepare->gpd_pool, prepare->rx_gpd[i],
				      prepare->rx_gpd_dma[i]);
	}
	dma_pool_destroy(prepare->gpd_pool);
}

static int mt6797_md1_cldma_validate_resources(struct platform_device *pdev)
{
	struct mt6797_md1_cldma_resources resources = {};
	struct resource *res;
	u32 irq_spec[ARRAY_SIZE(mt6797_md1_cldma_irq_spec)];
	unsigned int i;
	int ret;

	if (of_property_read_u32(pdev->dev.of_node, "mediatek,md_id",
				 &resources.md_id) ||
	    of_property_read_u32(pdev->dev.of_node, "mediatek,cldma_capability",
				 &resources.capability) ||
	    of_property_read_u32(pdev->dev.of_node, "mediatek,md_smem_size",
				 &resources.md_smem_size))
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"missing vendor MD1 CLDMA identity properties\n");

	for (i = 0; i < MT6797_MD1_CLDMA_REGION_COUNT; i++) {
		res = platform_get_resource_byname(
			pdev, IORESOURCE_MEM, mt6797_md1_cldma_reg_names[i]);
		if (!res)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "missing %s resource\n",
					     mt6797_md1_cldma_reg_names[i]);
		resources.region[i].base = res->start;
		resources.region[i].size = resource_size(res);
	}
	if (of_property_count_u32_elems(pdev->dev.of_node, "interrupts") !=
		    ARRAY_SIZE(irq_spec) ||
	    of_property_read_u32_array(pdev->dev.of_node, "interrupts",
				       irq_spec, ARRAY_SIZE(irq_spec)) ||
	    memcmp(irq_spec, mt6797_md1_cldma_irq_spec, sizeof(irq_spec)))
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"CLDMA GIC specifiers differ from the vendor topology\n");
	for (i = 0; i < MT6797_MD1_CLDMA_IRQ_COUNT; i++) {
		resources.irq[i].hwirq = irq_spec[i * 3 + 1];
		resources.irq[i].type = i;
	}
	ret = mt6797_md1_cldma_resources_validate(&resources);
	if (ret)
		return dev_err_probe(
			&pdev->dev, ret,
			"MD1 CLDMA topology is not the attested vendor topology\n");
	return 0;
}

static int mt6797_md1_cldma_validate_lk_lease(struct device *dev)
{
	struct mt6797_md1_mpu_remap_layout layout;
	int ret;

	ret = mt6797_md1_lk_handoff_layout_get(&layout);
	if (ret == -ENODATA)
		return dev_err_probe(
			dev, -EPROBE_DEFER,
			"waiting for the read-only LK CCCI lease attestation\n");
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not obtain LK CCCI lease\n");
	ret = mt6797_md1_cldma_lk_carveout_validate(&layout);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"LK CCCI lease differs from the active Gemini contract\n");
	return 0;
}

static int
mt6797_md1_cldma_prepare_dma(struct mt6797_md1_cldma_prepare *prepare)
{
	struct device *dev = prepare->dev;
	unsigned int i;
	int ret;

	ret = dma_set_mask_and_coherent(
		dev, DMA_BIT_MASK(MT6797_MD1_CLDMA_ADDR_BITS));
	if (ret)
		return dev_err_probe(
			dev, ret,
			"could not establish a 36-bit coherent DMA domain\n");

	prepare->gpd_pool = dma_pool_create("mt6797-md1-cldma-gpd", dev,
					    MT6797_MD1_CLDMA_GPD_BYTES,
					    MT6797_MD1_CLDMA_GPD_ALIGN, 0);
	if (!prepare->gpd_pool)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, mt6797_md1_cldma_prepare_release,
				       prepare);
	if (ret)
		return ret;

	for (i = 0; i < MT6797_MD1_CLDMA_CONTROL_GPD_COUNT; i++) {
		prepare->tx_gpd[i] = dma_pool_alloc(
			prepare->gpd_pool, GFP_KERNEL, &prepare->tx_gpd_dma[i]);
		prepare->rx_gpd[i] = dma_pool_alloc(
			prepare->gpd_pool, GFP_KERNEL, &prepare->rx_gpd_dma[i]);
		prepare->rx_buffer[i] = devm_kmalloc(
			dev, MT6797_MD1_CLDMA_CONTROL_RX_SIZE, GFP_KERNEL);
		if (!prepare->tx_gpd[i] || !prepare->rx_gpd[i] ||
		    !prepare->rx_buffer[i])
			return -ENOMEM;
		prepare->rx_size[i] = MT6797_MD1_CLDMA_CONTROL_RX_SIZE;
		prepare->rx_dma[i] = dma_map_single(dev, prepare->rx_buffer[i],
						    prepare->rx_size[i],
						    DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, prepare->rx_dma[i]))
			return dev_err_probe(
				dev, -EIO, "RX buffer %u DMA mapping failed\n",
				i);
		prepare->rx_mapped[i] = true;
	}
	ret = mt6797_md1_cldma_tx_ring_init(prepare->tx_gpd,
					    prepare->tx_gpd_dma,
					    MT6797_MD1_CLDMA_CONTROL_GPD_COUNT);
	if (ret)
		return ret;
	return mt6797_md1_cldma_rx_ring_init(
		prepare->rx_gpd, prepare->rx_gpd_dma, prepare->rx_dma,
		prepare->rx_size, MT6797_MD1_CLDMA_CONTROL_GPD_COUNT);
}

static int
mt6797_md1_cldma_request_irqs(struct platform_device *pdev,
			      struct mt6797_md1_cldma_prepare *prepare)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_MD1_CLDMA_IRQ_COUNT; i++) {
		prepare->irq[i] = platform_get_irq(pdev, i);
		if (prepare->irq[i] < 0)
			return prepare->irq[i];
		if (irq_get_trigger_type(prepare->irq[i]) !=
		    mt6797_md1_cldma_irq_spec[i * 3 + 2])
			return dev_err_probe(
				&pdev->dev, -EINVAL,
				"CLDMA IRQ %u has an unexpected trigger type\n",
				i);
		ret = devm_request_irq(&pdev->dev, prepare->irq[i],
				       mt6797_md1_cldma_inert_irq,
				       IRQF_NO_AUTOEN, dev_name(&pdev->dev),
				       prepare);
		if (ret)
			return dev_err_probe(
				&pdev->dev, ret,
				"could not reserve disabled MD1 CLDMA IRQ %u\n",
				i);
	}
	return 0;
}

static int mt6797_md1_cldma_prepare_probe(struct platform_device *pdev)
{
	struct mt6797_md1_cldma_prepare *prepare;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_cldma_prepare) ||
	    !device_property_read_bool(
		    &pdev->dev, "mediatek,experimental-md1-cldma-prepare") ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,stage-dma-rings-inert"))
		return dev_err_probe(
			&pdev->dev, -EPERM,
			"MD1 CLDMA preparation requires independent module and DT gates\n");
	if (!mt6797_md1_lk_handoff_image_loaded())
		return dev_err_probe(
			&pdev->dev, -EPROBE_DEFER,
			"waiting for verified LK MD1 image-load evidence\n");
	ret = mt6797_md1_cldma_validate_lk_lease(&pdev->dev);
	if (ret)
		return ret;
	ret = mt6797_md1_cldma_validate_resources(pdev);
	if (ret)
		return ret;

	prepare = devm_kzalloc(&pdev->dev, sizeof(*prepare), GFP_KERNEL);
	if (!prepare)
		return -ENOMEM;
	prepare->dev = &pdev->dev;
	ret = mt6797_md1_cldma_prepare_dma(prepare);
	if (ret)
		return ret;
	ret = mt6797_md1_cldma_request_irqs(pdev, prepare);
	if (ret)
		return ret;

	dev_info(
		&pdev->dev,
		"prepared %u inert 16-byte RX/TX GPDs and disabled CLDMA/CCIF/WDT IRQs; HWO, MMIO, shared memory and MD1 release remain untouched\n",
		MT6797_MD1_CLDMA_CONTROL_GPD_COUNT);
	return 0;
}

static const struct of_device_id mt6797_md1_cldma_prepare_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-cldma-prepare" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_md1_cldma_prepare_of_match);

static struct platform_driver mt6797_md1_cldma_prepare_driver = {
	.probe = mt6797_md1_cldma_prepare_probe,
	.driver = {
		.name = "mt6797-md1-cldma-prepare",
		.of_match_table = mt6797_md1_cldma_prepare_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_md1_cldma_prepare_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 MD1 inert CLDMA resource preparation");
