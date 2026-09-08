// SPDX-License-Identifier: GPL-2.0-only
/* Bounded runtime command and event codecs for MT6797 GEN3 WLAN. */

#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"

#define MT6797_WIFI_COMMAND_PQ_ID	0x8000U
#define MT6797_WIFI_COMMAND_PACKET_TYPE	0xa0U
#define MT6797_WIFI_EVENT_PACKET_TYPE	0xe000U
#define MT6797_WIFI_COMMAND_BASIC_CONFIG	0x02U

#define MT6797_WIFI_RX_PACKET_TYPE_MASK		0xe00fU
#define MT6797_WIFI_RX_PACKET_TYPE_EVENT		0xe000U
#define MT6797_WIFI_RX_PACKET_TYPE_SW_FRAME	0xe001U
#define MT6797_WIFI_RX_GROUPS_MASK		GENMASK(12, 9)
#define MT6797_WIFI_RX_GROUPS_SHIFT		9
#define MT6797_WIFI_RX_HEADER_LENGTH_MASK	GENMASK(5, 0)
#define MT6797_WIFI_RX_HEADER_OFFSET		BIT(6)
#define MT6797_WIFI_RX_HEADER_TRANSLATED	BIT(7)
#define MT6797_WIFI_RX_STATUS_ERROR_MASK		(BIT(1) | BIT(3) | BIT(4))

#define MT6797_WIFI_TX_ETHERTYPE_WORD_OFFSET	20U
#define MT6797_WIFI_TX_QUEUE_DATA		(4U << 3)
#define MT6797_WIFI_TX_QUEUE_EAPOL		(BIT(7) | BIT(3))
#define MT6797_WIFI_TX_HEADER_ETHERNET_LONG	(BIT(7) | BIT(4))
#define MT6797_WIFI_TX_PROTECTED		BIT(7)
#define MT6797_WIFI_TX_ARP_PRIORITY		(6U << 4)
#define MT6797_WIFI_TX_FIXED_RATE		BIT(7)
#define MT6797_WIFI_TX_RETRY_LIMIT		(30U << 11)
#define MT6797_WIFI_TX_FIXED_RATE_MAX		GENMASK(13, 0)

int mt6797_wifi_command_init(u8 *command, size_t capacity,
			     size_t payload_length, u8 id, bool set,
			     u8 sequence)
{
	size_t packet_length;

	if (!command)
		return -EINVAL;
	if (check_add_overflow(payload_length,
			       (size_t)MT6797_WIFI_COMMAND_HEADER_LENGTH,
			       &packet_length))
		return -EOVERFLOW;
	if (packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    packet_length > capacity)
		return -EMSGSIZE;

	memset(command, 0, packet_length);
	put_unaligned_le16(packet_length, command);
	put_unaligned_le16(MT6797_WIFI_COMMAND_PQ_ID, command + 2);
	command[4] = id;
	command[5] = MT6797_WIFI_COMMAND_PACKET_TYPE;
	command[6] = set;
	command[7] = sequence;

	return 0;
}

int mt6797_wifi_basic_config_build(u8 *command, size_t capacity, u8 sequence)
{
	/* Native 802.11 and both checksum-offload masks stay disabled. */
	return mt6797_wifi_command_init(command, capacity,
			MT6797_WIFI_BASIC_CONFIG_LENGTH -
			MT6797_WIFI_COMMAND_HEADER_LENGTH,
			MT6797_WIFI_COMMAND_BASIC_CONFIG, true, sequence);
}

int mt6797_wifi_event_parse(const u8 *packet, size_t available,
			    struct mt6797_wifi_event_view *event)
{
	size_t packet_length;

	if (!packet || !event)
		return -EINVAL;
	if (available < MT6797_WIFI_EVENT_HEADER_LENGTH)
		return -EMSGSIZE;

	packet_length = get_unaligned_le16(packet);
	if (packet_length < MT6797_WIFI_EVENT_HEADER_LENGTH ||
	    packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    packet_length > available)
		return -EMSGSIZE;
	if (get_unaligned_le16(packet + 2) != MT6797_WIFI_EVENT_PACKET_TYPE)
		return -EPROTO;
	/* GEN3 firmware uses the reserved header bytes; they are not MBZ. */

	event->payload = packet + MT6797_WIFI_EVENT_HEADER_LENGTH;
	event->payload_length = packet_length -
		MT6797_WIFI_EVENT_HEADER_LENGTH;
	event->packet_length = packet_length;
	event->id = packet[4];
	event->sequence = packet[5];

	return 0;
}

static int mt6797_wifi_rx_group_offset(u8 groups, size_t *offset)
{
	static const u8 lengths[] = { 16, 8, 24, 16 };
	static const u8 order[] = { 3, 0, 1, 2 };
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(order); index++) {
		u8 group = order[index];

		if ((groups & BIT(group)) &&
		    check_add_overflow(*offset, (size_t)lengths[group], offset))
			return -EOVERFLOW;
	}

	return 0;
}

