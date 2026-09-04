/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AHB_HIF_H
#define __MT6797_WIFI_AHB_HIF_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_WIFI_AHB_HIF_SIZE		0x1100
#define MT6797_WIFI_AHB_BLOCK_SIZE		512
/* 2352-byte vendor RX maximum rounded up to a 512-byte HIF block. */
#define MT6797_WIFI_AHB_MAX_TRANSFER_SIZE	2560

struct mt6797_wifi_ahb_hif {
	void __iomem *base;
	struct mutex lock; /* Serializes command and data portal accesses. */
};

int mt6797_wifi_ahb_hif_init(struct mt6797_wifi_ahb_hif *hif,
			     void __iomem *base);

int mt6797_wifi_ahb_cmd52_read(struct mt6797_wifi_ahb_hif *hif, u8 function,
			       u32 address, u8 *value);
int mt6797_wifi_ahb_cmd52_write(struct mt6797_wifi_ahb_hif *hif, u8 function,
				u32 address, u8 value);

int mt6797_wifi_ahb_cmd53_read(struct mt6797_wifi_ahb_hif *hif, u32 address,
			       void *buffer, size_t length);
int mt6797_wifi_ahb_cmd53_write(struct mt6797_wifi_ahb_hif *hif, u32 address,
				const void *buffer, size_t length);

int mt6797_wifi_ahb_pack_cmd52(bool write, u8 function, u32 address, u8 value,
			       u32 *command);
int mt6797_wifi_ahb_plan_cmd53(bool write, u32 address, size_t length,
			       u32 *command, size_t *transfer_length);

#endif /* __MT6797_WIFI_AHB_HIF_H */
