// SPDX-License-Identifier: GPL-2.0-only
/* Fixed, hardware-independent MT6797 MD1 protocol formats. */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "protocol.h"

#define MT6797_CCCI_ASSERT		BIT(15)
#define MT6797_CCCI_SEQUENCE		GENMASK(14, 0)
#define MT6797_CLDMA_DATA_HIGH		GENMASK(3, 0)
#define MT6797_CLDMA_NEXT_HIGH		GENMASK(7, 4)

static_assert(sizeof(struct mt6797_ccci_header) == 16);
static_assert(sizeof(struct mt6797_cldma_tgpd) == 16);
static_assert(sizeof(struct mt6797_cldma_rgpd) == 16);

int mt6797_ccci_header_encode(struct mt6797_ccci_header *header,
			      const struct mt6797_ccci_header_fields *fields)
{
	u16 sequence_assert;

	if (!header || !fields)
		return -EINVAL;
	if (fields->sequence > MT6797_CCCI_SEQUENCE_MAX)
		return -ERANGE;

	sequence_assert = FIELD_PREP(MT6797_CCCI_SEQUENCE, fields->sequence);
	if (fields->assert)
		sequence_assert |= MT6797_CCCI_ASSERT;

	header->data[0] = cpu_to_le32(fields->data[0]);
	header->data[1] = cpu_to_le32(fields->data[1]);
	header->channel = cpu_to_le16(fields->channel);
	header->sequence_assert = cpu_to_le16(sequence_assert);
	header->reserved = cpu_to_le32(fields->reserved);

	return 0;
}

int mt6797_ccci_header_decode(const struct mt6797_ccci_header *header,
			      struct mt6797_ccci_header_fields *fields)
{
	u16 sequence_assert;

	if (!header || !fields)
		return -EINVAL;

	sequence_assert = le16_to_cpu(header->sequence_assert);
	fields->data[0] = le32_to_cpu(header->data[0]);
	fields->data[1] = le32_to_cpu(header->data[1]);
	fields->channel = le16_to_cpu(header->channel);
	fields->sequence = FIELD_GET(MT6797_CCCI_SEQUENCE, sequence_assert);
	fields->assert = sequence_assert & MT6797_CCCI_ASSERT;
	fields->reserved = le32_to_cpu(header->reserved);

	return 0;
}

static int mt6797_cldma_pointer_shift(enum mt6797_cldma_pointer pointer,
				      unsigned int *shift)
{
	if (!shift)
		return -EINVAL;

	switch (pointer) {
	case MT6797_CLDMA_POINTER_DATA:
		*shift = __bf_shf(MT6797_CLDMA_DATA_HIGH);
		return 0;
	case MT6797_CLDMA_POINTER_NEXT:
		*shift = __bf_shf(MT6797_CLDMA_NEXT_HIGH);
		return 0;
	default:
		return -EINVAL;
	}
}

int mt6797_cldma_address_encode(u64 address, enum mt6797_cldma_pointer pointer,
				__le32 *low, __le16 *debug_id)
{
	unsigned int shift;
	u16 debug;
	u16 mask;
	int ret;

	if (!low || !debug_id)
		return -EINVAL;
	if (address > MT6797_CLDMA_ADDRESS_MAX)
		return -ERANGE;
	ret = mt6797_cldma_pointer_shift(pointer, &shift);
	if (ret)
		return ret;

	mask = MT6797_CLDMA_DATA_HIGH << shift;
	debug = le16_to_cpu(*debug_id);
	debug &= ~mask;
	debug |= ((address >> 32) & MT6797_CLDMA_DATA_HIGH) << shift;
	*low = cpu_to_le32(lower_32_bits(address));
	*debug_id = cpu_to_le16(debug);

	return 0;
}

int mt6797_cldma_address_decode(__le32 low, __le16 debug_id,
				enum mt6797_cldma_pointer pointer, u64 *address)
{
	unsigned int shift;
	u16 debug;
	int ret;

	if (!address)
		return -EINVAL;
	ret = mt6797_cldma_pointer_shift(pointer, &shift);
	if (ret)
		return ret;

	debug = le16_to_cpu(debug_id);
	*address = le32_to_cpu(low);
	*address |= (u64)((debug >> shift) & MT6797_CLDMA_DATA_HIGH) << 32;

	return 0;
}

MODULE_DESCRIPTION("MediaTek MT6797 modem protocol helpers");
MODULE_LICENSE("GPL");
