// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-independent MT6797 CLDMA receive-ring construction. */

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/byteorder/generic.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "cldma.h"

int mt6797_cldma_rx_ring_build(struct mt6797_cldma_rgpd *ring,
			       unsigned int count, u64 ring_dma,
			       u64 data_dma, size_t data_stride)
{
	u64 ring_end;
	u64 data_end;
	unsigned int i;

	if (!ring || !count)
		return -EINVAL;
	if (!IS_ALIGNED(ring_dma, sizeof(*ring)))
		return -EINVAL;
	if (data_stride < sizeof(struct mt6797_ccci_header) ||
	    data_stride > U16_MAX)
		return -EINVAL;
	if (check_mul_overflow((u64)count, (u64)sizeof(*ring), &ring_end) ||
	    check_add_overflow(ring_dma, ring_end - 1, &ring_end) ||
	    ring_end > U32_MAX)
		return -ERANGE;
	if (check_mul_overflow((u64)count, (u64)data_stride, &data_end) ||
	    check_add_overflow(data_dma, data_end - 1, &data_end) ||
	    data_end > MT6797_CLDMA_ADDRESS_MAX)
		return -ERANGE;

	memset(ring, 0, count * sizeof(*ring));
	for (i = 0; i < count; i++) {
		u64 next = ring_dma + ((i + 1) % count) * sizeof(*ring);
		u64 buffer = data_dma + i * data_stride;
		int ret;

		ring[i].flags = MT6797_CLDMA_GPD_IOC | MT6797_CLDMA_GPD_HWO;
		ring[i].data_allow_len = cpu_to_le16(data_stride);
		ret = mt6797_cldma_address_encode(next,
						  MT6797_CLDMA_POINTER_NEXT,
						  &ring[i].next, &ring[i].debug_id);
		if (ret)
			return ret;
		ret = mt6797_cldma_address_encode(buffer,
						  MT6797_CLDMA_POINTER_DATA,
						  &ring[i].data, &ring[i].debug_id);
		if (ret)
			return ret;
	}

	return 0;
}

int mt6797_cldma_rx_read(const struct mt6797_cldma_rgpd *gpd,
			 const void *data, size_t data_capacity,
			 u64 expected_next_dma, u64 expected_data_dma,
			 size_t *packet_len,
			 struct mt6797_ccci_header_fields *fields)
{
	u64 data_dma;
	u64 next_dma;
	size_t length;
	int ret;

	if (!gpd || !data || !packet_len || !fields)
		return -EINVAL;
	if (READ_ONCE(gpd->flags) & MT6797_CLDMA_GPD_HWO)
		return -EAGAIN;
	if (READ_ONCE(gpd->flags) != MT6797_CLDMA_GPD_IOC)
		return -EPROTO;
	if (data_capacity < sizeof(struct mt6797_ccci_header) ||
	    data_capacity > U16_MAX ||
	    le16_to_cpu(READ_ONCE(gpd->data_allow_len)) != data_capacity)
		return -EPROTO;

	ret = mt6797_cldma_address_decode(READ_ONCE(gpd->next),
					  READ_ONCE(gpd->debug_id),
					  MT6797_CLDMA_POINTER_NEXT,
					  &next_dma);
	if (ret)
		return ret;
	ret = mt6797_cldma_address_decode(READ_ONCE(gpd->data),
					  READ_ONCE(gpd->debug_id),
					  MT6797_CLDMA_POINTER_DATA,
					  &data_dma);
	if (ret)
		return ret;
	if (next_dma != expected_next_dma || data_dma != expected_data_dma)
		return -EPROTO;

	length = le16_to_cpu(READ_ONCE(gpd->data_len));
	if (length < sizeof(struct mt6797_ccci_header) ||
	    length > data_capacity ||
	    length > le16_to_cpu(READ_ONCE(gpd->data_allow_len)))
		return -EMSGSIZE;

	ret = mt6797_ccci_header_decode(data, fields);
	if (ret)
		return ret;

	*packet_len = length;
	return 0;
}

int mt6797_cldma_rx_rearm(struct mt6797_cldma_rgpd *gpd,
			  size_t data_capacity)
{
	if (!gpd || data_capacity < sizeof(struct mt6797_ccci_header) ||
	    data_capacity > U16_MAX)
		return -EINVAL;
	if (READ_ONCE(gpd->flags) != MT6797_CLDMA_GPD_IOC ||
	    le16_to_cpu(READ_ONCE(gpd->data_allow_len)) != data_capacity)
		return -EPROTO;

	WRITE_ONCE(gpd->data_len, cpu_to_le16(0));
	/* Publish all descriptor fields before returning ownership to CLDMA. */
	dma_wmb();
	WRITE_ONCE(gpd->flags, MT6797_CLDMA_GPD_IOC | MT6797_CLDMA_GPD_HWO);

	return 0;
}
