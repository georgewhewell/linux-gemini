// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental MT6797 BTIF/APDMA byte transport.
 *
 * The Gemini vendor tree establishes this register and virtual-FIFO contract.
 * It does not establish pad muxing, CONSYS power ownership, firmware loading
 * or a safe STP full-mode transaction. Consequently the Device Tree node is
 * disabled, and an enabled node still needs an explicit module parameter.
 * This driver deliberately has no userspace ABI and sends no controller data.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <linux/soc/mediatek/mt6797-btif.h>
#include <linux/soc/mediatek/mtk-stp.h>

/* BTIF core register offsets and fields. */
#define MT6797_BTIF_IER			0x04
#define MT6797_BTIF_IIR			0x08
#define MT6797_BTIF_FIFOCTRL		0x08
#define MT6797_BTIF_FAKELCR		0x0c
#define MT6797_BTIF_DMA_EN		0x4c
#define MT6797_BTIF_TRI_LVL		0x60
#define MT6797_BTIF_HANDSHAKE		0x6c

#define MT6797_BTIF_IER_RX		BIT(0)
#define MT6797_BTIF_IER_TX		BIT(1)
#define MT6797_BTIF_IIR_NO_INT	BIT(0)
#define MT6797_BTIF_FIFOCLR_RX		BIT(1)
#define MT6797_BTIF_FIFOCLR_TX		BIT(2)
#define MT6797_BTIF_DMA_RX		BIT(0)
#define MT6797_BTIF_DMA_TX		BIT(1)
#define MT6797_BTIF_DMA_AUTORST	BIT(2)
#define MT6797_BTIF_HANDSHAKE_EN	BIT(0)
#define MT6797_BTIF_TRI_TX(n)		(((n) & 0xf) << 0)
#define MT6797_BTIF_TRI_RX(n)		(((n) & 0x7) << 4)

/* APDMA virtual-FIFO register offsets and fields. */
#define MT6797_BTIF_VFF_INT_FLAG	0x00
#define MT6797_BTIF_VFF_INT_EN		0x04
#define MT6797_BTIF_VFF_EN		0x08
#define MT6797_BTIF_VFF_RST		0x0c
#define MT6797_BTIF_VFF_STOP		0x10
#define MT6797_BTIF_VFF_ADDR		0x1c
#define MT6797_BTIF_VFF_LEN		0x24
#define MT6797_BTIF_VFF_THRE		0x28
#define MT6797_BTIF_VFF_WPT		0x2c
#define MT6797_BTIF_VFF_RPT		0x30
#define MT6797_BTIF_VFF_ADDR_H		0x54

#define MT6797_BTIF_VFF_EN_BIT		BIT(0)
#define MT6797_BTIF_VFF_WARM_RST	BIT(0)
#define MT6797_BTIF_VFF_STOP_BIT	BIT(0)
#define MT6797_BTIF_VFF_TX_INT		BIT(0)
#define MT6797_BTIF_VFF_RX_INT		(BIT(0) | BIT(1))

#define MT6797_BTIF_POLL_DELAY_US	10
#define MT6797_BTIF_POLL_TIMEOUT_US	100000
#define MT6797_BTIF_TX_THRESHOLD	(MT6797_BTIF_VFIFO_SIZE - 7)
#define MT6797_BTIF_RX_THRESHOLD	((MT6797_BTIF_VFIFO_SIZE * 3) / 4)

static bool enable_experimental_transport;
module_param_named(enable_experimental_transport,
		   enable_experimental_transport, bool, 0400);
MODULE_PARM_DESC(enable_experimental_transport,
		 "allow a DT-marked MT6797 BTIF/APDMA experiment (default: false)");

struct mt6797_btif_transport {
	struct device *dev;
	void __iomem *core;
	void __iomem *tx_dma;
	void __iomem *rx_dma;
	struct clk *btifc_clk;
	struct clk *apdmac_clk;
	struct mt6797_btif_vfifo tx_fifo;
	struct mt6797_btif_vfifo rx_fifo;
	struct mtk_stp stp;
	/* Protects started and the software RX virtual-FIFO pointer. */
	spinlock_t state_lock;
	int core_irq;
	int tx_irq;
	int rx_irq;
	bool started;
};

