/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_PROTOCOL_H
#define __MT6797_MD1_PROTOCOL_H

#include <linux/bits.h>
#include <linux/types.h>

#define MT6797_CCCI_SEQUENCE_MAX		0x7fff
#define MT6797_CLDMA_ADDRESS_MAX		GENMASK_ULL(35, 0)

struct mt6797_ccci_header {
	__le32 data[2];
	__le16 channel;
	__le16 sequence_assert;
	__le32 reserved;
} __packed;

struct mt6797_ccci_header_fields {
	u32 data[2];
	u16 channel;
	u16 sequence;
	bool assert;
	u32 reserved;
};

struct mt6797_cldma_tgpd {
	u8 flags;
	u8 checksum;
	__le16 debug_id;
	__le32 next;
	__le32 data;
	__le16 data_len;
	u8 extension_len;
	u8 reserved;
} __packed;

struct mt6797_cldma_rgpd {
	u8 flags;
	u8 checksum;
	__le16 data_allow_len;
	__le32 next;
	__le32 data;
	__le16 data_len;
	__le16 debug_id;
} __packed;

enum mt6797_cldma_pointer {
	MT6797_CLDMA_POINTER_DATA,
	MT6797_CLDMA_POINTER_NEXT,
};

int mt6797_ccci_header_encode(struct mt6797_ccci_header *header,
			      const struct mt6797_ccci_header_fields *fields);
int mt6797_ccci_header_decode(const struct mt6797_ccci_header *header,
			      struct mt6797_ccci_header_fields *fields);

int mt6797_cldma_address_encode(u64 address, enum mt6797_cldma_pointer pointer,
				__le32 *low, __le16 *debug_id);
int mt6797_cldma_address_decode(__le32 low, __le16 debug_id,
				enum mt6797_cldma_pointer pointer, u64 *address);

#endif /* __MT6797_MD1_PROTOCOL_H */
