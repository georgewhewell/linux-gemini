// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 connectivity HIF DMA driver
 *
 * Copyright (c) 2026
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "../virt-dma.h"

#define MTK_HIF_DMA_INT_FLAG		0x00
#define MTK_HIF_DMA_INT_EN		0x04
#define MTK_HIF_DMA_EN			0x08
#define MTK_HIF_DMA_RESET		0x0c
#define MTK_HIF_DMA_CON			0x18
#define MTK_HIF_DMA_SRC			0x1c
#define MTK_HIF_DMA_DST			0x20
#define MTK_HIF_DMA_LEN			0x24
#define MTK_HIF_DMA_DEBUG_STATUS	0x50
#define MTK_HIF_DMA_SRC_ADDR2		0x54
#define MTK_HIF_DMA_DST_ADDR2		0x58

#define MTK_HIF_DMA_INT_BIT		BIT(0)
#define MTK_HIF_DMA_EN_BIT		BIT(0)
#define MTK_HIF_DMA_HARD_RESET_BIT	BIT(1)
#define MTK_HIF_DMA_DIR_RX_BIT		BIT(0)
#define MTK_HIF_DMA_BURST_4_4		GENMASK(17, 16)
#define MTK_HIF_DMA_BUS_INCR_BIT	BIT(31)
#define MTK_HIF_DMA_MAX_LEN		GENMASK(19, 0)
#define MTK_HIF_DMA_ADDR2_MASK		BIT(0)
#define MTK_HIF_DMA_BUSWIDTHS		BIT(DMA_SLAVE_BUSWIDTH_4_BYTES)

#define MTK_HIF_DMA_POLL_US		10
#define MTK_HIF_DMA_TIMEOUT_US		1000

struct mtk_hif_dma_desc {
	struct virt_dma_desc vd;
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
	enum dma_transfer_direction direction;
};

struct mtk_hif_dma_dev {
	struct dma_device ddev;
	struct virt_dma_chan vc;
	struct mtk_hif_dma_desc *active;
	struct dma_slave_config cfg;
	void __iomem *base;
	struct clk *clk;
	int irq;
};

static struct mtk_hif_dma_dev *to_mtk_hif_dma_dev(struct dma_chan *chan)
{
	return container_of(chan->device, struct mtk_hif_dma_dev, ddev);
}

static struct mtk_hif_dma_desc *to_mtk_hif_dma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct mtk_hif_dma_desc, vd);
}

static void mtk_hif_dma_write(struct mtk_hif_dma_dev *hif, u32 reg, u32 val)
{
	writel(val, hif->base + reg);
}

static u32 mtk_hif_dma_read(struct mtk_hif_dma_dev *hif, u32 reg)
{
	return readl(hif->base + reg);
}

static int mtk_hif_dma_wait_idle(struct mtk_hif_dma_dev *hif)
{
	u32 val;

	return readl_poll_timeout_atomic(hif->base + MTK_HIF_DMA_EN, val,
					 !(val & MTK_HIF_DMA_EN_BIT),
					 MTK_HIF_DMA_POLL_US,
					 MTK_HIF_DMA_TIMEOUT_US);
}

static int mtk_hif_dma_hard_reset(struct mtk_hif_dma_dev *hif)
{
	mtk_hif_dma_write(hif, MTK_HIF_DMA_RESET,
			  MTK_HIF_DMA_HARD_RESET_BIT);
	mdelay(1);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_RESET, 0);

	return mtk_hif_dma_wait_idle(hif);
}

static void mtk_hif_dma_start(struct mtk_hif_dma_dev *hif,
			      struct mtk_hif_dma_desc *desc)
{
	u32 con = MTK_HIF_DMA_BURST_4_4 | MTK_HIF_DMA_BUS_INCR_BIT;

	if (desc->direction == DMA_DEV_TO_MEM)
		con |= MTK_HIF_DMA_DIR_RX_BIT;