static bool mt6797_btif_vfifo_pointer_valid(u32 pointer)
{
	return !(pointer & ~(MT6797_BTIF_VFIFO_PTR_MASK |
			      MT6797_BTIF_VFIFO_WRAP)) &&
		(pointer & MT6797_BTIF_VFIFO_PTR_MASK) < MT6797_BTIF_VFIFO_SIZE;
}

int mt6797_btif_vfifo_advance(u32 pointer, size_t count, u32 *next)
{
	u32 offset;
	u32 wrap;

	if (!next || !mt6797_btif_vfifo_pointer_valid(pointer) ||
	    count > MT6797_BTIF_VFIFO_SIZE)
		return -EINVAL;

	offset = pointer & MT6797_BTIF_VFIFO_PTR_MASK;
	wrap = pointer & MT6797_BTIF_VFIFO_WRAP;
	offset += count;
	if (offset >= MT6797_BTIF_VFIFO_SIZE) {
		offset -= MT6797_BTIF_VFIFO_SIZE;
		wrap ^= MT6797_BTIF_VFIFO_WRAP;
	}

	*next = offset | wrap;
	return 0;
}

int mt6797_btif_vfifo_tx_copy(struct mt6797_btif_vfifo *fifo, u32 hw_wpt,
			      u32 hw_left, const u8 *data, size_t length,
			      u32 *next_wpt)
{
	size_t first_len;
	u32 offset;
	int ret;

	if (!fifo || !fifo->cpu_addr || !next_wpt || (!data && length) ||
	    !mt6797_btif_vfifo_pointer_valid(hw_wpt) ||
	    hw_left > MT6797_BTIF_VFIFO_SIZE)
		return -EINVAL;
	if (length > MT6797_BTIF_VFIFO_SIZE)
		return -EMSGSIZE;
	if (length > hw_left)
		return -EAGAIN;

	offset = hw_wpt & MT6797_BTIF_VFIFO_PTR_MASK;
	first_len = min_t(size_t, length, MT6797_BTIF_VFIFO_SIZE - offset);
	if (first_len)
		memcpy(fifo->cpu_addr + offset, data, first_len);
	if (length > first_len)
		memcpy(fifo->cpu_addr, data + first_len, length - first_len);

	/* Publish payload stores before the caller publishes WPT to the DMA. */
	dma_wmb();
	ret = mt6797_btif_vfifo_advance(hw_wpt, length, next_wpt);
	if (ret)
		return ret;
	fifo->wpt = *next_wpt;
	return 0;
}

int mt6797_btif_vfifo_rx_segments(const struct mt6797_btif_vfifo *fifo,
				  u32 hw_wpt,
				  struct mt6797_btif_rx_segments *segments)
{
	u32 rpt;
	u32 wpt;
	size_t length;
	size_t first_len;

	if (!fifo || !fifo->cpu_addr || !segments ||
	    !mt6797_btif_vfifo_pointer_valid(fifo->rpt) ||
	    !mt6797_btif_vfifo_pointer_valid(hw_wpt))
		return -EINVAL;

	memset(segments, 0, sizeof(*segments));
	rpt = fifo->rpt & MT6797_BTIF_VFIFO_PTR_MASK;
	wpt = hw_wpt & MT6797_BTIF_VFIFO_PTR_MASK;
	if ((fifo->rpt ^ hw_wpt) & MT6797_BTIF_VFIFO_WRAP)
		length = MT6797_BTIF_VFIFO_SIZE - rpt + wpt;
	else if (wpt >= rpt)
		length = wpt - rpt;
	else
		return -EOVERFLOW;

	first_len = min_t(size_t, length, MT6797_BTIF_VFIFO_SIZE - rpt);
	segments->first = fifo->cpu_addr + rpt;
	segments->first_len = first_len;
	if (length > first_len) {
		segments->second = fifo->cpu_addr;
		segments->second_len = length - first_len;
	}
	segments->next_rpt = hw_wpt;
	return 0;
}

