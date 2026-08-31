/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 CLDMA invariant resource and descriptor contract.
 *
 * This describes only facts common to the vendor MD1 CCCI CLDMA path: six
 * MMIO regions, three GIC source interrupts, LK-attested CCCI memory, and the
 * 16-byte GPD wire format. It has no Device Tree parser, resource mapping,
 * DMA allocation/mapping, MMIO, IRQ, CCCI, power, reset, firmware, SIM, or RF.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_CLDMA_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_CLDMA_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct mt6797_md1_mpu_remap_layout;

#define MT6797_MD1_CLDMA_REGION_COUNT 6U
#define MT6797_MD1_CLDMA_IRQ_COUNT 3U
#define MT6797_MD1_CLDMA_GPD_BYTES 16U
#define MT6797_MD1_CLDMA_GPD_ALIGN 16U
#define MT6797_MD1_CLDMA_ADDR_BITS 36U
#define MT6797_MD1_CLDMA_ADDR_MAX ((1ULL << MT6797_MD1_CLDMA_ADDR_BITS) - 1)
#define MT6797_MD1_CLDMA_TX_GPD_IOC 0x80U
#define MT6797_MD1_CLDMA_RX_GPD_IOC 0x80U

enum mt6797_md1_cldma_region_id {
	MT6797_MD1_CLDMA_AP_AO,
	MT6797_MD1_CLDMA_MD_AO,
	MT6797_MD1_CLDMA_AP_PDN,
	MT6797_MD1_CLDMA_MD_PDN,
	MT6797_MD1_CLDMA_AP_CCIF,
	MT6797_MD1_CLDMA_MD_CCIF,
};

enum mt6797_md1_cldma_irq_id {
	MT6797_MD1_CLDMA_IRQ,
	MT6797_MD1_AP_CCIF_IRQ,
	MT6797_MD1_MD_WDT_IRQ,
};

enum mt6797_md1_cldma_irq_type {
	MT6797_MD1_CLDMA_IRQ_LEVEL_HIGH,
	MT6797_MD1_CLDMA_IRQ_LEVEL_LOW,
	MT6797_MD1_CLDMA_IRQ_EDGE_FALLING,
};

struct mt6797_md1_cldma_region {
	u64 base;
	u32 size;
};

/* Source GIC numbers, not Linux virtual IRQ numbers. */
struct mt6797_md1_cldma_irq {
	u32 hwirq;
	enum mt6797_md1_cldma_irq_type type;
};

struct mt6797_md1_cldma_resources {
	u32 md_id;
	u32 capability;
	u32 md_smem_size;
	struct mt6797_md1_cldma_region region[MT6797_MD1_CLDMA_REGION_COUNT];
	struct mt6797_md1_cldma_irq irq[MT6797_MD1_CLDMA_IRQ_COUNT];
};

/* Exact packed vendor GPD layouts, including 36-bit pointer high nibbles. */
struct mt6797_md1_cldma_tgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	__le16 debug_id;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	u8 desc_ext_len;
	u8 non_used;
} __packed;

struct mt6797_md1_cldma_rgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	__le16 data_allow_len;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	__le16 debug_id;
} __packed;

int mt6797_md1_cldma_resources_validate(
	const struct mt6797_md1_cldma_resources *resources);
int mt6797_md1_cldma_lk_carveout_validate(
	const struct mt6797_md1_mpu_remap_layout *layout);

/* No allocation, mapping, checksum or HWO transition occurs in these helpers. */
int mt6797_md1_cldma_tx_ring_init(struct mt6797_md1_cldma_tgpd *const *gpd,
				  const dma_addr_t *gpd_dma, size_t count);
int mt6797_md1_cldma_rx_ring_init(struct mt6797_md1_cldma_rgpd *const *gpd,
				  const dma_addr_t *gpd_dma,
				  const dma_addr_t *data_dma,
				  const u16 *data_allow_len, size_t count);
int mt6797_md1_cldma_tgpd_prepare_tx(struct mt6797_md1_cldma_tgpd *gpd,
				     dma_addr_t data_dma, u16 data_len);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_CLDMA_H */
