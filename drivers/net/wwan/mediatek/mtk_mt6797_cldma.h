/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_CLDMA_H
#define __MTK_MT6797_CLDMA_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

#define MTK_MT6797_CLDMA_GPD_BYTES	16U
#define MTK_MT6797_CLDMA_GPD_ALIGN	16U
#define MTK_MT6797_CLDMA_ADDR_BITS	36U
#define MTK_MT6797_CLDMA_ADDR_MAX	DMA_BIT_MASK(MTK_MT6797_CLDMA_ADDR_BITS)
#define MTK_MT6797_CLDMA_TX_GPD_IOC	0x80U
#define MTK_MT6797_CLDMA_RX_GPD_IOC	0x80U
#define MTK_MT6797_CLDMA_GPD_HWO		0x01U
#define MTK_MT6797_CLDMA_ACTIVE_QUEUES	0x7fU

struct mtk_mt6797_cldma_irq_pending {
	u8 tx_done;
	u8 rx_done;
	u8 tx_error;
	u8 rx_error;
};

/* Exact packed firmware ABI, including 36-bit payload pointer high bits. */
struct mtk_mt6797_cldma_tgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	__le16 debug_id;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	u8 desc_ext_len;
	u8 non_used;
} __packed;

struct mtk_mt6797_cldma_rgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	__le16 data_allow_len;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	__le16 debug_id;
} __packed;

int mtk_mt6797_cldma_tx_ring_init(struct mtk_mt6797_cldma_tgpd *const *gpd,
				  const dma_addr_t *gpd_dma, size_t count);
int mtk_mt6797_cldma_rx_ring_init(struct mtk_mt6797_cldma_rgpd *const *gpd,
				  const dma_addr_t *gpd_dma,
				  const dma_addr_t *data_dma,
				  const u16 *data_allow_len, size_t count);
int mtk_mt6797_cldma_tgpd_prepare_tx(struct mtk_mt6797_cldma_tgpd *gpd,
				     dma_addr_t data_dma, u16 data_len);
int mtk_mt6797_cldma_tgpd_publish(struct mtk_mt6797_cldma_tgpd *gpd);
int mtk_mt6797_cldma_tgpd_reclaim(struct mtk_mt6797_cldma_tgpd *gpd,
				  u16 *data_len);
int mtk_mt6797_cldma_rgpd_publish(struct mtk_mt6797_cldma_rgpd *gpd,
				  dma_addr_t data_dma, u16 data_allow_len);
int mtk_mt6797_cldma_rgpd_consume(struct mtk_mt6797_cldma_rgpd *gpd,
				  u16 *data_len);
void mtk_mt6797_cldma_irq_decode(u32 tx_status, u32 tx_mask,
				 u32 rx_status, u32 rx_mask,
				 struct mtk_mt6797_cldma_irq_pending *pending);

#endif /* __MTK_MT6797_CLDMA_H */