static void mt6797_btif_update_bits(void __iomem *base, u32 offset,
				    u32 mask, u32 value)
{
	u32 regval;

	regval = readl(base + offset);
	regval &= ~mask;
	regval |= value & mask;
	writel(regval, base + offset);
}

static int mt6797_btif_dma_warm_reset(void __iomem *base)
{
	u32 value;

	mt6797_btif_update_bits(base, MT6797_BTIF_VFF_RST,
				MT6797_BTIF_VFF_WARM_RST,
				MT6797_BTIF_VFF_WARM_RST);
	return readl_poll_timeout(base + MT6797_BTIF_VFF_EN, value,
				  !(value & MT6797_BTIF_VFF_EN_BIT),
				  MT6797_BTIF_POLL_DELAY_US,
				  MT6797_BTIF_POLL_TIMEOUT_US);
}

static int mt6797_btif_dma_stop(void __iomem *base)
{
	u32 value;

	mt6797_btif_update_bits(base, MT6797_BTIF_VFF_STOP,
				MT6797_BTIF_VFF_STOP_BIT,
				MT6797_BTIF_VFF_STOP_BIT);
	return readl_poll_timeout(base + MT6797_BTIF_VFF_STOP, value,
				  !(value & MT6797_BTIF_VFF_STOP_BIT),
				  MT6797_BTIF_POLL_DELAY_US,
				  MT6797_BTIF_POLL_TIMEOUT_US);
}

static int mt6797_btif_dma_program(void __iomem *base,
				   const struct mt6797_btif_vfifo *fifo,
				   u32 threshold, u32 int_flags)
{
	if (upper_32_bits(fifo->dma_addr) ||
	    !IS_ALIGNED(fifo->dma_addr, MT6797_BTIF_VFIFO_ALIGNMENT))
		return -ERANGE;

	writel(lower_32_bits(fifo->dma_addr), base + MT6797_BTIF_VFF_ADDR);
	writel(0, base + MT6797_BTIF_VFF_ADDR_H);
	writel(MT6797_BTIF_VFIFO_SIZE, base + MT6797_BTIF_VFF_LEN);
	writel(threshold, base + MT6797_BTIF_VFF_THRE);
	writel(0, base + MT6797_BTIF_VFF_WPT);
	writel(0, base + MT6797_BTIF_VFF_RPT);
	writel(0, base + MT6797_BTIF_VFF_INT_EN);
	writel(int_flags, base + MT6797_BTIF_VFF_INT_FLAG);
	return 0;
}

static int mt6797_btif_alloc_vfifo(struct device *dev,
				   struct mt6797_btif_vfifo *fifo)
{
	dma_addr_t *dma_addr = &fifo->dma_addr;
	void *cpu_addr;

	cpu_addr = dmam_alloc_coherent(dev, MT6797_BTIF_VFIFO_SIZE, dma_addr, GFP_KERNEL);
	if (!cpu_addr)
		return -ENOMEM;
	fifo->cpu_addr = cpu_addr;
	return 0;
}

static void mt6797_btif_core_init(struct mt6797_btif_transport *btif)
{
	void __iomem *base = btif->core;

	/* Exact vendor normal-mode/new-handshake and FIFO-clear ordering. */
	writel(0, base + MT6797_BTIF_FAKELCR);
	mt6797_btif_update_bits(base, MT6797_BTIF_HANDSHAKE,
				MT6797_BTIF_HANDSHAKE_EN,
				MT6797_BTIF_HANDSHAKE_EN);
	mt6797_btif_update_bits(base, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_RX,
				MT6797_BTIF_FIFOCLR_RX);
	mt6797_btif_update_bits(base, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_RX, 0);
	mt6797_btif_update_bits(base, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_TX,
				MT6797_BTIF_FIFOCLR_TX);
	mt6797_btif_update_bits(base, MT6797_BTIF_FIFOCTRL,
				MT6797_BTIF_FIFOCLR_TX, 0);
	writel(MT6797_BTIF_TRI_TX(8) | MT6797_BTIF_TRI_RX(1),
	       base + MT6797_BTIF_TRI_LVL);
	mt6797_btif_update_bits(base, MT6797_BTIF_DMA_EN,
				MT6797_BTIF_DMA_RX | MT6797_BTIF_DMA_TX |
				MT6797_BTIF_DMA_AUTORST,
				MT6797_BTIF_DMA_AUTORST);
	mt6797_btif_update_bits(base, MT6797_BTIF_IER,
				MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX,
				MT6797_BTIF_IER_RX);
}

