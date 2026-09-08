// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 MD1 CCCI transport over the on-SoC CLDMA engine. */

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/soc/mediatek/mt6797-md1-cldma.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md1-fs-provider.h>
#include <linux/soc/mediatek/mt6797-md1-hs.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/string.h>
#include <linux/wwan.h>
#include <linux/workqueue.h>

#include "mtk_ccci_common.h"
#include "mtk_mt6797_ccci_diag.h"
#include "mtk_mt6797_ccci_rpc.h"
#include "mtk_mt6797_ccci_fs.h"
#include "mtk_mt6797_ccci_fs_diag.h"

#define MT6797_CCCI_QUEUE_COUNT		7U
#define MT6797_CCCI_RING_SIZE		16U
#define MT6797_CCCI_RX_SIZE		3584U
#define MT6797_CCCI_NAPI_WEIGHT		64
#define MT6797_CCCI_CHANNEL_COUNT	256U
#define MT6797_CCCI_HANDSHAKE_TIMEOUT	(30 * HZ)
#define MT6797_CCCI_ACTIVE_IRQS \
	(MT6797_MD1_CLDMA_ACTIVE_QUEUES | \
	 (MT6797_MD1_CLDMA_ACTIVE_QUEUES << 16))

#define CLDMA_UL_START_ADDR(q)		(0x0004 + 4 * (q))
#define CLDMA_UL_START_CMD		0x0054
#define CLDMA_UL_RESUME_CMD		0x0058
#define CLDMA_UL_STOP_CMD		0x005c
#define CLDMA_UL_STATUS			0x0050
#define CLDMA_UL_CFG			0x0074
#define CLDMA_UL_CHECKSUM_ENABLE		0x0140
#define CLDMA_SO_START_CMD		0x03bc
#define CLDMA_SO_RESUME_CMD		0x03c0
#define CLDMA_SO_STOP_CMD		0x03c4
#define CLDMA_L2_TX_STATUS		0x0410
#define CLDMA_L2_TX_MASK		0x0418
#define CLDMA_L2_TX_MASK_CLEAR		0x0420
#define CLDMA_L2_TX_MASK_SET		0x0428
#define CLDMA_L2_RX_STATUS		0x0450
#define CLDMA_L3_TX_MASK_CLEAR0		0x0440
#define CLDMA_L3_TX_MASK_CLEAR1		0x0444
#define CLDMA_L3_RX_MASK_CLEAR0		0x0480
#define CLDMA_L3_RX_MASK_CLEAR1		0x0484

#define CLDMA_SO_CFG			0x0a04
#define CLDMA_SO_CHECKSUM_ENABLE		0x0a74
#define CLDMA_SO_START_ADDR(q)		(0x0a78 + 4 * (q))
#define CLDMA_SO_STATUS			0x0ab8
#define CLDMA_DEBUG_ID_ENABLE		0x0ac8
#define CLDMA_L2_RX_MASK		0x0c58
#define CLDMA_L2_RX_MASK_CLEAR		0x0c60
#define CLDMA_L2_RX_MASK_SET		0x0c68
#define CLDMA_BUS_CFG			0x0c90

static const char * const mt6797_ccci_region_names[] = {
	"ap-cldma-ao", "md-cldma-ao", "ap-cldma-pdn", "md-cldma-pdn",
	"ap-ccif", "md-ccif",
};

static const enum mt6797_md1_cldma_irq_type mt6797_ccci_irq_types[] = {
	MT6797_MD1_CLDMA_IRQ_LEVEL_HIGH,
	MT6797_MD1_CLDMA_IRQ_LEVEL_LOW,
	MT6797_MD1_CLDMA_IRQ_EDGE_FALLING,
};

static const u32 mt6797_ccci_irq_flags[] = {
	IRQ_TYPE_LEVEL_HIGH, IRQ_TYPE_LEVEL_LOW, IRQ_TYPE_EDGE_FALLING,
};

struct mt6797_ccci_slot {
	void *gpd;
	dma_addr_t gpd_dma;
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	bool mapped;
};

struct mt6797_ccci_queue {
	struct mt6797_ccci_slot tx[MT6797_CCCI_RING_SIZE];
	struct mt6797_ccci_slot rx[MT6797_CCCI_RING_SIZE];
	unsigned int tx_head;
	unsigned int tx_tail;
	unsigned int tx_used;
	unsigned int rx_tail;
	/* Protects the TX producer, consumer and descriptor ownership transfer. */
	spinlock_t tx_lock;
};

struct mt6797_ccci;

struct mt6797_ccci_link {
	struct mt6797_ccci *ccci;
	struct net_device *netdev;
	u32 if_id;
	u16 tx_channel;
	u8 tx_queue;
};

struct mt6797_ccci {
	struct mt6797_ccci_diag diag;
	struct device *dev;
	void __iomem *ao;
	void __iomem *pdn;
	void __iomem *smem;
	resource_size_t smem_size;
	struct dma_pool *gpd_pool;
	struct mt6797_ccci_queue queue[MT6797_CCCI_QUEUE_COUNT];
	/* queue.tx_lock -> tx_stop_lock; STOP never takes queue.tx_lock. */
	spinlock_t tx_stop_lock;
	struct mt6797_rpc_eint sim_eint[2];
	atomic_t rpc_logs;
	atomic_t fs_logs;
	struct mt6797_fs_diag fs_diag;
	atomic_t fs_pending;
	unsigned long fs_indices;
	struct workqueue_struct *fs_wq;
	struct mt6797_fs_service fs_service; /* fs_wq worker only */
	struct net_device *napi_dev;
	struct napi_struct napi;
	struct mt6797_md1_hs hs;
	struct work_struct hs1_work;
	struct delayed_work handshake_timeout_work;
	struct sk_buff *hs1_skb;
	struct mtk_ccci_runtime_config runtime_config;
	struct wwan_port *at_port;
	struct mt6797_ccci_link __rcu *link[MTK_CCCI_MAX_NET_LINKS];
	u8 *runtime;
	atomic_t tx_pending;
	atomic_t rx_pending;
	/* Mask ownership survives repolls independently of pending work. */
	atomic_t tx_mask_owned;
	atomic_t rx_mask_owned;
	u64 napi_mask_generation; /* tx_stop_lock; no practical per-boot wrap */
	atomic_t failed;
	atomic_t ready;
	atomic_t at_open;
	atomic_t hs1_pending;
	atomic_t teardown;
	atomic_t tx_sequence[MT6797_CCCI_CHANNEL_COUNT];
	s16 rx_sequence[MT6797_CCCI_CHANNEL_COUNT];
	int cldma_irq;
	int ccif_irq;
	int wdt_irq;
	bool started;
};

static bool enable_mt6797_ccci;
module_param_named(enable, enable_mt6797_ccci, bool, 0400);
MODULE_PARM_DESC(enable, "allow MT6797 CCCI to take active CLDMA ownership");

static u16 mt6797_ccci_next_sequence(struct mt6797_ccci *ccci, u16 channel)
{
	return (u16)atomic_fetch_inc(&ccci->tx_sequence[channel]);
}

static void mt6797_ccci_frontend_ready(struct mt6797_ccci *ccci, bool ready)
{
	struct wwan_port *port = READ_ONCE(ccci->at_port);
	unsigned int i;

	atomic_set(&ccci->ready, ready);
	if (ready && atomic_read(&ccci->failed)) {
		atomic_set(&ccci->ready, 0);
		ready = false;
	}
	if (port) {
		if (ready)
			wwan_port_txon(port);
		else
			wwan_port_txoff(port);
	}
	rcu_read_lock();
	for (i = 0; i < MTK_CCCI_MAX_NET_LINKS; i++) {
		struct mt6797_ccci_link *link;

		link = rcu_dereference(ccci->link[i]);
		if (!link || !netif_running(link->netdev))
			continue;
		if (ready) {
			netif_carrier_on(link->netdev);
			netif_wake_queue(link->netdev);
		} else {
			netif_carrier_off(link->netdev);
			netif_stop_queue(link->netdev);
		}
	}
	rcu_read_unlock();
}

static void mt6797_ccci_free_netdev(void *data)
{
	free_netdev(data);
}

static void mt6797_ccci_napi_del(void *data)
{
	netif_napi_del(data);
}

static void mt6797_ccci_remove_port(void *data)
{
	wwan_remove_port(data);
}

static void mt6797_ccci_unregister_wwan(void *data)
{
	wwan_unregister_ops(data);
}

