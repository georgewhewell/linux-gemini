// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 CLDMA descriptor construction and ownership helpers. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "mtk_mt6797_cldma.h"

static_assert(sizeof(struct mtk_mt6797_cldma_tgpd) ==
	      MTK_MT6797_CLDMA_GPD_BYTES);
static_assert(sizeof(struct mtk_mt6797_cldma_rgpd) ==
	      MTK_MT6797_CLDMA_GPD_BYTES);

static bool mtk_mt6797_cldma_addr_valid(dma_addr_t addr)
{
	return (u64)addr <= MTK_MT6797_CLDMA_ADDR_MAX;
}

static int mtk_mt6797_cldma_set_pointer(__le32 *low, __le16 *debug,
					dma_addr_t addr, bool next)
{
	u16 value;

	if (!mtk_mt6797_cldma_addr_valid(addr))
		return -ERANGE;

	value = le16_to_cpu(*debug);
	if (next)
		value = (value & 0xff0f) | (((u64)addr >> 28) & 0x00f0);
	else
		value = (value & 0xfff0) | (((u64)addr >> 32) & 0x000f);
	*low = cpu_to_le32((u32)addr);
	*debug = cpu_to_le16(value);

	return 0;
}

/* Queue-start registers carry only the low 32 bits of a ring root. */
static int mtk_mt6797_cldma_gpd_dma_valid(dma_addr_t addr)
{
	if (!IS_ALIGNED(addr, MTK_MT6797_CLDMA_GPD_ALIGN))
		return -EINVAL;
	if ((u64)addr > U32_MAX)
		return -ERANGE;

	return 0;
}

int mtk_mt6797_cldma_tgpd_prepare_tx(struct mtk_mt6797_cldma_tgpd *gpd,
				     dma_addr_t data_dma, u16 data_len)
{
	int ret;

	if (!gpd || !data_len)
		return -EINVAL;

	ret = mtk_mt6797_cldma_set_pointer(&gpd->data_buff_bd_ptr,
					   &gpd->debug_id, data_dma, false);
	if (ret)
		return ret;
	gpd->data_buff_len = cpu_to_le16(data_len);

	return 0;
}

int mtk_mt6797_cldma_tx_ring_init(struct mtk_mt6797_cldma_tgpd *const *gpd,
				  const dma_addr_t *gpd_dma, size_t count)
{
	size_t i;
	int ret;

	if (!gpd || !gpd_dma || !count)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		if (!gpd[i])
			return -EINVAL;
		ret = mtk_mt6797_cldma_gpd_dma_valid(gpd_dma[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < count; i++) {
		memset(gpd[i], 0, sizeof(*gpd[i]));
		gpd[i]->gpd_flags = MTK_MT6797_CLDMA_TX_GPD_IOC;
		ret = mtk_mt6797_cldma_set_pointer(&gpd[i]->next_gpd_ptr,
						   &gpd[i]->debug_id,
						   gpd_dma[(i + 1) % count],
						   true);
		if (ret)
			return ret;
	}

	return 0;
}

int mtk_mt6797_cldma_rx_ring_init(struct mtk_mt6797_cldma_rgpd *const *gpd,
				  const dma_addr_t *gpd_dma,
				  const dma_addr_t *data_dma,
				  const u16 *data_allow_len, size_t count)
{
	size_t i;
	int ret;

	if (!gpd || !gpd_dma || !data_dma || !data_allow_len || !count)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		if (!gpd[i] || !data_allow_len[i])
			return -EINVAL;
		ret = mtk_mt6797_cldma_gpd_dma_valid(gpd_dma[i]);
		if (ret)
			return ret;
		if (!mtk_mt6797_cldma_addr_valid(data_dma[i]))
			return -ERANGE;
	}

	for (i = 0; i < count; i++) {
		memset(gpd[i], 0, sizeof(*gpd[i]));
		gpd[i]->gpd_flags = MTK_MT6797_CLDMA_RX_GPD_IOC;
		gpd[i]->data_allow_len = cpu_to_le16(data_allow_len[i]);
		ret = mtk_mt6797_cldma_set_pointer(&gpd[i]->data_buff_bd_ptr,
						   &gpd[i]->debug_id,
						   data_dma[i], false);
		if (ret)
			return ret;
		ret = mtk_mt6797_cldma_set_pointer(&gpd[i]->next_gpd_ptr,
						   &gpd[i]->debug_id,
						   gpd_dma[(i + 1) % count],
						   true);
		if (ret)
			return ret;
	}

	return 0;
}