static irqreturn_t mt6797_btif_core_irq(int irq, void *data)
{
	struct mt6797_btif_transport *btif = data;
	u32 iir;

	iir = readl(btif->core + MT6797_BTIF_IIR);
	if (iir & MT6797_BTIF_IIR_NO_INT)
		return IRQ_NONE;

	/* A DMA-mode core IRQ is unexpected; mask it rather than spin forever. */
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_IER,
				MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX, 0);
	dev_warn_ratelimited(btif->dev,
			     "unexpected BTIF core IRQ %#x; core IRQs masked\n", iir);
	return IRQ_HANDLED;
}

static irqreturn_t mt6797_btif_tx_irq(int irq, void *data)
{
	struct mt6797_btif_transport *btif = data;

	/* TX completion is not consumed until a future STP TX owner exists. */
	mt6797_btif_update_bits(btif->tx_dma, MT6797_BTIF_VFF_INT_EN,
				MT6797_BTIF_VFF_TX_INT, 0);
	mt6797_btif_update_bits(btif->tx_dma, MT6797_BTIF_VFF_INT_FLAG,
				MT6797_BTIF_VFF_TX_INT, 0);
	return IRQ_HANDLED;
}

static irqreturn_t mt6797_btif_rx_irq(int irq, void *data)
{
	struct mt6797_btif_transport *btif = data;
	unsigned long flags;

	spin_lock_irqsave(&btif->state_lock, flags);
	if (!btif->started) {
		spin_unlock_irqrestore(&btif->state_lock, flags);
		return IRQ_NONE;
	}
	mt6797_btif_update_bits(btif->rx_dma, MT6797_BTIF_VFF_INT_EN,
				MT6797_BTIF_VFF_RX_INT, 0);
	/* RX flags are write-one-to-clear, as in the matching APDMA driver. */
	writel(MT6797_BTIF_VFF_RX_INT,
	       btif->rx_dma + MT6797_BTIF_VFF_INT_FLAG);
	spin_unlock_irqrestore(&btif->state_lock, flags);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mt6797_btif_rx_thread(int irq, void *data)
{
	struct mt6797_btif_transport *btif = data;
	struct mt6797_btif_rx_segments segments;
	unsigned long flags;
	u32 wpt;
	int ret;

	spin_lock_irqsave(&btif->state_lock, flags);
	if (!btif->started) {
		spin_unlock_irqrestore(&btif->state_lock, flags);
		return IRQ_HANDLED;
	}
	wpt = readl(btif->rx_dma + MT6797_BTIF_VFF_WPT);
	ret = mt6797_btif_vfifo_rx_segments(&btif->rx_fifo, wpt, &segments);
	spin_unlock_irqrestore(&btif->state_lock, flags);
	if (ret) {
		dev_warn_ratelimited(btif->dev,
				     "invalid RX virtual-FIFO pointers: %d\n", ret);
		goto out_reenable;
	}

	/* RX DMA owns payload bytes until WPT is observed. */
	dma_rmb();
	if (segments.first_len)
		ret = mtk_stp_rx(&btif->stp, segments.first, segments.first_len);
	if (!ret && segments.second_len)
		ret = mtk_stp_rx(&btif->stp, segments.second, segments.second_len);
	if (ret)
		dev_warn_ratelimited(btif->dev, "STP RX parser returned %d\n", ret);

	spin_lock_irqsave(&btif->state_lock, flags);
	if (btif->started) {
		btif->rx_fifo.rpt = segments.next_rpt;
		writel(segments.next_rpt,
		       btif->rx_dma + MT6797_BTIF_VFF_RPT);
	}
	spin_unlock_irqrestore(&btif->state_lock, flags);

out_reenable:
	spin_lock_irqsave(&btif->state_lock, flags);
	if (btif->started)
		mt6797_btif_update_bits(btif->rx_dma, MT6797_BTIF_VFF_INT_EN,
					MT6797_BTIF_VFF_RX_INT,
					MT6797_BTIF_VFF_RX_INT);
	spin_unlock_irqrestore(&btif->state_lock, flags);
	return IRQ_HANDLED;
}

static void mt6797_btif_disable(struct mt6797_btif_transport *btif)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&btif->state_lock, flags);
	if (!btif->started) {
		spin_unlock_irqrestore(&btif->state_lock, flags);
		return;
	}
	btif->started = false;
	mt6797_btif_update_bits(btif->rx_dma, MT6797_BTIF_VFF_INT_EN,
				MT6797_BTIF_VFF_RX_INT, 0);
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_IER,
				MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX, 0);
	spin_unlock_irqrestore(&btif->state_lock, flags);

	disable_irq(btif->rx_irq);
	disable_irq(btif->tx_irq);
	disable_irq(btif->core_irq);
	ret = mt6797_btif_dma_stop(btif->rx_dma);
	if (ret)
		dev_warn(btif->dev, "RX APDMA stop timed out: %d\n", ret);
	ret = mt6797_btif_dma_stop(btif->tx_dma);
	if (ret)
		dev_warn(btif->dev, "TX APDMA stop timed out: %d\n", ret);
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_DMA_EN,
				MT6797_BTIF_DMA_RX | MT6797_BTIF_DMA_TX, 0);
	clk_disable_unprepare(btif->apdmac_clk);
	clk_disable_unprepare(btif->btifc_clk);
}