	mtk_hif_dma_write(hif, MTK_HIF_DMA_CON, con);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_SRC, lower_32_bits(desc->src));
	mtk_hif_dma_write(hif, MTK_HIF_DMA_DST, lower_32_bits(desc->dst));
	mtk_hif_dma_write(hif, MTK_HIF_DMA_LEN, desc->len);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_SRC_ADDR2,
			  upper_32_bits(desc->src) & MTK_HIF_DMA_ADDR2_MASK);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_DST_ADDR2,
			  upper_32_bits(desc->dst) & MTK_HIF_DMA_ADDR2_MASK);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_FLAG, 0);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_EN, MTK_HIF_DMA_INT_BIT);
	/* Publish the complete descriptor before starting the DMA engine. */
	wmb();
	mtk_hif_dma_write(hif, MTK_HIF_DMA_EN, MTK_HIF_DMA_EN_BIT);
}

static void mtk_hif_dma_start_next(struct mtk_hif_dma_dev *hif)
{
	struct virt_dma_desc *vd;

	if (hif->active)
		return;

	vd = vchan_next_desc(&hif->vc);
	if (!vd)
		return;

	hif->active = to_mtk_hif_dma_desc(vd);
	mtk_hif_dma_start(hif, hif->active);
}

static irqreturn_t mtk_hif_dma_irq(int irq, void *data)
{
	struct mtk_hif_dma_dev *hif = data;

	if (!(mtk_hif_dma_read(hif, MTK_HIF_DMA_INT_FLAG) &
	      MTK_HIF_DMA_INT_BIT))
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mtk_hif_dma_irq_thread(int irq, void *data)
{
	struct mtk_hif_dma_dev *hif = data;
	struct mtk_hif_dma_desc *desc;
	unsigned long flags;
	int ret;

	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_EN, 0);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_FLAG, 0);

	ret = mtk_hif_dma_wait_idle(hif);
	if (ret) {
		dev_err_ratelimited(hif->ddev.dev,
				    "completion while engine active, status %#x\n",
				    mtk_hif_dma_read(hif,
						     MTK_HIF_DMA_DEBUG_STATUS));
		if (mtk_hif_dma_hard_reset(hif))
			dev_err_ratelimited(hif->ddev.dev,
					    "hard reset timed out\n");
	}

	spin_lock_irqsave(&hif->vc.lock, flags);
	desc = hif->active;
	if (desc) {
		if (ret) {
			desc->vd.tx_result.result = DMA_TRANS_ABORTED;
			desc->vd.tx_result.residue = desc->len;
		}
		list_del(&desc->vd.node);
		vchan_cookie_complete(&desc->vd);
		hif->active = NULL;
	}
	mtk_hif_dma_start_next(hif);
	spin_unlock_irqrestore(&hif->vc.lock, flags);

	return IRQ_HANDLED;
}

static int mtk_hif_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);
	int ret;

	ret = pm_runtime_resume_and_get(hif->ddev.dev);
	if (ret < 0)
		return ret;

	ret = mtk_hif_dma_hard_reset(hif);
	if (ret) {
		dev_err(hif->ddev.dev, "initial hard reset timed out\n");
		goto err_pm;
	}
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_EN, 0);
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_FLAG, 0);

	ret = request_threaded_irq(hif->irq, mtk_hif_dma_irq,
				   mtk_hif_dma_irq_thread, IRQF_ONESHOT,
				   dev_name(hif->ddev.dev), hif);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_put_sync(hif->ddev.dev);
	return ret;
}

static void mtk_hif_dma_desc_free(struct virt_dma_desc *vd)
{
	kfree(to_mtk_hif_dma_desc(vd));
}

static int mtk_hif_dma_config(struct dma_chan *chan,
			      struct dma_slave_config *cfg)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);

	if (cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES &&
	    cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_UNDEFINED)
		return -EINVAL;
	if (cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES &&
	    cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_UNDEFINED)
		return -EINVAL;

	hif->cfg = *cfg;
	return 0;
}

