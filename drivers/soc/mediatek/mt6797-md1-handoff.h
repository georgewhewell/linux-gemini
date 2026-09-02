/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_HANDOFF_H
#define __MT6797_MD1_HANDOFF_H

#include <linux/dma-mapping.h>
#include <linux/types.h>

#define MT6797_MD1_RESOURCE_COUNT	18
#define MT6797_MD1_IRQ_COUNT		3
#define MT6797_MD1_GPD_HWO		BIT(0)
#define MT6797_MD1_GPD_IOC		BIT(7)
#define MT6797_MD1_GPD_SIZE		16
#define MT6797_MD1_DMA_ADDRESS_BITS	36
#define MT6797_MD1_REMAP_SLOT_SIZE	0x02000000ULL
#define MT6797_MD1_REMAP_WINDOW_SIZE	(8 * MT6797_MD1_REMAP_SLOT_SIZE)

struct mt6797_md1_resource {
	u64 start;
	u32 size;
};

struct mt6797_md1_layout {
	u64 firmware_base;
	u64 firmware_size;
	u64 shared_base;
	u64 shared_size;
};

struct mt6797_md1_tgpd {
	u8 flags;
	u8 checksum;
	__le16 debug_id;
	__le32 next;
	__le32 data;
	__le16 data_len;
	u8 ext_len;
	u8 reserved;
} __packed;

struct mt6797_md1_rgpd {
	u8 flags;
	u8 checksum;
	__le16 data_allow_len;
	__le32 next;
	__le32 data;
	__le16 data_len;
	__le16 debug_id;
} __packed;

struct mt6797_md1_q0 {
	struct mt6797_md1_tgpd tx;
	struct mt6797_md1_rgpd rx;
};

struct mt6797_md1_remap_values {
	u32 bank0_map0;
	u32 bank0_map1;
	u32 bank4_map0;
	u32 bank4_map1;
};

struct mt6797_md1_remap_plan {
	struct mt6797_md1_remap_values physical;
	struct mt6797_md1_remap_values emi_relative;
	u64 bank4_base;
};

int mt6797_md1_resources_validate(const struct mt6797_md1_resource *resources,
				  size_t count);
int mt6797_md1_layout_validate(const struct mt6797_md1_layout *layout);
int mt6797_md1_remap_plan_build(const struct mt6797_md1_layout *layout,
				struct mt6797_md1_remap_plan *plan);
int mt6797_md1_q0_init(struct mt6797_md1_q0 *q0, dma_addr_t tx_dma,
		       dma_addr_t rx_dma);

#endif /* __MT6797_MD1_HANDOFF_H */
