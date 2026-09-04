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

void mt6797_ccci_boot_begin(enum mt6797_ccci_boot_state *state)
{
	if (state)
		*state = MT6797_CCCI_BOOT_WAIT_HS1;
}

int mt6797_ccci_boot_runtime_sent(enum mt6797_ccci_boot_state *state)
{
	if (!state)
		return -EINVAL;
	if (*state != MT6797_CCCI_BOOT_SEND_RUNTIME) {
		*state = MT6797_CCCI_BOOT_FAILED;
		return -EPROTO;
	}

	*state = MT6797_CCCI_BOOT_WAIT_HS2;
	return 0;
}

int mt6797_ccci_boot_receive(enum mt6797_ccci_boot_state *state,
			     const struct mt6797_ccci_header_fields *fields)
{
	if (!state || !fields)
		return -EINVAL;
	if (fields->channel != MT6797_CCCI_CONTROL_RX)
		return -ENOMSG;

	switch (fields->data[1]) {
	case MT6797_CCCI_MD_INIT_START_BOOT:
		if (*state == MT6797_CCCI_BOOT_WAIT_HS1 &&
		    fields->reserved == MT6797_CCCI_MD_INIT_CHECK_ID) {
			*state = MT6797_CCCI_BOOT_SEND_RUNTIME;
			return MT6797_CCCI_BOOT_EVENT_HS1;
		}
		if (*state == MT6797_CCCI_BOOT_WAIT_HS2 &&
		    fields->reserved != MT6797_CCCI_MD_INIT_CHECK_ID) {
			*state = MT6797_CCCI_BOOT_READY;
			return MT6797_CCCI_BOOT_EVENT_READY;
		}
		break;
	case MT6797_CCCI_DRIVER_VERSION_ERROR:
		*state = MT6797_CCCI_BOOT_FAILED;
		return -EPROTONOSUPPORT;
	case MT6797_CCCI_MD_EXCEPTION:
	case MT6797_CCCI_MD_EXCEPTION_OK:
	case MT6797_CCCI_MD_EXCEPTION_PASS:
		*state = MT6797_CCCI_BOOT_FAILED;
		return -EIO;
	default:
		break;
	}

	*state = MT6797_CCCI_BOOT_FAILED;
	return -EPROTO;
}

void mt6797_ccci_boot_abort(enum mt6797_ccci_boot_state *state)
{
	if (state)
		*state = MT6797_CCCI_BOOT_FAILED;
}

MODULE_DESCRIPTION("MediaTek MT6797 modem protocol helpers");
MODULE_LICENSE("GPL");
