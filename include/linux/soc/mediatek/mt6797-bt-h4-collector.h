/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded H4 command-response collector for the one-shot MT6797 BT proof. */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_BT_H4_COLLECTOR_H
#define __LINUX_SOC_MEDIATEK_MT6797_BT_H4_COLLECTOR_H

#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define MT6797_BT_H4_EVENT_MAX_PAYLOAD	255
#define MT6797_BT_H4_ACL_MAX_PAYLOAD		1024
#define MT6797_BT_H4_SCO_MAX_PAYLOAD		255
#define MT6797_BT_H4_MAX_PACKET (1 + 4 + MT6797_BT_H4_ACL_MAX_PAYLOAD)

enum mt6797_bt_h4_rx_state {
	MT6797_BT_H4_RX_TYPE,
	MT6797_BT_H4_RX_HEADER,
	MT6797_BT_H4_RX_BODY,
};

/*
 * There is no allocation or hidden callback lifetime. The embedding STP owner
 * synchronously quiesces RX, then fences this caller-owned object, before its
 * storage can be released. A late feed after fence is rejected.
 */
struct mt6797_bt_h4_collector {
	spinlock_t lock;
	struct completion command_complete;
	enum mt6797_bt_h4_rx_state rx_state;
	u8 packet[MT6797_BT_H4_MAX_PACKET];
	size_t packet_length;
	size_t header_length;
	size_t header_received;
	size_t packet_expected;
	u8 packet_type;
	bool fenced;
	bool command_pending;
	u16 expected_opcode;
	int command_result;
	u8 reply[1 + 2 + MT6797_BT_H4_EVENT_MAX_PAYLOAD];
	size_t reply_length;
	/* Bounded accounting safely classifies non-matching traffic as async. */
	u64 async_packets;
	u64 async_bytes;
	u64 malformed_packets;
};

void mt6797_bt_h4_collector_init(struct mt6797_bt_h4_collector *collector);
int mt6797_bt_h4_collector_begin(struct mt6797_bt_h4_collector *collector,
				  u16 opcode);
int mt6797_bt_h4_collector_receive(struct mt6797_bt_h4_collector *collector,
				    const u8 *data, size_t length);
int mt6797_bt_h4_collector_wait(struct mt6797_bt_h4_collector *collector,
				 unsigned long timeout, u8 *reply,
				 size_t reply_capacity, size_t *reply_length);
void mt6797_bt_h4_collector_fence(struct mt6797_bt_h4_collector *collector);

#endif