static void mt6797_ccci_first_fatal(struct mt6797_ccci *ccci, const char *site)
{
	struct mt6797_ccci_diag *d = &ccci->diag;
	unsigned int i;

	if (!mt6797_ccci_diag_freeze(d, ccci->queue[0].tx[0].gpd))
		return;
	mt6797_fs_diag_freeze(&ccci->fs_diag);
	dev_err(ccci->dev,
		"MT6797_CCCI_FIRST_FATAL site=%s ready=%d irqs=%u first_tx=%08x/%08x first_rx=%08x/%08x last_tx=%08x/%08x last_rx=%08x/%08x q0_submitted=%u reclaimed=%u head=%u tail=%u used=%u descriptor_valid=%u payload_valid=%u reply_finalized=%u runtime_len=%zu\n",
		site, atomic_read(&ccci->ready), d->irq_count,
		d->first_irq[0], d->first_irq[1], d->first_irq[2], d->first_irq[3],
		d->last_irq[0], d->last_irq[1], d->last_irq[2], d->last_irq[3],
		d->submitted, d->reclaimed, d->head, d->tail, d->used,
		d->descriptor_valid, d->payload_valid, d->reply_finalized, d->runtime_len);
	/* Freeze before printing: IRQ may have interrupted an in-flight request.
	 * Readers below see immutable metadata; printk never holds fs_diag.lock.
	 */
	dev_err(ccci->dev, "CCCI_FS_RECENT total=%llu count=%u pending=%d\n",
		(unsigned long long)ccci->fs_diag.total, ccci->fs_diag.count,
		atomic_read(&ccci->fs_pending));
	for (i = 0; i < ccci->fs_diag.count; i++) {
		unsigned int slot = (ccci->fs_diag.head + MT6797_FS_DIAG_COUNT -
				     ccci->fs_diag.count + i) % MT6797_FS_DIAG_COUNT;
		const struct mt6797_fs_diag_entry *e = &ccci->fs_diag.entry[slot];

		dev_err(ccci->dev,
			"CCCI_FS_RECENT seq=%llu op=%x parse=%d compact=%u flags=%x requested=%u open_valid=%u open_flags=%x done=%u status=%d result=%d sent=%zu\n",
			(unsigned long long)e->sequence, e->op, e->parse_error,
			e->compact, e->flags, e->requested, e->open_valid, e->open_flags,
			e->done, e->status,
			e->result, e->sent);
	}
	if (d->descriptor_valid)
		print_hex_dump(KERN_ERR, "CCCI_Q0_GPD ", DUMP_PREFIX_OFFSET,
			       32, 1, d->descriptor, sizeof(d->descriptor), false);
	if (d->payload_valid) {
		print_hex_dump(KERN_ERR, "CCCI_HS1 ", DUMP_PREFIX_OFFSET,
			       32, 1, d->hs1, sizeof(d->hs1), false);
		print_hex_dump(KERN_ERR, "CCCI_AP_REPLY ", DUMP_PREFIX_OFFSET,
			       32, 1, d->reply, sizeof(d->reply), false);
		print_hex_dump(KERN_ERR, "CCCI_RUNTIME ", DUMP_PREFIX_OFFSET,
			       32, 1, d->runtime, d->runtime_len, false);
	}
	dev_err(ccci->dev, "CCCI_PREREADY_RX_COUNT count=%u cap=4 bytes_per_frame=1024\n",
		d->rx_count);
	for (i = 0; i < d->rx_count; i++) {
		const struct mt6797_ccci_diag_rx *r = &d->rx[i];
		char prefix[32];

		dev_err(ccci->dev,
			"CCCI_PREREADY_RX index=%u queue=%u drop=%u length=%zu captured=%zu truncated=%u decode_error=%d header_valid=%u channel=%u payload_word0_valid=%u payload_word0=%08x\n",
			i, r->queue, r->cause, r->length, r->captured, r->truncated,
			r->decode_error, r->header_valid, r->channel,
			r->payload_word0_valid, r->payload_word0);
		snprintf(prefix, sizeof(prefix), "CCCI_PREREADY_RX%u ", i);
		print_hex_dump(KERN_ERR, prefix, DUMP_PREFIX_OFFSET,
			       32, 1, r->bytes, r->captured, false);
	}
}

static void mt6797_ccci_quiesce_from(struct mt6797_ccci *ccci, int reason,
				     const char *site)
{
	unsigned long flags;

	if (atomic_xchg(&ccci->failed, 1))
		return;

	if (reason != -ESHUTDOWN) {
		mt6797_ccci_first_fatal(ccci, site);
		dev_err(ccci->dev, "MT6797_CCCI_TERMINAL_FAILURE reason=%d\n", reason);
	}

	mt6797_ccci_frontend_ready(ccci, false);
	spin_lock_irqsave(&ccci->tx_stop_lock, flags);
	writel(MT6797_CCCI_ACTIVE_IRQS, ccci->pdn + CLDMA_L2_TX_MASK_SET);
	writel(MT6797_CCCI_ACTIVE_IRQS, ccci->ao + CLDMA_L2_RX_MASK_SET);
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_UL_STOP_CMD);
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_SO_STOP_CMD);
	readl(ccci->pdn + CLDMA_SO_STOP_CMD);
	spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
	mt6797_md1_hs_crash(&ccci->hs, reason);
	if (reason != -ESHUTDOWN)
		mt6797_md1_cold_owner_transport_fatal(reason);
}

static void mt6797_ccci_quiesce(struct mt6797_ccci *ccci, int reason)
{
	mt6797_ccci_quiesce_from(ccci, reason, "transport");
}