static struct dma_async_tx_descriptor *
mtk_hif_dma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
			  unsigned int sglen,
			  enum dma_transfer_direction direction,
			  unsigned long flags, void *context)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);
	struct mtk_hif_dma_desc *desc;
	dma_addr_t mem_addr, src, dst;
	size_t len;

	if (!is_slave_direction(direction) || sglen != 1)
		return NULL;

	mem_addr = sg_dma_address(sgl);
	len = sg_dma_len(sgl);
	if (!len || len > MTK_HIF_DMA_MAX_LEN || !IS_ALIGNED(len, 4))
		return NULL;

	if (direction == DMA_MEM_TO_DEV) {
		if (!hif->cfg.dst_addr)
			return NULL;
		src = mem_addr;
		dst = hif->cfg.dst_addr;
	} else {
		if (!hif->cfg.src_addr)
			return NULL;
		src = hif->cfg.src_addr;
		dst = mem_addr;
	}

	if ((upper_32_bits(src) & ~MTK_HIF_DMA_ADDR2_MASK) ||
	    (upper_32_bits(dst) & ~MTK_HIF_DMA_ADDR2_MASK) ||
	    !IS_ALIGNED(src, 4) || !IS_ALIGNED(dst, 4))
		return NULL;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->src = src;
	desc->dst = dst;
	desc->len = len;
	desc->direction = direction;

	return vchan_tx_prep(&hif->vc, &desc->vd, flags);
}

static void mtk_hif_dma_issue_pending(struct dma_chan *chan)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);
	unsigned long flags;

	spin_lock_irqsave(&hif->vc.lock, flags);
	if (vchan_issue_pending(&hif->vc))
		mtk_hif_dma_start_next(hif);
	spin_unlock_irqrestore(&hif->vc.lock, flags);
}

static enum dma_status mtk_hif_dma_tx_status(struct dma_chan *chan,
					     dma_cookie_t cookie,
					     struct dma_tx_state *state)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);
	struct virt_dma_desc *vd;
	enum dma_status status;
	unsigned long flags;

	status = dma_cookie_status(chan, cookie, state);
	if (status == DMA_COMPLETE || !state)
		return status;

	spin_lock_irqsave(&hif->vc.lock, flags);
	vd = vchan_find_desc(&hif->vc, cookie);
	if (vd)
		dma_set_residue(state, to_mtk_hif_dma_desc(vd)->len);
	spin_unlock_irqrestore(&hif->vc.lock, flags);

	return status;
}

static int mtk_hif_dma_terminate_all(struct dma_chan *chan)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);
	unsigned long flags;
	LIST_HEAD(head);
	int ret;

	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_EN, 0);
	synchronize_irq(hif->irq);

	ret = mtk_hif_dma_wait_idle(hif);
	if (ret) {
		dev_err_ratelimited(hif->ddev.dev,
				    "stop timed out, status %#x\n",
				    mtk_hif_dma_read(hif,
						     MTK_HIF_DMA_DEBUG_STATUS));
		if (mtk_hif_dma_hard_reset(hif))
			dev_err_ratelimited(hif->ddev.dev,
					    "hard reset timed out\n");
	}
	mtk_hif_dma_write(hif, MTK_HIF_DMA_INT_FLAG, 0);

	spin_lock_irqsave(&hif->vc.lock, flags);
	hif->active = NULL;
	vchan_get_all_descriptors(&hif->vc, &head);
	spin_unlock_irqrestore(&hif->vc.lock, flags);
	vchan_dma_desc_free_list(&hif->vc, &head);

	return ret;
}

static void mtk_hif_dma_synchronize(struct dma_chan *chan)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);

	synchronize_irq(hif->irq);
	vchan_synchronize(&hif->vc);
}

static void mtk_hif_dma_free_chan_resources(struct dma_chan *chan)
{
	struct mtk_hif_dma_dev *hif = to_mtk_hif_dma_dev(chan);

	mtk_hif_dma_terminate_all(chan);
	free_irq(hif->irq, hif);
	vchan_free_chan_resources(&hif->vc);
	pm_runtime_put_sync(hif->ddev.dev);
}

static int mtk_hif_dma_runtime_suspend(struct device *dev)
{
	struct mtk_hif_dma_dev *hif = dev_get_drvdata(dev);

	clk_disable_unprepare(hif->clk);
	return 0;
}

static int mtk_hif_dma_runtime_resume(struct device *dev)
{
	struct mtk_hif_dma_dev *hif = dev_get_drvdata(dev);

	return clk_prepare_enable(hif->clk);
}

static int mtk_hif_dma_suspend(struct device *dev)
{
	struct mtk_hif_dma_dev *hif = dev_get_drvdata(dev);
	unsigned long flags;
	bool busy;

	spin_lock_irqsave(&hif->vc.lock, flags);
	busy = hif->active || !list_empty(&hif->vc.desc_issued) ||
	       !list_empty(&hif->vc.desc_submitted);
	spin_unlock_irqrestore(&hif->vc.lock, flags);
	if (busy)
		return -EBUSY;

	if (!pm_runtime_suspended(dev))
		clk_disable_unprepare(hif->clk);

	return 0;
}

