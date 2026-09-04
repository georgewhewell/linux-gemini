/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_PROTOCOL_H
#define __MT6797_MD1_PROTOCOL_H

#include <linux/bits.h>
#include <linux/types.h>

#define MT6797_CCCI_SEQUENCE_MAX		0x7fff
#define MT6797_CLDMA_ADDRESS_MAX		GENMASK_ULL(35, 0)

#define MT6797_CCCI_CONTROL_RX		0
#define MT6797_CCCI_CONTROL_TX		1
#define MT6797_CCCI_UART2_RX		10
#define MT6797_CCCI_UART2_TX		12
#define MT6797_CCCI_UART2_QUEUE		1
#define MT6797_CCCI_MD_INIT_START_BOOT	0
#define MT6797_CCCI_MD_EXCEPTION		4
#define MT6797_CCCI_DRIVER_VERSION_ERROR	5
#define MT6797_CCCI_MD_EXCEPTION_OK	6
#define MT6797_CCCI_MD_EXCEPTION_PASS	8
#define MT6797_CCCI_MD_INIT_CHECK_ID	0x5555ffff

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

struct mt6797_ccci_rx_sequence {
	u16 last;
	bool valid;
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

/**
 * enum mt6797_ccci_boot_state - AP view of the MD1 control handshake
 * @MT6797_CCCI_BOOT_OFFLINE: no handshake is in progress
 * @MT6797_CCCI_BOOT_WAIT_HS1: waiting for the modem's initial boot message
 * @MT6797_CCCI_BOOT_SEND_RUNTIME: HS1 passed; AP runtime data must be sent
 * @MT6797_CCCI_BOOT_WAIT_HS2: runtime data was sent; waiting for boot-ready
 * @MT6797_CCCI_BOOT_READY: the modem completed the control handshake
 * @MT6797_CCCI_BOOT_FAILED: an exception or protocol violation occurred
 */
enum mt6797_ccci_boot_state {
	MT6797_CCCI_BOOT_OFFLINE,
	MT6797_CCCI_BOOT_WAIT_HS1,
	MT6797_CCCI_BOOT_SEND_RUNTIME,
	MT6797_CCCI_BOOT_WAIT_HS2,
	MT6797_CCCI_BOOT_READY,
	MT6797_CCCI_BOOT_FAILED,
};

enum mt6797_ccci_boot_event {
	MT6797_CCCI_BOOT_EVENT_HS1 = 1,
	MT6797_CCCI_BOOT_EVENT_READY,
};

int mt6797_ccci_header_encode(struct mt6797_ccci_header *header,
			      const struct mt6797_ccci_header_fields *fields);
int mt6797_ccci_header_decode(const struct mt6797_ccci_header *header,
			      struct mt6797_ccci_header_fields *fields);

int mt6797_cldma_address_encode(u64 address, enum mt6797_cldma_pointer pointer,
				__le32 *low, __le16 *debug_id);
int mt6797_cldma_address_decode(__le32 low, __le16 debug_id,
				enum mt6797_cldma_pointer pointer, u64 *address);

void mt6797_ccci_boot_begin(enum mt6797_ccci_boot_state *state);
int mt6797_ccci_boot_runtime_sent(enum mt6797_ccci_boot_state *state);
int mt6797_ccci_boot_receive(enum mt6797_ccci_boot_state *state,
			     const struct mt6797_ccci_header_fields *fields);
int mt6797_ccci_boot_rx(enum mt6797_ccci_boot_state *state,
			struct mt6797_ccci_rx_sequence *sequence,
			const struct mt6797_ccci_header_fields *fields);
void mt6797_ccci_boot_abort(enum mt6797_ccci_boot_state *state);

int mt6797_ccci_uart2_gate(enum mt6797_ccci_boot_state state,
			   u16 rx_channel, u16 tx_channel,
			   unsigned int rx_queue, unsigned int tx_queue,
			   bool rx_queue_owned, bool tx_queue_owned);

#endif /* __MT6797_MD1_PROTOCOL_H */