static void mt6797_ccci_free_rings(void *data)
{
	struct mt6797_ccci *ccci = data;
	void __iomem *so_status = ccci->ao + CLDMA_SO_STATUS;
	u32 status;
	unsigned int q, i;
	int ret;

	if (ccci->started) {
		mt6797_ccci_quiesce(ccci, -ESHUTDOWN);
		ret = readl_poll_timeout(ccci->pdn + CLDMA_UL_STATUS, status,
					 !(status & MT6797_MD1_CLDMA_ACTIVE_QUEUES),
					 10, 100000);
		if (!ret)
			ret = readl_poll_timeout(so_status, status,
						 !(status & MT6797_MD1_CLDMA_ACTIVE_QUEUES),
						 10, 100000);
		if (ret) {
			dev_err(ccci->dev,
				"CLDMA did not stop; retaining DMA mappings\n");
			return;
		}
	}
	for (q = 0; q < MT6797_CCCI_QUEUE_COUNT; q++) {
		for (i = 0; i < MT6797_CCCI_RING_SIZE; i++) {
			struct mt6797_ccci_slot *tx = &ccci->queue[q].tx[i];
			struct mt6797_ccci_slot *rx = &ccci->queue[q].rx[i];

			if (tx->mapped)
				dma_unmap_single(ccci->dev, tx->skb_dma,
						 tx->skb->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(tx->skb);
			if (rx->mapped)
				dma_unmap_single(ccci->dev, rx->skb_dma,
						 MT6797_CCCI_RX_SIZE, DMA_FROM_DEVICE);
			dev_kfree_skb_any(rx->skb);
			if (tx->gpd)
				dma_pool_free(ccci->gpd_pool, tx->gpd, tx->gpd_dma);
			if (rx->gpd)
				dma_pool_free(ccci->gpd_pool, rx->gpd, rx->gpd_dma);
		}
	}
	dma_pool_destroy(ccci->gpd_pool);
}

static struct sk_buff *mt6797_ccci_alloc_rx(struct mt6797_ccci *ccci, gfp_t gfp)
{
	struct sk_buff *skb;

	skb = __netdev_alloc_skb(ccci->napi_dev,
				 MT6797_CCCI_RX_SIZE + NET_IP_ALIGN, gfp);
	if (skb)
		skb_reserve(skb, NET_IP_ALIGN);
	return skb;
}

static int mt6797_ccci_alloc_rings(struct mt6797_ccci *ccci)
{
	struct mt6797_md1_cldma_tgpd *tx_gpd[MT6797_CCCI_RING_SIZE];
	struct mt6797_md1_cldma_rgpd *rx_gpd[MT6797_CCCI_RING_SIZE];
	dma_addr_t tx_dma[MT6797_CCCI_RING_SIZE];
	dma_addr_t rx_gpd_dma[MT6797_CCCI_RING_SIZE];
	dma_addr_t rx_dma[MT6797_CCCI_RING_SIZE];
	u16 rx_size[MT6797_CCCI_RING_SIZE];
	const size_t ring_size = MT6797_CCCI_RING_SIZE;
	unsigned int q, i;
	int ret;

	ret = dma_set_mask(ccci->dev, DMA_BIT_MASK(MT6797_MD1_CLDMA_ADDR_BITS));
	if (ret)
		return ret;
	ret = dma_set_coherent_mask(ccci->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ccci->gpd_pool = dma_pool_create("mt6797-ccci-gpd", ccci->dev,
					 MT6797_MD1_CLDMA_GPD_BYTES,
					 MT6797_MD1_CLDMA_GPD_ALIGN, 0);
	if (!ccci->gpd_pool)
		return -ENOMEM;
	ret = devm_add_action_or_reset(ccci->dev, mt6797_ccci_free_rings, ccci);
	if (ret)
		return ret;

	for (q = 0; q < MT6797_CCCI_QUEUE_COUNT; q++) {
		spin_lock_init(&ccci->queue[q].tx_lock);
		for (i = 0; i < MT6797_CCCI_RING_SIZE; i++) {
			struct mt6797_ccci_slot *tx = &ccci->queue[q].tx[i];
			struct mt6797_ccci_slot *rx = &ccci->queue[q].rx[i];

			tx->gpd = dma_pool_alloc(ccci->gpd_pool, GFP_KERNEL,
						 &tx->gpd_dma);
			rx->gpd = dma_pool_alloc(ccci->gpd_pool, GFP_KERNEL,
						 &rx->gpd_dma);
			rx->skb = mt6797_ccci_alloc_rx(ccci, GFP_KERNEL);
			if (!tx->gpd || !rx->gpd || !rx->skb)
				return -ENOMEM;
			rx->skb_dma = dma_map_single(ccci->dev, rx->skb->data,
						     MT6797_CCCI_RX_SIZE,
						     DMA_FROM_DEVICE);
			if (dma_mapping_error(ccci->dev, rx->skb_dma))
				return -EIO;
			rx->mapped = true;
			tx_gpd[i] = tx->gpd;
			rx_gpd[i] = rx->gpd;
			tx_dma[i] = tx->gpd_dma;
			rx_gpd_dma[i] = rx->gpd_dma;
			rx_dma[i] = rx->skb_dma;
			rx_size[i] = MT6797_CCCI_RX_SIZE;
		}
		ret = mt6797_md1_cldma_tx_ring_init(tx_gpd, tx_dma, ring_size);
		if (ret)
			return ret;
		ret = mt6797_md1_cldma_rx_ring_init(rx_gpd, rx_gpd_dma, rx_dma, rx_size, ring_size);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6797_ccci_tx(struct mt6797_ccci *ccci, unsigned int q,
			  struct sk_buff *skb, u16 channel)
{
	struct mt6797_ccci_queue *queue = &ccci->queue[q];
	struct mtk_ccci_header *header;
	struct mt6797_ccci_slot *slot;
	__le32 reserved, data0;
	dma_addr_t dma;
	unsigned long flags;
	int ret;

	if (q >= MT6797_CCCI_QUEUE_COUNT ||
	    channel >= MT6797_CCCI_CHANNEL_COUNT ||
	    skb->len < MTK_CCCI_HEADER_SIZE || skb->len > U16_MAX ||
	    atomic_read(&ccci->failed))
		return -ESHUTDOWN;

	spin_lock_irqsave(&queue->tx_lock, flags);
	if (queue->tx_used == MT6797_CCCI_RING_SIZE)
		goto busy;
	header = (struct mtk_ccci_header *)skb->data;
	reserved = header->reserved;
	data0 = header->data0;
	mtk_ccci_header_init(header, channel,
			     mt6797_ccci_next_sequence(ccci, channel), skb->len);
	header->reserved = reserved;
	/* RPC echoes the request's data0; FS carries the vendor fragment flag. */
	if (channel == MT6797_RPC_TX || channel == MT6797_FS_TX)
		header->data0 = data0;
	if (!q && channel == MTK_CCCI_CONTROL_TX)
		mt6797_ccci_diag_final_reply(&ccci->diag, skb->data, skb->len);
	dma = dma_map_single(ccci->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(ccci->dev, dma)) {
		ret = -EIO;
		goto rollback_sequence;
	}
	slot = &queue->tx[queue->tx_head];
	ret = mt6797_md1_cldma_tgpd_prepare_tx(slot->gpd, dma, skb->len);
	if (ret)
		goto unmap;
	slot->skb = skb;
	slot->skb_dma = dma;
	slot->mapped = true;
	spin_lock(&ccci->tx_stop_lock);
	if (atomic_read(&ccci->failed)) {
		ret = -ESHUTDOWN;
		goto no_publish;
	}
	ret = mt6797_md1_cldma_tgpd_publish(slot->gpd);
	if (ret)
		goto no_publish;
	queue->tx_head = (queue->tx_head + 1) % MT6797_CCCI_RING_SIZE;
	queue->tx_used++;
	/* A new modem request may reuse this slot as soon as its reply is
	 * published. Release here, before RESUME, not after the worker returns.
	 */
	if (channel == MT6797_FS_TX && !(le32_to_cpu(data0) & 0x80000000U) &&
	    le32_to_cpu(reserved) < MT6797_FS_INDEX_COUNT)
		clear_bit(le32_to_cpu(reserved), &ccci->fs_indices);
	writel(BIT(q), ccci->pdn + CLDMA_UL_RESUME_CMD);
	readl(ccci->pdn + CLDMA_UL_RESUME_CMD);
	spin_unlock(&ccci->tx_stop_lock);
	if (!q)
		mt6797_ccci_diag_tx(&ccci->diag, false, queue->tx_head,
				     queue->tx_tail, queue->tx_used);
	spin_unlock_irqrestore(&queue->tx_lock, flags);
	return 0;

no_publish:
	spin_unlock(&ccci->tx_stop_lock);
	slot->skb = NULL;
	slot->mapped = false;
unmap:
	dma_unmap_single(ccci->dev, dma, skb->len, DMA_TO_DEVICE);
rollback_sequence:
	atomic_dec(&ccci->tx_sequence[channel]);
	spin_unlock_irqrestore(&queue->tx_lock, flags);
	return ret;
busy:
	spin_unlock_irqrestore(&queue->tx_lock, flags);
	return -EBUSY;
}

static bool mt6797_ccci_tx_has_room(struct mt6797_ccci *ccci, unsigned int q)
{
	struct mt6797_ccci_queue *queue = &ccci->queue[q];
	unsigned long flags;
	bool room;

	spin_lock_irqsave(&queue->tx_lock, flags);
	room = queue->tx_used < MT6797_CCCI_RING_SIZE;
	spin_unlock_irqrestore(&queue->tx_lock, flags);
	return room;
}

static void mt6797_ccci_tx_wake(struct mt6797_ccci *ccci, unsigned int q)
{
	unsigned int i;

	if (!atomic_read(&ccci->ready))
		return;
	if (q == 1 && ccci->at_port)
		wwan_port_txon(ccci->at_port);
	rcu_read_lock();
	for (i = 0; i < MTK_CCCI_MAX_NET_LINKS; i++) {
		struct mt6797_ccci_link *link;

		link = rcu_dereference(ccci->link[i]);
		if (link && link->tx_queue == q && netif_running(link->netdev))
			netif_wake_queue(link->netdev);
	}
	rcu_read_unlock();
}

static int mt6797_ccci_at_start(struct wwan_port *port)
{
	struct mt6797_ccci *ccci = wwan_port_get_drvdata(port);

	atomic_set(&ccci->at_open, 1);
	if (atomic_read(&ccci->ready))
		wwan_port_txon(port);
	else
		wwan_port_txoff(port);
	return 0;
}

static void mt6797_ccci_at_stop(struct wwan_port *port)
{
	struct mt6797_ccci *ccci = wwan_port_get_drvdata(port);

	atomic_set(&ccci->at_open, 0);
	wwan_port_txoff(port);
}

static int mt6797_ccci_at_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct mt6797_ccci *ccci = wwan_port_get_drvdata(port);
	struct mtk_ccci_header *header;
	int ret;

	if (!atomic_read(&ccci->ready))
		return -ENETDOWN;
	if (skb->len > MTK_CCCI_NET_RX_SIZE)
		return -EMSGSIZE;
	ret = skb_linearize(skb);
	if (ret)
		return ret;
	ret = skb_cow_head(skb, MTK_CCCI_HEADER_SIZE);
	if (ret)
		return ret;
	header = (struct mtk_ccci_header *)skb_push(skb, MTK_CCCI_HEADER_SIZE);
	mtk_ccci_header_init(header, MTK_CCCI_UART2_TX, 0, skb->len);
	ret = mt6797_ccci_tx(ccci, 1, skb, MTK_CCCI_UART2_TX);
	if (!ret)
		return 0;
	skb_pull(skb, MTK_CCCI_HEADER_SIZE);
	if (ret == -EBUSY) {
		wwan_port_txoff(port);
		if (mt6797_ccci_tx_has_room(ccci, 1))
			wwan_port_txon(port);
	}
	return ret;
}

static const struct wwan_port_ops mt6797_ccci_at_ops = {
	.start = mt6797_ccci_at_start,
	.stop = mt6797_ccci_at_stop,
	.tx = mt6797_ccci_at_tx,
};

static int mt6797_ccci_net_open(struct net_device *netdev)
{
	struct mt6797_ccci_link *link = wwan_netdev_drvpriv(netdev);

	netif_start_queue(netdev);
	if (atomic_read(&link->ccci->ready)) {
		netif_carrier_on(netdev);
	} else {
		netif_carrier_off(netdev);
		netif_stop_queue(netdev);
	}
	return 0;
}

static int mt6797_ccci_net_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	return 0;
}

static netdev_tx_t mt6797_ccci_net_xmit(struct sk_buff *skb,
					struct net_device *netdev)
{
	struct mt6797_ccci_link *link = wwan_netdev_drvpriv(netdev);
	struct mt6797_ccci *ccci = link->ccci;
	struct mtk_ccci_header *header;
	unsigned int payload_len = skb->len;
	u8 version;
	int ret;

	if (!payload_len || payload_len > netdev->mtu ||
	    !atomic_read(&ccci->ready))
		goto drop;
	ret = skb_linearize(skb);
	if (ret)
		goto drop;
	version = skb->data[0] >> 4;
	if (version != 4 && version != 6)
		goto drop;
	ret = skb_cow_head(skb, MTK_CCCI_HEADER_SIZE);
	if (ret)
		goto drop;
	header = (struct mtk_ccci_header *)skb_push(skb, MTK_CCCI_HEADER_SIZE);
	mtk_ccci_header_init(header, link->tx_channel, 0, skb->len);
	ret = mt6797_ccci_tx(ccci, link->tx_queue, skb, link->tx_channel);
	if (ret == -EBUSY) {
		skb_pull(skb, MTK_CCCI_HEADER_SIZE);
		netif_stop_queue(netdev);
		if (mt6797_ccci_tx_has_room(ccci, link->tx_queue))
			netif_wake_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	if (ret) {
		skb_pull(skb, MTK_CCCI_HEADER_SIZE);
		goto drop;
	}
	dev_sw_netstats_tx_add(netdev, 1, payload_len);
	return NETDEV_TX_OK;

drop:
	DEV_STATS_INC(netdev, tx_dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops mt6797_ccci_netdev_ops = {
	.ndo_open = mt6797_ccci_net_open,
	.ndo_stop = mt6797_ccci_net_stop,
	.ndo_start_xmit = mt6797_ccci_net_xmit,
};

static void mt6797_ccci_net_setup(struct net_device *netdev)
{
	netdev->needed_headroom += MTK_CCCI_HEADER_SIZE;
	netdev->mtu = MTK_CCCI_NET_MTU;
	netdev->min_mtu = 68;
	netdev->max_mtu = MTK_CCCI_NET_MTU;
	netdev->tx_queue_len = 1000;
	netdev->flags = IFF_POINTOPOINT | IFF_NOARP;
	netdev->features |= NETIF_F_VLAN_CHALLENGED | NETIF_F_GRO;
	netdev->hw_features |= NETIF_F_GRO;
	netdev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	netdev->needs_free_netdev = true;
	netdev->type = ARPHRD_NONE;
	netdev->netdev_ops = &mt6797_ccci_netdev_ops;
}

static int mt6797_ccci_net_newlink(void *context, struct net_device *netdev,
				   u32 if_id,
				   struct netlink_ext_ack *extack)
{
	struct mt6797_ccci *ccci = context;
	struct mt6797_ccci_link *link = wwan_netdev_drvpriv(netdev);
	int ret;

	ret = mtk_ccci_net_channels(if_id, &link->tx_channel, NULL,
				    &link->tx_queue);
	if (ret)
		return ret;
	if (rcu_access_pointer(ccci->link[if_id]))
		return -EEXIST;
	link->ccci = ccci;
	link->netdev = netdev;
	link->if_id = if_id;
	ret = register_netdevice(netdev);
	if (ret)
		return ret;
	rcu_assign_pointer(ccci->link[if_id], link);
	netif_carrier_off(netdev);
	netif_device_attach(netdev);
	return 0;
}

static void mt6797_ccci_net_dellink(void *context, struct net_device *netdev,
				    struct list_head *head)
{
	struct mt6797_ccci_link *link = wwan_netdev_drvpriv(netdev);
	struct mt6797_ccci *ccci = context;

	if (WARN_ON(link->if_id >= MTK_CCCI_MAX_NET_LINKS) ||
	    WARN_ON(rcu_access_pointer(ccci->link[link->if_id]) != link))
		return;
	RCU_INIT_POINTER(ccci->link[link->if_id], NULL);
	/* unregister_netdevice_many() provides the matching synchronize_net(). */
	unregister_netdevice_queue(netdev, head);
}

static const struct wwan_ops mt6797_ccci_wwan_ops = {
	.priv_size = sizeof(struct mt6797_ccci_link),
	.setup = mt6797_ccci_net_setup,
	.newlink = mt6797_ccci_net_newlink,
	.dellink = mt6797_ccci_net_dellink,
};

static void mt6797_ccci_hs1_work(struct work_struct *work)
{
	struct mt6797_ccci *ccci = container_of(work, struct mt6797_ccci, hs1_work);
	struct sk_buff *skb = xchg(&ccci->hs1_skb, NULL);
	struct mtk_ccci_frame frame;
	struct mt6797_md1_hs_frame hs_frame = {
		.wire_size = skb ? skb->len : 0,
	};
	enum mt6797_md1_hs_action action;
	struct sk_buff *reply;
	size_t runtime_len, reply_len;
	int ret;

	if (!skb) {
		ret = -ENODATA;
		goto failed;
	}
	if (atomic_read(&ccci->failed)) {
		ret = -ESHUTDOWN;
		goto failed;
	}
	ret = mtk_ccci_header_decode(skb->data, skb->len, &frame);
	if (ret)
		goto failed;
	hs_frame.data0 = frame.data0;
	hs_frame.data1 = frame.data1;
	hs_frame.channel = frame.channel;
	hs_frame.reserved = frame.reserved;
	reply = alloc_skb(MTK_CCCI_RUNTIME_V2_REPLY_SIZE, GFP_KERNEL);
	if (!reply) {
		ret = -ENOMEM;
		goto failed;
	}
	skb_put(reply, MTK_CCCI_RUNTIME_V2_REPLY_SIZE);
	ret = mtk_ccci_runtime_v2_reply(skb->data, skb->len,
					&ccci->runtime_config, 0, ccci->runtime,
					MTK_CCCI_RUNTIME_AP_SIZE, &runtime_len,
					reply->data, reply->len, &reply_len);
	if (ret)
		goto free_reply;
	ret = mt6797_md1_hs_rx(&ccci->hs, &hs_frame, &action);
	if (ret || action != MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME) {
		ret = ret ?: -EPROTO;
		goto free_reply;
	}
	/* Vendor port_proxy_md_hs1_msg_notify() finalizes region 13 here. */
	ret = mt6797_md1_cold_owner_hs1_stage2();
	if (ret)
		goto free_reply;
	memcpy_toio(ccci->smem + MTK_CCCI_RUNTIME_OFFSET,
		    ccci->runtime, runtime_len);
	/* Publish the complete runtime block before queuing its HS1 response. */
	wmb();
	mt6797_ccci_diag_payload(&ccci->diag, skb->data, skb->len,
				reply->data, reply_len, ccci->runtime, runtime_len);
	ret = mt6797_ccci_tx(ccci, 0, reply, MTK_CCCI_CONTROL_TX);
	if (!ret) {
		dev_info(ccci->dev,
			 "MD1 accepted CCCI HS1 v2; runtime reply queued (%zu bytes)\n",
			 runtime_len);
		kfree_skb(skb);
		atomic_set(&ccci->hs1_pending, 0);
		return;
	}
free_reply:
	kfree_skb(reply);
failed:
	kfree_skb(skb);
	atomic_set(&ccci->hs1_pending, 0);
	mt6797_ccci_quiesce(ccci, ret);
}

static void mt6797_ccci_handshake_timeout_work(struct work_struct *work)
{
	struct mt6797_ccci *ccci = container_of(to_delayed_work(work),
		struct mt6797_ccci, handshake_timeout_work);

	if (!atomic_read(&ccci->ready))
		mt6797_ccci_quiesce_from(ccci, -ETIMEDOUT, "ap-handshake-timer");
}

static int mt6797_ccci_control_rx(struct mt6797_ccci *ccci,
				  struct sk_buff *skb,
				  const struct mtk_ccci_frame *frame)
{
	struct mt6797_md1_hs_frame hs_frame = {
		.data0 = frame->data0,
		.data1 = frame->data1,
		.channel = frame->channel,
		.reserved = frame->reserved,
		.wire_size = skb->len,
	};
	enum mt6797_md1_hs_action action;
	struct sk_buff *copy;
	int ret;

	if (skb->len == MTK_CCCI_HEADER_SIZE + MTK_CCCI_FEATURE_QUERY_SIZE &&
	    frame->data1 == 0 && frame->reserved == MTK_CCCI_MD_INIT_CHK_ID) {
		if (atomic_cmpxchg(&ccci->hs1_pending, 0, 1)) {
			mt6797_ccci_quiesce(ccci, -EPROTO);
			return -EPROTO;
		}
		copy = skb_copy(skb, GFP_ATOMIC);
		if (!copy) {
			atomic_set(&ccci->hs1_pending, 0);
			mt6797_ccci_quiesce(ccci, -ENOMEM);
			return -ENOMEM;
		}
		WRITE_ONCE(ccci->hs1_skb, copy);
		schedule_work(&ccci->hs1_work);
		return 0;
	}
	if (skb->len == MTK_CCCI_HEADER_SIZE && frame->data1 == 0 &&
	    frame->reserved == MTK_CCCI_MD_INIT_CHK_ID) {
		mt6797_ccci_quiesce(ccci, -EOPNOTSUPP);
		return -EOPNOTSUPP;
	}

	ret = mt6797_md1_hs_rx(&ccci->hs, &hs_frame, &action);
	if (!ret && action == MT6797_MD1_HS_ACTION_HS2_READY) {
		cancel_delayed_work(&ccci->handshake_timeout_work);
		mt6797_ccci_frontend_ready(ccci, true);
		dev_info(ccci->dev, "MD1 completed CCCI HS2\n");
	}
	return ret;
}

static int mt6797_ccci_rpc_rx(struct mt6797_ccci *ccci,
			    struct sk_buff *skb, unsigned int q)
{
	u8 bytes[MT6797_RPC_REPLY_SIZE];
	struct sk_buff *reply;
	s32 status;
	int ret;

	if (q != 1 || atomic_read(&ccci->failed))
		return -ESHUTDOWN;
	ret = mt6797_rpc_eint_reply(skb->data, skb->len, ccci->sim_eint,
				  bytes, sizeof(bytes), &status);
	if (ret)
		return ret;
	reply = alloc_skb(sizeof(bytes), GFP_ATOMIC);
	if (!reply)
		return -ENOMEM;
	skb_put_data(reply, bytes, sizeof(bytes));
	/* On success TX owns reply through reclaim; on failure we still own it. */
	ret = mt6797_ccci_tx(ccci, 1, reply, MT6797_RPC_TX);
	if (ret)
		kfree_skb(reply);
	if (atomic_add_unless(&ccci->rpc_logs, 1, 16))
		dev_info(ccci->dev,
			 "CCCI RPC op=0x4005 reply_channel=33 status=%d tx_result=%d bytes=40\n",
			 status, ret);
	return ret;
}

struct mt6797_ccci_fs_work {
	struct work_struct work;
	struct mt6797_ccci *ccci;
	u32 index;
	size_t len;
	u8 frame[];
};

static inline bool mt6797_ccci_fs_index_busy(struct mt6797_ccci *ccci, u32 index)
{
	unsigned long flags;
	bool busy;

	/* HWO can be consumed before RESUME. A receiving CPU must observe the
	 * whole successful publish+index-release transaction, not its midpoint.
	 */
	spin_lock_irqsave(&ccci->tx_stop_lock, flags);
	busy = test_bit(index, &ccci->fs_indices);
	spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
	return busy;
}

static int mt6797_ccci_fs_index_claim(struct mt6797_ccci *ccci, u32 index)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ccci->tx_stop_lock, flags);
	if (atomic_read(&ccci->failed) || atomic_read(&ccci->teardown))
		ret = -ESHUTDOWN;
	else if (test_and_set_bit(index, &ccci->fs_indices))
		ret = -EALREADY;
	spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
	return ret;
}

/* Every frame of one reply is queued in order; only the final frame releases
 * the request index, inside TX after publication.
 */
static int mt6797_ccci_fs_send(struct mt6797_ccci *ccci,
			       const struct mt6797_fs_reply *reply, size_t *sent)
{
	unsigned int frames = mt6797_fs_reply_frames(reply), n;
	struct sk_buff *skb;
	int len, ret = 0;

	*sent = 0;
	for (n = 0; n < frames && !ret; n++) {
		skb = alloc_skb(MT6797_FS_PREFIX + MT6797_FS_FRAGMENT, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;
		len = mt6797_fs_reply_frame(reply, n, skb_tail_pointer(skb),
					    MT6797_FS_PREFIX + MT6797_FS_FRAGMENT);
		if (len < 0) {
			kfree_skb(skb);
			return len;
		}
		skb_put(skb, len);
		/* On success TX owns the frame through reclaim. */
		ret = mt6797_ccci_tx(ccci, 1, skb, MT6797_FS_TX);
		if (ret)
			kfree_skb(skb);
		else
			*sent += len;
	}
	return ret;
}

static void mt6797_ccci_fs_work(struct work_struct *work)
{
	struct mt6797_ccci_fs_work *job =
		container_of(work, struct mt6797_ccci_fs_work, work);
	struct mt6797_ccci *ccci = job->ccci;
	struct mt6797_fs_reply reply = { .capacity = MT6797_FS_REPLY_MAX };
	size_t sent = 0;
	u64 diagnostic_sequence;
	s32 status = 0;
	int ret;
	bool index_released = false;

	if (atomic_read(&ccci->failed) || atomic_read(&ccci->teardown))
		goto done;
	diagnostic_sequence = mt6797_fs_diag_begin(&ccci->fs_diag, job->frame, job->len);
	reply.payload = kmalloc(reply.capacity, GFP_KERNEL);
	if (!reply.payload) {
		mt6797_fs_diag_end(&ccci->fs_diag, diagnostic_sequence, 0, -ENOMEM, 0);
		mt6797_ccci_quiesce(ccci, -ENOMEM);
		goto done;
	}
	ret = mt6797_fs_service_request(&ccci->fs_service, job->frame, job->len,
					&reply, &status);
	if (ret) {
		struct mt6797_fs_request request;
		int parsed = mt6797_fs_request_parse(job->frame, job->len, &request);

		dev_info_ratelimited(ccci->dev,
			"CCCI FS service_error=%d parse_error=%d request_bytes=%zu\n",
			ret, parsed, job->len);
		/* Numeric shape only: never log path or NVRAM file contents. */
		if (!parsed && request.op == MT6797_FS_COMPACT_READ &&
		    request.argc == 2 && request.len[1] == 40)
			dev_info_ratelimited(ccci->dev,
				"CCCI FS compact_flags=%x open_flags=%x offset=%d whence=%u read_length=%u\n",
				mt6797_fs_read32(request.arg[1]),
				mt6797_fs_read32(request.arg[1] + 12),
				(s32)mt6797_fs_read32(request.arg[1] + 20),
				mt6797_fs_read32(request.arg[1] + 24),
				mt6797_fs_read32(request.arg[1] + 32));
	}
	if (!ret && !atomic_read(&ccci->failed) && !atomic_read(&ccci->teardown)) {
		ret = mt6797_ccci_fs_send(ccci, &reply, &sent);
		if (ret)
			dev_info_ratelimited(ccci->dev,
				"CCCI FS send_error=%d reply_bytes=%zu frames=%u sent=%zu\n",
				ret, reply.len, mt6797_fs_reply_frames(&reply), sent);
		index_released = !ret;
	}
	if (atomic_add_unless(&ccci->fs_logs, 1, 128))
		dev_info(ccci->dev,
			 "CCCI FS op=0x%x reply_channel=15 status=%d tx_result=%d bytes=%zu\n",
			 mt6797_fs_read32(job->frame + 16), status, ret, sent);
	mt6797_fs_diag_end(&ccci->fs_diag, diagnostic_sequence, status, ret, sent);
	if (ret == -ENOMEM || ret == -EIO || ret == -EBUSY)
		mt6797_ccci_quiesce(ccci, ret);
	kfree(reply.payload);
done:
	if (!index_released)
		clear_bit(job->index, &ccci->fs_indices);
	atomic_dec(&ccci->fs_pending);
	kfree(job);
}

static void mt6797_ccci_fs_destroy(void *data)
{
	struct mt6797_ccci *ccci = data;
	struct workqueue_struct *wq = xchg(&ccci->fs_wq, NULL);

	if (wq) {
		destroy_workqueue(wq);
		/* No worker can run any more; drop every retained handle. */
		mt6797_fs_service_release(&ccci->fs_service);
	}
}

/* NAPI admission: header sanity, supported operation, one active request per
 * index, bounded jobs. Unsupported or malformed frames stay captured; the
 * service parses the immutable copy in process context.
 */
static int mt6797_ccci_fs_rx(struct mt6797_ccci *ccci,
			   struct sk_buff *skb, unsigned int q)
{
	struct mt6797_ccci_fs_work *job;
	u32 index;
	int ret;

	if (!mt6797_md1_fs_provider_enabled())
		return -EOPNOTSUPP;
	if (q != 1 || atomic_read(&ccci->failed))
		return -ESHUTDOWN;
	if (skb->len < 24 || skb->len > MT6797_FS_REQUEST_MAX ||
	    mt6797_fs_read32(skb->data) ||
	    mt6797_fs_read32(skb->data + 4) != skb->len)
		return -EPROTO;
	index = mt6797_fs_read32(skb->data + 12);
	if (index >= MT6797_FS_INDEX_COUNT)
		return -EPROTO;
	if (!mt6797_fs_service_supports(mt6797_fs_read32(skb->data + 16)))
		return -EOPNOTSUPP;
	if (atomic_read(&ccci->teardown) || !ccci->fs_wq)
		return -ESHUTDOWN;
	ret = mt6797_ccci_fs_index_claim(ccci, index);
	if (ret)
		return ret;
	/* Five active request indices plus one published callback finishing. */
	if (atomic_inc_return(&ccci->fs_pending) > 6) {
		atomic_dec(&ccci->fs_pending);
		clear_bit(index, &ccci->fs_indices);
		return -ENOSPC;
	}
	job = kmalloc(struct_size(job, frame, skb->len), GFP_ATOMIC);
	if (!job) {
		atomic_dec(&ccci->fs_pending);
		clear_bit(index, &ccci->fs_indices);
		return -ENOMEM;
	}
	job->ccci = ccci;
	job->index = index;
	job->len = skb->len;
	memcpy(job->frame, skb->data, skb->len);
	INIT_WORK(&job->work, mt6797_ccci_fs_work);
	/* Only NAPI queues work. Teardown disables NAPI before destroying wq;
	 * each fresh job is immutable; at most five active indices plus one
	 * already-published callback finishing can hold allocated jobs.
	 */
	queue_work(ccci->fs_wq, &job->work);
	return 0;
}

static void mt6797_ccci_dispatch_rx(struct mt6797_ccci *ccci,
				    struct sk_buff *skb, unsigned int q)
{
	/* RX buffers are linear; rx_poll has unmapped DMA before dispatch.
	 * Diagnostic copies finish here before the sole skb release below.
	 */
	struct mt6797_ccci_link *link;
	struct mtk_ccci_frame frame;
	__be16 protocol;
	int if_id;
	u8 version;
	u32 diag_drop = 0;
	int ret;

	ret = mtk_ccci_header_decode(skb->data, skb->len, &frame);
	if (ret) {
		diag_drop = MT6797_CCCI_DIAG_BAD_HEADER;
		goto drop;
	}
	if (frame.channel >= MT6797_CCCI_CHANNEL_COUNT) {
		diag_drop = MT6797_CCCI_DIAG_BAD_CHANNEL;
		goto drop;
	}
	if (frame.assert_bit && ccci->rx_sequence[frame.channel] != 0 &&
	    ((frame.sequence - ccci->rx_sequence[frame.channel]) & 0x7fff) != 1) {
		if (!atomic_read(&ccci->ready) && frame.channel != MTK_CCCI_CONTROL_RX)
			mt6797_ccci_diag_rx_save(&ccci->diag, skb->data, skb->len, q,
				MT6797_CCCI_DIAG_BAD_SEQUENCE, 0, frame.channel);
		mt6797_ccci_quiesce(ccci, -EILSEQ);
		goto drop;
	}
	ccci->rx_sequence[frame.channel] = frame.sequence;
	if (frame.channel == MTK_CCCI_CONTROL_RX) {
		mt6797_ccci_control_rx(ccci, skb, &frame);
		goto drop;
	}
	if (frame.channel == MT6797_RPC_RX) {
		ret = mt6797_ccci_rpc_rx(ccci, skb, q);
		if (ret && !atomic_read(&ccci->ready))
			mt6797_ccci_diag_rx_save(&ccci->diag, skb->data, skb->len, q,
				MT6797_CCCI_DIAG_BEFORE_READY, 0, frame.channel);
		if (ret == -ENOMEM || ret == -EIO || ret == -EBUSY)
			mt6797_ccci_quiesce(ccci, ret);
		goto drop;
	}
	if (frame.channel == MT6797_FS_RX) {
		ret = mt6797_ccci_fs_rx(ccci, skb, q);
		if (ret && !atomic_read(&ccci->ready))
			mt6797_ccci_diag_rx_save(&ccci->diag, skb->data, skb->len, q,
				MT6797_CCCI_DIAG_BEFORE_READY, 0, frame.channel);
		if (ret == -ENOMEM || ret == -EIO || ret == -EBUSY)
			mt6797_ccci_quiesce(ccci, ret);
		goto drop;
	}
	if (!atomic_read(&ccci->ready)) {
		diag_drop = MT6797_CCCI_DIAG_BEFORE_READY;
		goto drop;
	}
	if (frame.data1 != skb->len || skb->len <= MTK_CCCI_HEADER_SIZE)
		goto drop;
	if (frame.channel == MTK_CCCI_UART2_RX) {
		if (!atomic_read(&ccci->at_open))
			goto drop;
		skb_pull(skb, MTK_CCCI_HEADER_SIZE);
		wwan_port_rx(ccci->at_port, skb);
		return;
	}
	if_id = mtk_ccci_net_rx_link(frame.channel);
	if (if_id < 0)
		goto drop;
	skb_pull(skb, MTK_CCCI_HEADER_SIZE);
	version = skb->data[0] >> 4;
	if (version == 4)
		protocol = htons(ETH_P_IP);
	else if (version == 6)
		protocol = htons(ETH_P_IPV6);
	else
		goto drop;
	rcu_read_lock();
	link = rcu_dereference(ccci->link[if_id]);
	if (!link || !netif_running(link->netdev) ||
	    skb->len > link->netdev->mtu) {
		rcu_read_unlock();
		goto drop;
	}
	skb->dev = link->netdev;
	skb->protocol = protocol;
	skb->ip_summed = CHECKSUM_NONE;
	skb_reset_network_header(skb);
	dev_sw_netstats_rx_add(link->netdev, skb->len);
	napi_gro_receive(&ccci->napi, skb);
	rcu_read_unlock();
	return;
drop:
	if (diag_drop && !atomic_read(&ccci->ready))
		mt6797_ccci_diag_rx_save(&ccci->diag, skb->data, skb->len, q,
				       diag_drop, ret, ret ? 0 : frame.channel);
	kfree_skb(skb);
}

static int mt6797_ccci_rx_poll(struct mt6797_ccci *ccci, unsigned int q,
			       int budget)
{
	struct mt6797_ccci_queue *queue = &ccci->queue[q];
	int done = 0;

	while (done < budget) {
		struct mt6797_ccci_slot *slot = &queue->rx[queue->rx_tail];
		struct mt6797_md1_cldma_rgpd *gpd = slot->gpd;
		struct sk_buff *replacement, *skb;
		dma_addr_t replacement_dma;
		u16 len;
		int ret;

		ret = mt6797_md1_cldma_rgpd_consume(gpd, &len);
		if (ret == -EAGAIN)
			break;
		if (ret) {
			mt6797_ccci_quiesce(ccci, ret);
			break;
		}
		replacement = mt6797_ccci_alloc_rx(ccci, GFP_ATOMIC);
		if (!replacement)
			break;
		replacement_dma = dma_map_single(ccci->dev, replacement->data,
						 MT6797_CCCI_RX_SIZE,
						 DMA_FROM_DEVICE);
		if (dma_mapping_error(ccci->dev, replacement_dma)) {
			kfree_skb(replacement);
			break;
		}
		dma_unmap_single(ccci->dev, slot->skb_dma, MT6797_CCCI_RX_SIZE,
				 DMA_FROM_DEVICE);
		skb = slot->skb;
		skb_put(skb, len);
		slot->skb = replacement;
		slot->skb_dma = replacement_dma;
		ret = mt6797_md1_cldma_rgpd_publish(gpd, replacement_dma,
						    MT6797_CCCI_RX_SIZE);
		if (ret) {
			kfree_skb(skb);
			mt6797_ccci_quiesce(ccci, ret);
			break;
		}
		queue->rx_tail = (queue->rx_tail + 1) % MT6797_CCCI_RING_SIZE;
		mt6797_ccci_dispatch_rx(ccci, skb, q);
		done++;
		if (atomic_read(&ccci->failed))
			break;
	}
	if (done && !atomic_read(&ccci->failed)) {
		writel(BIT(q), ccci->pdn + CLDMA_SO_RESUME_CMD);
		readl(ccci->pdn + CLDMA_SO_RESUME_CMD);
	}
	return done;
}

static void mt6797_ccci_tx_poll(struct mt6797_ccci *ccci, unsigned int q)
{
	struct mt6797_ccci_queue *queue = &ccci->queue[q];
	unsigned long flags;
	bool reclaimed = false;

	spin_lock_irqsave(&queue->tx_lock, flags);
	while (queue->tx_used) {
		struct mt6797_ccci_slot *slot = &queue->tx[queue->tx_tail];
		u16 len;

		int ret = mt6797_md1_cldma_tgpd_reclaim(slot->gpd, &len);

		if (ret == -EAGAIN)
			break;
		if (ret) {
			mt6797_ccci_quiesce(ccci, ret);
			break;
		}
		dma_unmap_single(ccci->dev, slot->skb_dma, slot->skb->len,
				 DMA_TO_DEVICE);
		dev_consume_skb_irq(slot->skb);
		slot->skb = NULL;
		slot->mapped = false;
		queue->tx_tail = (queue->tx_tail + 1) % MT6797_CCCI_RING_SIZE;
		queue->tx_used--;
		if (!q)
			mt6797_ccci_diag_tx(&ccci->diag, true, queue->tx_head,
					     queue->tx_tail, queue->tx_used);
		reclaimed = true;
	}
	spin_unlock_irqrestore(&queue->tx_lock, flags);
	if (reclaimed)
		mt6797_ccci_tx_wake(ccci, q);
}

static int mt6797_ccci_poll(struct napi_struct *napi, int budget)
{
	struct mt6797_ccci *ccci = container_of(napi, struct mt6797_ccci, napi);
	u32 tx, rx, tx_rearm, rx_rearm;
	u64 generation;
	unsigned long flags;
	unsigned int q;
	int done = 0;

	/* No gap between consuming pending bits and taking their mask ownership. */
	spin_lock_irqsave(&ccci->tx_stop_lock, flags);
	ccci->napi_mask_generation++;
	tx = atomic_xchg(&ccci->tx_pending, 0);
	rx = atomic_xchg(&ccci->rx_pending, 0);
	atomic_or(tx, &ccci->tx_mask_owned);
	atomic_or(rx, &ccci->rx_mask_owned);
	spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);

	for (q = 0; q < MT6797_CCCI_QUEUE_COUNT; q++) {
		if (tx & BIT(q))
			mt6797_ccci_tx_poll(ccci, q);
		if ((rx & BIT(q)) && done < budget) {
			struct mt6797_ccci_queue *queue = &ccci->queue[q];
			struct mt6797_ccci_slot *slot;

			done += mt6797_ccci_rx_poll(ccci, q, budget - done);
			slot = &queue->rx[queue->rx_tail];
			if (!atomic_read(&ccci->failed) &&
			    !(READ_ONCE(((struct mt6797_md1_cldma_rgpd *)slot->gpd)->gpd_flags) &
			      MT6797_MD1_CLDMA_GPD_HWO)) {
				atomic_or(BIT(q), &ccci->rx_pending);
				done = budget;
			}
		}
	}
	if (atomic_read(&ccci->failed)) {
		if (budget)
			napi_complete_done(napi, 0);
		return 0;
	}
	if (done >= budget)
		atomic_or(rx, &ccci->rx_pending);
	if (done < budget && (atomic_read(&ccci->tx_pending) ||
			      atomic_read(&ccci->rx_pending)))
		return budget;
	if (done < budget) {
		/* Complete can release NAPI and flush GRO (which can enter TX).
		 * Keep the masked union visible to every newer poll. A completing
		 * generation may rearm only if no IRQ or poll has superseded it.
		 * Never hold tx_stop_lock across this callback.
		 */
		spin_lock_irqsave(&ccci->tx_stop_lock, flags);
		generation = ccci->napi_mask_generation;
		spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
		if (!napi_complete_done(napi, done))
			/* No restore: a newer completion may already have rearmed all. */
			return done;
		spin_lock_irqsave(&ccci->tx_stop_lock, flags);
		if (!atomic_read(&ccci->failed) &&
		    generation == ccci->napi_mask_generation &&
		    !atomic_read(&ccci->tx_pending) &&
		    !atomic_read(&ccci->rx_pending)) {
			tx_rearm = atomic_xchg(&ccci->tx_mask_owned, 0);
			rx_rearm = atomic_xchg(&ccci->rx_mask_owned, 0);
			writel(tx_rearm, ccci->pdn + CLDMA_L2_TX_MASK_CLEAR);
			writel(rx_rearm, ccci->ao + CLDMA_L2_RX_MASK_CLEAR);
		}
		spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
	}
	return done;
}

static irqreturn_t mt6797_ccci_cldma_irq(int irq, void *data)
{
	struct mt6797_ccci *ccci = data;
	struct mt6797_md1_cldma_irq_pending pending;
	u32 tx_status, tx_mask, rx_status, rx_mask;
	unsigned long flags;

	tx_status = readl(ccci->pdn + CLDMA_L2_TX_STATUS);
	tx_mask = readl(ccci->pdn + CLDMA_L2_TX_MASK);
	rx_status = readl(ccci->pdn + CLDMA_L2_RX_STATUS);
	rx_mask = readl(ccci->ao + CLDMA_L2_RX_MASK);
	mt6797_ccci_diag_irq(&ccci->diag, tx_status, tx_mask, rx_status, rx_mask);
	mt6797_md1_cldma_irq_decode(tx_status, tx_mask, rx_status, rx_mask, &pending);
	if (!pending.tx_done && !pending.rx_done && !pending.tx_error &&
	    !pending.rx_error)
		return IRQ_NONE;
	/* Match vendor ACK scope: masked events were not claimed by this IRQ. */
	writel(tx_status & ~tx_mask, ccci->pdn + CLDMA_L2_TX_STATUS);
	writel(rx_status & ~rx_mask, ccci->pdn + CLDMA_L2_RX_STATUS);
	if (pending.tx_error || pending.rx_error) {
		mt6797_ccci_quiesce(ccci, -EIO);
		return IRQ_HANDLED;
	}
	/* Serialize mask+pending publication against completion rearm and STOP. */
	spin_lock_irqsave(&ccci->tx_stop_lock, flags);
	if (atomic_read(&ccci->failed)) {
		spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
		return IRQ_HANDLED;
	}
	ccci->napi_mask_generation++;
	if (pending.tx_done) {
		writel(pending.tx_done, ccci->pdn + CLDMA_L2_TX_MASK_SET);
		atomic_or(pending.tx_done, &ccci->tx_pending);
	}
	if (pending.rx_done) {
		writel(pending.rx_done, ccci->ao + CLDMA_L2_RX_MASK_SET);
		atomic_or(pending.rx_done, &ccci->rx_pending);
	}
	spin_unlock_irqrestore(&ccci->tx_stop_lock, flags);
	napi_schedule_irqoff(&ccci->napi);
	return IRQ_HANDLED;
}

static irqreturn_t mt6797_ccci_wdt_irq(int irq, void *data)
{
	struct mt6797_ccci *ccci = data;

	mt6797_ccci_quiesce_from(ccci, -ETIMEDOUT, "md-wdt-irq");
	return IRQ_HANDLED;
}

static irqreturn_t mt6797_ccci_disabled_ccif_irq(int irq, void *data)
{
	return IRQ_NONE;
}

static int mt6797_ccci_clean_reset_check(struct mt6797_ccci *ccci)
{
	u32 ul, so, txirq, rxirq, busy, heads = 0;
	unsigned int q;

	/* Lease proves our reset epoch; idle alone would not establish ownership. */
	ul = readl(ccci->pdn + CLDMA_UL_STATUS);
	so = readl(ccci->ao + CLDMA_SO_STATUS);
	txirq = readl(ccci->pdn + CLDMA_L2_TX_STATUS);
	rxirq = readl(ccci->pdn + CLDMA_L2_RX_STATUS);
	busy = readl(ccci->pdn + 0x04b4); /* vendor CLDMA_AP_IP_BUSY */
	for (q = 0; q < MT6797_CCCI_QUEUE_COUNT; q++) {
		heads |= readl(ccci->pdn + CLDMA_UL_START_ADDR(q));
		heads |= readl(ccci->ao + CLDMA_SO_START_ADDR(q));
	}
	dev_info(ccci->dev,
		 "leased AP CLDMA pre-ring snapshot: ul=%#x so=%#x txirq=%#x rxirq=%#x busy=%#x heads_or=%#x; IP_BUSY observed-only after MD1 GO\n",
		 ul, so, txirq, rxirq, busy, heads);
	/*
	 * This is after MD1 GO, not the supplier's pre-power reset check.
	 * Vendor modem_cldma.c starts AP queues after releasing MD1 and treats
	 * IP_BUSY as wakeup state acknowledged during RX handling. MD1 may
	 * therefore assert it before we install AP descriptors. Preserve it:
	 * neither require reset-time zero nor acknowledge it before RX here.
	 * The exclusive reset-completed lease and fresh AP queue/IRQ/head
	 * checks remain mandatory; IP_BUSY alone is not AP queue ownership.
	 */
	if (ul || so || txirq || rxirq || heads)
		return -EUCLEAN;
	return 0;
}

static int mt6797_ccci_start(struct mt6797_ccci *ccci)
{
	u32 status;
	unsigned int q, i;
	int ret;

	ret = mt6797_ccci_clean_reset_check(ccci);
	if (ret)
		return ret;
	/* Establish an idle, fully masked AP-side engine before replacing heads. */
	writel(MT6797_CCCI_ACTIVE_IRQS, ccci->pdn + CLDMA_L2_TX_MASK_SET);
	writel(MT6797_CCCI_ACTIVE_IRQS, ccci->ao + CLDMA_L2_RX_MASK_SET);
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_UL_STOP_CMD);
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_SO_STOP_CMD);
	ret = readl_poll_timeout(ccci->pdn + CLDMA_UL_STATUS, status,
				 !(status & MT6797_MD1_CLDMA_ACTIVE_QUEUES),
				 10, 100000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(ccci->ao + CLDMA_SO_STATUS, status,
				 !(status & MT6797_MD1_CLDMA_ACTIVE_QUEUES),
				 10, 100000);
	if (ret)
		return ret;
	writel(U32_MAX, ccci->pdn + CLDMA_L2_TX_STATUS);
	writel(U32_MAX, ccci->pdn + CLDMA_L2_RX_STATUS);

	for (q = 0; q < MT6797_CCCI_QUEUE_COUNT; q++) {
		for (i = 0; i < MT6797_CCCI_RING_SIZE; i++) {
			struct mt6797_ccci_slot *rx = &ccci->queue[q].rx[i];

			ret = mt6797_md1_cldma_rgpd_publish(rx->gpd, rx->skb_dma,
							    MT6797_CCCI_RX_SIZE);
			if (ret)
				return ret;
		}
		writel(lower_32_bits(ccci->queue[q].tx[0].gpd_dma),
		       ccci->pdn + CLDMA_UL_START_ADDR(q));
		writel(lower_32_bits(ccci->queue[q].rx[0].gpd_dma),
		       ccci->ao + CLDMA_SO_START_ADDR(q));
	}
	writel(readl(ccci->pdn + CLDMA_UL_CFG) | 0x40,
	       ccci->pdn + CLDMA_UL_CFG);
	writel(readl(ccci->ao + CLDMA_SO_CFG) | 0x45, ccci->ao + CLDMA_SO_CFG);
	writel(readl(ccci->ao + CLDMA_BUS_CFG) | BIT(1),
	       ccci->ao + CLDMA_BUS_CFG);
	writel(0, ccci->pdn + CLDMA_UL_CHECKSUM_ENABLE);
	writel(0, ccci->ao + CLDMA_SO_CHECKSUM_ENABLE);
	writel(0, ccci->ao + CLDMA_DEBUG_ID_ENABLE);
	/* Publish every armed RX GPD and queue head before starting CLDMA. */
	wmb();
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_UL_START_CMD);
	readl(ccci->pdn + CLDMA_UL_START_CMD);
	writel(MT6797_MD1_CLDMA_ACTIVE_QUEUES, ccci->pdn + CLDMA_SO_START_CMD);
	readl(ccci->pdn + CLDMA_SO_START_CMD);
	writel(MT6797_MD1_CLDMA_IRQ_DONE | MT6797_MD1_CLDMA_IRQ_ERROR,
	       ccci->pdn + CLDMA_L2_TX_MASK_CLEAR);
	writel(MT6797_MD1_CLDMA_IRQ_DONE | MT6797_MD1_CLDMA_IRQ_ERROR,
	       ccci->ao + CLDMA_L2_RX_MASK_CLEAR);
	writel(U32_MAX, ccci->pdn + CLDMA_L3_TX_MASK_CLEAR0);
	writel(U32_MAX, ccci->pdn + CLDMA_L3_TX_MASK_CLEAR1);
	writel(U32_MAX, ccci->pdn + CLDMA_L3_RX_MASK_CLEAR0);
	writel(U32_MAX, ccci->pdn + CLDMA_L3_RX_MASK_CLEAR1);
	ccci->started = true;
	enable_irq(ccci->cldma_irq);
	enable_irq(ccci->wdt_irq);
	return 0;
}

static int mt6797_ccci_validate_handoff(struct mt6797_ccci *ccci,
					struct resource *smem_res)
{
	struct mt6797_md1_post_vector_evidence evidence;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_mpu_remap_layout layout;
	u64 md_smem;
	int ret;

	ret = mt6797_md1_cold_owner_wait_handoff(ccci->dev, &evidence);
	if (ret)
		return ret == -ENODATA ? -EPROBE_DEFER : ret;
	if (evidence.token != MT6797_MD1_POST_VECTOR_GO ||
	    !evidence.passive_complete)
		return -EPERM;
	ret = mt6797_md1_lk_handoff_layout_get(&layout);
	if (ret)
		return ret == -ENODATA ? -EPROBE_DEFER : ret;
	ret = mt6797_md1_cldma_lk_carveout_validate(&layout);
	if (ret)
		return ret;
	ret = mt6797_md1_lk_handoff_plan_get(&plan);
	if (ret)
		return ret;
	if (layout.ap_md_smem_base < plan.smem_offset_ap_to_md)
		return -ERANGE;
	md_smem = layout.ap_md_smem_base - plan.smem_offset_ap_to_md;
	if (md_smem > U32_MAX)
		return -ERANGE;
	/*
	 * LK deletes the pre-boot dynamic reservation nodes and installs its
	 * live CCCI mblocks, leaving memory-region phandles dangling. The
	 * handoff provider publishes this layout only after validating those
	 * live reservations, their bounds, no-map attributes and overlaps.
	 * Do not resolve a pre-LK phandle or invent a replacement reservation.
	 */
	*smem_res = (struct resource) {
		.start = layout.ap_md_smem_base,
		.end = layout.ap_md_smem_base + layout.ap_md_smem_size - 1,
		.flags = IORESOURCE_MEM,
	};
	ccci->runtime_config.smem_md_addr = md_smem;
	ccci->runtime_config.smem_size = SZ_1M;
	ccci->runtime_config.random_seed = get_random_u32();
	ccci->runtime_config.ccmni_mtu = MTK_CCCI_NET_RX_SIZE;
	return 0;
}

static int mt6797_ccci_validate_resources(struct platform_device *pdev)
{
	struct mt6797_md1_cldma_resources resources = {
		.capability = 6,
		.md_smem_size = SZ_1M,
	};
	struct of_phandle_args irq;
	struct resource *res;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mt6797_ccci_region_names); i++) {
		const char *name = mt6797_ccci_region_names[i];

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		if (!res)
			return -EINVAL;
		resources.region[i].base = res->start;
		resources.region[i].size = resource_size(res);
	}
	for (i = 0; i < MT6797_MD1_CLDMA_IRQ_COUNT; i++) {
		ret = of_irq_parse_one(pdev->dev.of_node, i, &irq);
		if (ret)
			return ret;
		if (irq.args_count != 3 || irq.args[0] ||
		    irq.args[2] != mt6797_ccci_irq_flags[i]) {
			of_node_put(irq.np);
			return -EINVAL;
		}
		resources.irq[i].hwirq = irq.args[1];
		resources.irq[i].type = mt6797_ccci_irq_types[i];
		of_node_put(irq.np);
	}
	return mt6797_md1_cldma_resources_validate(&resources);
}