static int mtk_hif_dma_resume(struct device *dev)
{
	struct mtk_hif_dma_dev *hif = dev_get_drvdata(dev);

	if (!pm_runtime_suspended(dev))
		return clk_prepare_enable(hif->clk);

	return 0;
}

static const struct dev_pm_ops mtk_hif_dma_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_hif_dma_suspend, mtk_hif_dma_resume)
	SET_RUNTIME_PM_OPS(mtk_hif_dma_runtime_suspend,
			   mtk_hif_dma_runtime_resume, NULL)
};

static const struct of_device_id mtk_hif_dma_of_match[] = {
	{ .compatible = "mediatek,mt6797-hif-dma" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_hif_dma_of_match);

static int mtk_hif_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_hif_dma_dev *hif;
	int ret;

	hif = devm_kzalloc(dev, sizeof(*hif), GFP_KERNEL);
	if (!hif)
		return -ENOMEM;

	hif->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hif->base))
		return PTR_ERR(hif->base);

	hif->irq = platform_get_irq(pdev, 0);
	if (hif->irq < 0)
		return hif->irq;

	hif->clk = devm_clk_get(dev, "apdma");
	if (IS_ERR(hif->clk))
		return dev_err_probe(dev, PTR_ERR(hif->clk),
				     "failed to get APDMA clock\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(33));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	dma_cap_set(DMA_SLAVE, hif->ddev.cap_mask);
	hif->ddev.device_alloc_chan_resources =
		mtk_hif_dma_alloc_chan_resources;
	hif->ddev.device_free_chan_resources =
		mtk_hif_dma_free_chan_resources;
	hif->ddev.device_config = mtk_hif_dma_config;
	hif->ddev.device_prep_slave_sg = mtk_hif_dma_prep_slave_sg;
	hif->ddev.device_issue_pending = mtk_hif_dma_issue_pending;
	hif->ddev.device_tx_status = mtk_hif_dma_tx_status;
	hif->ddev.device_terminate_all = mtk_hif_dma_terminate_all;
	hif->ddev.device_synchronize = mtk_hif_dma_synchronize;
	hif->ddev.src_addr_widths = MTK_HIF_DMA_BUSWIDTHS;
	hif->ddev.dst_addr_widths = MTK_HIF_DMA_BUSWIDTHS;
	hif->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	hif->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	hif->ddev.dev = dev;
	INIT_LIST_HEAD(&hif->ddev.channels);

	hif->vc.desc_free = mtk_hif_dma_desc_free;
	vchan_init(&hif->vc, &hif->ddev);

	platform_set_drvdata(pdev, hif);
	ret = pm_runtime_set_suspended(dev);
	if (ret)
		goto err_vchan;
	pm_runtime_enable(dev);

	ret = dma_async_device_register(&hif->ddev);
	if (ret)
		goto err_pm;

	ret = of_dma_controller_register(dev->of_node,
					 of_dma_xlate_by_chan_id, &hif->ddev);
	if (ret)
		goto err_dma;

	return 0;

err_dma:
	dma_async_device_unregister(&hif->ddev);
err_pm:
	pm_runtime_disable(dev);
err_vchan:
	list_del(&hif->vc.chan.device_node);
	tasklet_kill(&hif->vc.task);
	return ret;
}

static void mtk_hif_dma_remove(struct platform_device *pdev)
{
	struct mtk_hif_dma_dev *hif = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&hif->ddev);
	list_del(&hif->vc.chan.device_node);
	tasklet_kill(&hif->vc.task);
	pm_runtime_disable(&pdev->dev);
}

static struct platform_driver mtk_hif_dma_driver = {
	.probe = mtk_hif_dma_probe,
	.remove = mtk_hif_dma_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mtk_hif_dma_of_match,
		.pm = &mtk_hif_dma_pm_ops,
	},
};
module_platform_driver(mtk_hif_dma_driver);

MODULE_DESCRIPTION("MediaTek MT6797 connectivity HIF DMA driver");
MODULE_LICENSE("GPL");