static int mt6797_btif_enable(struct mt6797_btif_transport *btif)
{
	unsigned long flags;
	int ret;

	ret = clk_prepare_enable(btif->btifc_clk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(btif->apdmac_clk);
	if (ret)
		goto err_btif_clk;

	mt6797_btif_core_init(btif);
	ret = mt6797_btif_dma_warm_reset(btif->tx_dma);
	if (ret)
		goto err_clks;
	ret = mt6797_btif_dma_warm_reset(btif->rx_dma);
	if (ret)
		goto err_clks;
	ret = mt6797_btif_dma_program(btif->tx_dma, &btif->tx_fifo,
				      MT6797_BTIF_TX_THRESHOLD, 0);
	if (ret)
		goto err_clks;
	ret = mt6797_btif_dma_program(btif->rx_dma, &btif->rx_fifo,
				      MT6797_BTIF_RX_THRESHOLD,
				      MT6797_BTIF_VFF_RX_INT);
	if (ret)
		goto err_clks;

	mtk_stp_init(&btif->stp, MTK_STP_MODE_MANDATORY);
	spin_lock_irqsave(&btif->state_lock, flags);
	btif->started = true;
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_DMA_EN,
				MT6797_BTIF_DMA_RX, MT6797_BTIF_DMA_RX);
	writel(MT6797_BTIF_VFF_EN_BIT,
	       btif->rx_dma + MT6797_BTIF_VFF_EN);
	mt6797_btif_update_bits(btif->rx_dma, MT6797_BTIF_VFF_INT_EN,
				MT6797_BTIF_VFF_RX_INT,
				MT6797_BTIF_VFF_RX_INT);
	spin_unlock_irqrestore(&btif->state_lock, flags);

	return 0;

err_clks:
	/* Do not leave partially initialized DMA or core interrupts behind. */
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_DMA_EN,
				MT6797_BTIF_DMA_RX | MT6797_BTIF_DMA_TX, 0);
	mt6797_btif_update_bits(btif->core, MT6797_BTIF_IER,
				MT6797_BTIF_IER_RX | MT6797_BTIF_IER_TX, 0);
	clk_disable_unprepare(btif->apdmac_clk);
err_btif_clk:
	clk_disable_unprepare(btif->btifc_clk);
	return ret;
}