static void mt6797_ccci_read_sim_eint(struct mt6797_ccci *ccci)
{
	static const char * const nodes[] = { "sim-eint-1", "sim-eint-2" };
	static const char * const properties[] = {
		"mediatek,eint-number", "mediatek,debounce-us", "mediatek,active-high",
		"mediatek,edge-triggered", "mediatek,socket-type",
		"mediatek,dedicated-enable", "mediatek,source-selector",
	};
	struct device_node *node;
	u32 raw;
	unsigned int sim, type;

	/* Immutable metadata only; never request an IRQ or change pinmux/GPIO. */
	for (sim = 0; sim < ARRAY_SIZE(nodes); sim++) {
		node = of_get_child_by_name(ccci->dev->of_node, nodes[sim]);
		if (!node)
			continue;
		if (of_device_is_available(node)) {
			ccci->sim_eint[sim].present = true;
			for (type = 0; type < ARRAY_SIZE(properties); type++) {
				if (of_property_count_u32_elems(node, properties[type]) != 1 ||
				    of_property_read_u32(node, properties[type], &raw) ||
				    !mt6797_rpc_eint_value(type, raw,
					&ccci->sim_eint[sim].value[type]))
					continue;
				ccci->sim_eint[sim].valid |= BIT(type);
			}
		}
		of_node_put(node);
	}
}