int mt6797_wifi_rx_parse(const u8 *packet, size_t available,
			 struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_event_view event;
	size_t packet_length;
	size_t offset;
	u16 packet_type;
	u16 status;
	u8 header;
	u8 groups;
	int ret;

	if (!packet || !frame)
		return -EINVAL;
	*frame = (struct mt6797_wifi_rx_view) {};
	if (available < sizeof(u32))
		return -EMSGSIZE;

	packet_type = get_unaligned_le16(packet + 2);
	if (packet_type == MT6797_WIFI_RX_PACKET_TYPE_EVENT) {
		ret = mt6797_wifi_event_parse(packet, available, &event);
		if (ret)
			return ret;
		frame->packet = packet;
		frame->payload = event.payload;
		frame->payload_length = event.payload_length;
		frame->packet_length = event.packet_length;
		frame->kind = MT6797_WIFI_RX_EVENT;
		frame->event_id = event.id;
		frame->sequence = event.sequence;
		return 0;
	}
	if (available < MT6797_WIFI_RX_DESCRIPTOR_LENGTH)
		return -EMSGSIZE;

	packet_length = get_unaligned_le16(packet);
	if (packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    packet_length > available ||
	    packet_length < MT6797_WIFI_RX_DESCRIPTOR_LENGTH)
		return -EMSGSIZE;
	status = get_unaligned_le16(packet + 10);
	if (status & MT6797_WIFI_RX_STATUS_ERROR_MASK)
		return -EBADMSG;

	header = packet[6];
	groups = (packet_type & MT6797_WIFI_RX_GROUPS_MASK) >>
		MT6797_WIFI_RX_GROUPS_SHIFT;
	offset = MT6797_WIFI_RX_DESCRIPTOR_LENGTH;
	ret = mt6797_wifi_rx_group_offset(groups, &offset);
	if (ret)
		return ret;
	if ((header & MT6797_WIFI_RX_HEADER_OFFSET) &&
	    check_add_overflow(offset, (size_t)2, &offset))
		return -EOVERFLOW;
	if (offset >= packet_length)
		return -EPROTO;

	frame->packet = packet;
	frame->payload = packet + offset;
	frame->payload_length = packet_length - offset;
	frame->packet_length = packet_length;
	frame->channel = packet[5];
	frame->header_length = header & MT6797_WIFI_RX_HEADER_LENGTH_MASK;
	frame->wlan_index = packet[8];
	frame->header_translated = header & MT6797_WIFI_RX_HEADER_TRANSLATED;
	if (frame->header_length > frame->payload_length)
		return -EPROTO;

	if (!frame->header_translated &&
	    (packet_type & MT6797_WIFI_RX_PACKET_TYPE_MASK) ==
	    MT6797_WIFI_RX_PACKET_TYPE_SW_FRAME) {
		if (frame->payload_length < sizeof(u16))
			return -EPROTO;
		frame->kind = MT6797_WIFI_RX_IEEE80211;
		return 0;
	}
	if (!frame->header_translated)
		return -EOPNOTSUPP;
	if (frame->payload_length < ETH_HLEN)
		return -EPROTO;
	frame->kind = MT6797_WIFI_RX_ETHERNET;

	return 0;
}

int mt6797_wifi_tx_build(u8 *packet, size_t capacity, const u8 *payload,
			 size_t payload_length,
			 const struct mt6797_wifi_tx_params *params,
			 size_t *transfer_length)
{
	size_t packet_length;
	size_t aligned_length;
	u16 ether_type;
	u8 queue;

	if (!packet || !payload || !params || !transfer_length)
		return -EINVAL;
	if (payload_length < ETH_HLEN)
		return -EMSGSIZE;
	if (check_add_overflow(payload_length,
			       (size_t)MT6797_WIFI_TX_DESCRIPTOR_LENGTH,
			       &packet_length))
		return -EOVERFLOW;
	if (packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
		return -EMSGSIZE;
	aligned_length = ALIGN(packet_length, sizeof(u32));
	if (aligned_length > capacity)
		return -EMSGSIZE;

	ether_type = get_unaligned_be16(payload + 12);
	switch (params->traffic) {
	case MT6797_WIFI_TX_DATA:
		if (ether_type == ETH_P_PAE || ether_type == ETH_P_ARP ||
		    params->fixed_rate)
			return -EINVAL;
		queue = MT6797_WIFI_TX_QUEUE_DATA;
		break;
	case MT6797_WIFI_TX_EAPOL:
		if (ether_type != ETH_P_PAE)
			return -EINVAL;
		queue = MT6797_WIFI_TX_QUEUE_EAPOL;
		break;
	case MT6797_WIFI_TX_ARP:
		if (ether_type != ETH_P_ARP)
			return -EINVAL;
		queue = MT6797_WIFI_TX_QUEUE_DATA;
		break;
	default:
		return -EINVAL;
	}
	if (params->traffic != MT6797_WIFI_TX_DATA &&
	    (!params->fixed_rate ||
	     params->fixed_rate > MT6797_WIFI_TX_FIXED_RATE_MAX))
		return -EINVAL;

	memset(packet, 0, aligned_length);
	put_unaligned_le16(packet_length, packet);
	packet[2] = MT6797_WIFI_TX_ETHERTYPE_WORD_OFFSET;
	packet[3] = queue;
	packet[4] = params->wlan_index;
	packet[5] = MT6797_WIFI_TX_HEADER_ETHERNET_LONG;
	if (params->protected)
		packet[6] |= MT6797_WIFI_TX_PROTECTED;
	if (params->traffic == MT6797_WIFI_TX_ARP)
		packet[6] |= MT6797_WIFI_TX_ARP_PRIORITY;
	put_unaligned_le16(MT6797_WIFI_TX_RETRY_LIMIT, packet + 12);
	if (params->traffic != MT6797_WIFI_TX_DATA) {
		packet[11] = MT6797_WIFI_TX_FIXED_RATE;
		put_unaligned_le16(params->fixed_rate << 2, packet + 26);
	}
	memcpy(packet + MT6797_WIFI_TX_DESCRIPTOR_LENGTH, payload,
	       payload_length);
	*transfer_length = aligned_length;

	return 0;
}