int mtk_mt6797_cldma_tgpd_publish(struct mtk_mt6797_cldma_tgpd *gpd)
{
	if (!gpd || (gpd->gpd_flags & MTK_MT6797_CLDMA_GPD_HWO) ||
	    !le16_to_cpu(gpd->data_buff_len))
		return -EINVAL;

	/* All descriptor and payload writes must precede ownership transfer. */
	dma_wmb();
	WRITE_ONCE(gpd->gpd_flags,
		   MTK_MT6797_CLDMA_TX_GPD_IOC | MTK_MT6797_CLDMA_GPD_HWO);

	return 0;
}

int mtk_mt6797_cldma_tgpd_reclaim(struct mtk_mt6797_cldma_tgpd *gpd,
				  u16 *data_len)
{
	u16 debug;

	if (!gpd || !data_len)
		return -EINVAL;
	if (READ_ONCE(gpd->gpd_flags) & MTK_MT6797_CLDMA_GPD_HWO)
		return -EAGAIN;
	dma_rmb();
	*data_len = le16_to_cpu(gpd->data_buff_len);
	if (!*data_len)
		return -EBADMSG;

	gpd->data_buff_len = 0;
	gpd->data_buff_bd_ptr = 0;
	debug = le16_to_cpu(gpd->debug_id) & 0xfff0;
	gpd->debug_id = cpu_to_le16(debug);
	WRITE_ONCE(gpd->gpd_flags, MTK_MT6797_CLDMA_TX_GPD_IOC);

	return 0;
}

int mtk_mt6797_cldma_rgpd_publish(struct mtk_mt6797_cldma_rgpd *gpd,
				  dma_addr_t data_dma, u16 data_allow_len)
{
	int ret;

	if (!gpd || !data_allow_len ||
	    (gpd->gpd_flags & MTK_MT6797_CLDMA_GPD_HWO))
		return -EINVAL;

	ret = mtk_mt6797_cldma_set_pointer(&gpd->data_buff_bd_ptr,
					   &gpd->debug_id, data_dma, false);
	if (ret)
		return ret;
	gpd->data_allow_len = cpu_to_le16(data_allow_len);
	gpd->data_buff_len = 0;

	/* The replacement buffer and descriptor must be visible before HWO. */
	dma_wmb();
	WRITE_ONCE(gpd->gpd_flags,
		   MTK_MT6797_CLDMA_RX_GPD_IOC | MTK_MT6797_CLDMA_GPD_HWO);

	return 0;
}

int mtk_mt6797_cldma_rgpd_consume(struct mtk_mt6797_cldma_rgpd *gpd,
				  u16 *data_len)
{
	u16 allow;

	if (!gpd || !data_len)
		return -EINVAL;
	if (READ_ONCE(gpd->gpd_flags) & MTK_MT6797_CLDMA_GPD_HWO)
		return -EAGAIN;
	dma_rmb();
	allow = le16_to_cpu(gpd->data_allow_len);
	*data_len = le16_to_cpu(gpd->data_buff_len);
	if (!*data_len || *data_len > allow)
		return -EBADMSG;

	return 0;
}

void mtk_mt6797_cldma_irq_decode(u32 tx_status, u32 tx_mask,
				 u32 rx_status, u32 rx_mask,
				 struct mtk_mt6797_cldma_irq_pending *pending)
{
	u32 tx = tx_status & ~tx_mask;
	u32 rx = rx_status & ~rx_mask;

	if (!pending)
		return;

	pending->tx_done = tx & MTK_MT6797_CLDMA_ACTIVE_QUEUES;
	pending->rx_done = rx & MTK_MT6797_CLDMA_ACTIVE_QUEUES;
	pending->tx_error = (tx >> 16) & MTK_MT6797_CLDMA_ACTIVE_QUEUES;
	pending->rx_error = (rx >> 16) & MTK_MT6797_CLDMA_ACTIVE_QUEUES;
}