static int mt6797_ccci_probe(struct platform_device *pdev)
{
	struct mt6797_md1_cldma_mapping mapping;
	struct resource *ao_res, *pdn_res;
	struct wwan_port_caps at_caps = {
		.frag_len = MTK_CCCI_NET_RX_SIZE,
		.headroom_len = MTK_CCCI_HEADER_SIZE,
	};
	struct mt6797_ccci *ccci;
	struct resource smem_res;
	int ret;

	if (!enable_mt6797_ccci)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "active CLDMA ownership is not enabled\n");
	ret = mt6797_ccci_validate_resources(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid CLDMA resource topology\n");
	ccci = devm_kzalloc(&pdev->dev, sizeof(*ccci), GFP_KERNEL);
	if (!ccci)
		return -ENOMEM;
	ccci->dev = &pdev->dev;
	spin_lock_init(&ccci->diag.lock);
	spin_lock_init(&ccci->fs_diag.lock);
	spin_lock_init(&ccci->tx_stop_lock);
	atomic_set(&ccci->rpc_logs, 0);
	atomic_set(&ccci->fs_logs, 0);
	atomic_set(&ccci->fs_pending, 0);
	atomic_set(&ccci->tx_mask_owned, 0);
	atomic_set(&ccci->rx_mask_owned, 0);
	mt6797_ccci_read_sim_eint(ccci);
	atomic_set(&ccci->failed, 0);
	atomic_set(&ccci->ready, 0);
	atomic_set(&ccci->at_open, 0);
	atomic_set(&ccci->hs1_pending, 0);
	atomic_set(&ccci->teardown, 0);
	INIT_WORK(&ccci->hs1_work, mt6797_ccci_hs1_work);
	INIT_DELAYED_WORK(&ccci->handshake_timeout_work,
			  mt6797_ccci_handshake_timeout_work);
	memset(ccci->rx_sequence, 0xff, sizeof(ccci->rx_sequence));
	ccci->napi_dev = alloc_netdev_dummy(0);
	if (!ccci->napi_dev)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev,
				       mt6797_ccci_free_netdev, ccci->napi_dev);
	if (ret)
		return ret;
	netif_napi_add(ccci->napi_dev, &ccci->napi, mt6797_ccci_poll);
	ret = devm_add_action_or_reset(&pdev->dev,
				       mt6797_ccci_napi_del, &ccci->napi);
	if (ret)
		return ret;
	mt6797_md1_hs_init(&ccci->hs);
	ret = mt6797_ccci_validate_handoff(ccci, &smem_res);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "CCCI lifecycle handoff is not valid\n");
	ccci->smem_size = resource_size(&smem_res);
	ccci->smem = devm_ioremap_wc(&pdev->dev, smem_res.start, ccci->smem_size);
	if (!ccci->smem)
		return -ENOMEM;
	ao_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ap-cldma-ao");
	pdn_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ap-cldma-pdn");
	ret = mt6797_md1_cold_owner_acquire_cldma(&pdev->dev, ao_res, pdn_res, &mapping);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "AP CLDMA lease unavailable\n");
	if (!mapping.reset_completed)
		return -EPERM;
	ccci->ao = mapping.ao;
	ccci->pdn = mapping.pdn;
	ccci->runtime = devm_kzalloc(&pdev->dev, MTK_CCCI_RUNTIME_AP_SIZE,
				     GFP_KERNEL);
	if (!ccci->runtime)
		return -ENOMEM;
	ret = mt6797_ccci_alloc_rings(ccci);
	if (ret)
		return ret;
	ccci->cldma_irq = platform_get_irq_byname(pdev, "cldma");
	ccci->ccif_irq = platform_get_irq_byname(pdev, "ap-ccif");
	ccci->wdt_irq = platform_get_irq_byname(pdev, "md-wdt");
	if (ccci->cldma_irq < 0 || ccci->ccif_irq < 0 || ccci->wdt_irq < 0)
		return -EINVAL;
	ret = devm_request_irq(&pdev->dev, ccci->cldma_irq,
			       mt6797_ccci_cldma_irq, IRQF_NO_AUTOEN,
			       dev_name(&pdev->dev), ccci);
	if (ret)
		return ret;
	ret = devm_request_irq(&pdev->dev, ccci->ccif_irq,
			       mt6797_ccci_disabled_ccif_irq, IRQF_NO_AUTOEN,
			       dev_name(&pdev->dev), ccci);
	if (ret)
		return ret;
	ret = devm_request_irq(&pdev->dev, ccci->wdt_irq,
			       mt6797_ccci_wdt_irq, IRQF_NO_AUTOEN,
			       dev_name(&pdev->dev), ccci);
	if (ret)
		return ret;
	ccci->at_port = wwan_create_port(&pdev->dev, WWAN_PORT_AT,
					 &mt6797_ccci_at_ops, &at_caps, ccci);
	if (IS_ERR(ccci->at_port))
		return PTR_ERR(ccci->at_port);
	ret = devm_add_action_or_reset(&pdev->dev, mt6797_ccci_remove_port,
				       ccci->at_port);
	if (ret)
		return ret;
	wwan_port_txoff(ccci->at_port);
	ret = wwan_register_ops(&pdev->dev, &mt6797_ccci_wwan_ops, ccci, 0);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, mt6797_ccci_unregister_wwan,
				       &pdev->dev);
	if (ret)
		return ret;
	ret = mt6797_md1_hs_arm(&ccci->hs, true);
	if (ret)
		return ret;
	ccci->fs_wq = alloc_ordered_workqueue("mt6797-fs", 0);
	if (!ccci->fs_wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, mt6797_ccci_fs_destroy, ccci);
	if (ret)
		return ret;
	napi_enable(&ccci->napi);
	ret = mt6797_ccci_start(ccci);
	if (ret) {
		napi_disable(&ccci->napi);
		return ret;
	}
	platform_set_drvdata(pdev, ccci);
	schedule_delayed_work(&ccci->handshake_timeout_work,
			      MT6797_CCCI_HANDSHAKE_TIMEOUT);
	dev_info(&pdev->dev,
		 "owns seven CLDMA queues; waiting for CCCI HS1 v2\n");
	return 0;
}