static int mt6797_btif_transport_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_btif_transport *btif;
	struct device_node *consys;
	int ret;

	if (!enable_experimental_transport ||
	    !of_property_read_bool(dev->of_node,
				   "mediatek,experimental-transport")) {
		dev_info(dev, "transport blocked: need DT experiment flag and module parameter\n");
		return -EPERM;
	}

	/* This phandle records the owner; this driver never votes power itself. */
	consys = of_parse_phandle(dev->of_node, "mediatek,consys", 0);
	if (!consys)
		return dev_err_probe(dev, -EINVAL, "missing CONSYS phandle\n");
	of_node_put(consys);

	btif = devm_kzalloc(dev, sizeof(*btif), GFP_KERNEL);
	if (!btif)
		return -ENOMEM;
	btif->dev = dev;
	spin_lock_init(&btif->state_lock);

	btif->core = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(btif->core))
		return PTR_ERR(btif->core);
	btif->tx_dma = devm_platform_ioremap_resource_byname(pdev, "tx-dma");
	if (IS_ERR(btif->tx_dma))
		return PTR_ERR(btif->tx_dma);
	btif->rx_dma = devm_platform_ioremap_resource_byname(pdev, "rx-dma");
	if (IS_ERR(btif->rx_dma))
		return PTR_ERR(btif->rx_dma);

	btif->btifc_clk = devm_clk_get(dev, "btifc");
	if (IS_ERR(btif->btifc_clk))
		return dev_err_probe(dev, PTR_ERR(btif->btifc_clk),
				     "failed to get BTIF clock\n");
	btif->apdmac_clk = devm_clk_get(dev, "apdmac");
	if (IS_ERR(btif->apdmac_clk))
		return dev_err_probe(dev, PTR_ERR(btif->apdmac_clk),
				     "failed to get APDMA clock\n");

	btif->core_irq = platform_get_irq_byname(pdev, "core");
	if (btif->core_irq < 0)
		return dev_err_probe(dev, btif->core_irq, "missing core IRQ\n");
	btif->tx_irq = platform_get_irq_byname(pdev, "tx");
	if (btif->tx_irq < 0)
		return dev_err_probe(dev, btif->tx_irq, "missing TX DMA IRQ\n");
	btif->rx_irq = platform_get_irq_byname(pdev, "rx");
	if (btif->rx_irq < 0)
		return dev_err_probe(dev, btif->rx_irq, "missing RX DMA IRQ\n");

	/* The vendor allocator explicitly constrained these rings to DMA32. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "cannot constrain APDMA to 32-bit DMA\n");
	ret = mt6797_btif_alloc_vfifo(dev, &btif->tx_fifo);
	if (ret)
		return ret;
	ret = mt6797_btif_alloc_vfifo(dev, &btif->rx_fifo);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, btif->core_irq, mt6797_btif_core_irq, 0,
			       dev_name(dev), btif);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request core IRQ\n");
	ret = devm_request_irq(dev, btif->tx_irq, mt6797_btif_tx_irq, 0,
			       dev_name(dev), btif);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request TX DMA IRQ\n");
	ret = devm_request_threaded_irq(dev, btif->rx_irq, mt6797_btif_rx_irq,
					mt6797_btif_rx_thread, IRQF_ONESHOT,
					dev_name(dev), btif);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request RX DMA IRQ\n");

	platform_set_drvdata(pdev, btif);
	ret = mt6797_btif_enable(btif);
	if (ret)
		return dev_err_probe(dev, ret, "BTIF/APDMA startup failed\n");

	dev_info(dev,
		 "experimental mandatory-STP RX transport started; no firmware or TX enabled\n");
	return 0;
}

static void mt6797_btif_transport_remove(struct platform_device *pdev)
{
	mt6797_btif_disable(platform_get_drvdata(pdev));
}

static const struct of_device_id mt6797_btif_transport_of_match[] = {
	{ .compatible = "mediatek,mt6797-btif-transport" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_btif_transport_of_match);

static struct platform_driver mt6797_btif_transport_driver = {
	.probe = mt6797_btif_transport_probe,
	.remove = mt6797_btif_transport_remove,
	.driver = {
		.name = "mt6797-btif-transport",
		.of_match_table = mt6797_btif_transport_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_btif_transport_driver);

MODULE_DESCRIPTION("Experimental MT6797 BTIF/APDMA byte transport");
MODULE_LICENSE("GPL");
