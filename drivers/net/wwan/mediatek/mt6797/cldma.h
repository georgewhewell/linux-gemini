/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_CLDMA_H
#define __MT6797_MD1_CLDMA_H

#include <linux/types.h>

#include "protocol.h"

#define MT6797_CLDMA_RX_CONTROL_QUEUE	0
#define MT6797_CLDMA_RX_RING_COUNT	16
#define MT6797_CLDMA_RX_BUFFER_SIZE	3584

#define MT6797_CLDMA_GPD_HWO		BIT(0)
#define MT6797_CLDMA_GPD_IOC		BIT(7)

int mt6797_cldma_rx_ring_build(struct mt6797_cldma_rgpd *ring,
			       unsigned int count, u64 ring_dma,
			       u64 data_dma, size_t data_stride);
int mt6797_cldma_rx_descriptor_read(const struct mt6797_cldma_rgpd *gpd,
				    const void *data, size_t data_capacity,
				    struct mt6797_ccci_header_fields *fields);

#endif /* __MT6797_MD1_CLDMA_H */