static void mt6797_ccci_remove(struct platform_device *pdev)
{
	struct mt6797_ccci *ccci = platform_get_drvdata(pdev);

	if (atomic_xchg(&ccci->teardown, 1))
		return;
	disable_irq(ccci->cldma_irq);
	disable_irq(ccci->wdt_irq);
	napi_disable(&ccci->napi);
	/* STOP serializes against any worker TX before draining metadata work. */
	mt6797_ccci_quiesce(ccci, -ESHUTDOWN);
	mt6797_ccci_fs_destroy(ccci);
	cancel_delayed_work_sync(&ccci->handshake_timeout_work);
	cancel_work_sync(&ccci->hs1_work);
	kfree_skb(xchg(&ccci->hs1_skb, NULL));
	mt6797_ccci_quiesce(ccci, -ESHUTDOWN);
}

static const struct of_device_id mt6797_ccci_of_match[] = {
	{ .compatible = "mediatek,mt6797-ccci" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_ccci_of_match);

static struct platform_driver mt6797_ccci_driver = {
	.probe = mt6797_ccci_probe,
	.remove = mt6797_ccci_remove,
	.shutdown = mt6797_ccci_remove,
	.driver = {
		.name = "mt6797-ccci",
		.of_match_table = mt6797_ccci_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_ccci_driver);

MODULE_DESCRIPTION("MediaTek MT6797 MD1 CCCI CLDMA transport");
MODULE_LICENSE("GPL");
