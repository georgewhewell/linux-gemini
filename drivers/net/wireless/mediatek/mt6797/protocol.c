// SPDX-License-Identifier: GPL-2.0-only
/* Bounded runtime command and event codecs for MT6797 GEN3 WLAN. */

#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"

#define MT6797_WIFI_COMMAND_PQ_ID	0x8000U
#define MT6797_WIFI_COMMAND_PACKET_TYPE	0xa0U
#define MT6797_WIFI_EVENT_PACKET_TYPE	0xe000U
#define MT6797_WIFI_COMMAND_BASIC_CONFIG	0x02U

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
	if (get_unaligned_le16(packet + 2) != MT6797_WIFI_EVENT_PACKET_TYPE ||
	    packet[6] || packet[7])
		return -EPROTO;

	event->payload = packet + MT6797_WIFI_EVENT_HEADER_LENGTH;
	event->payload_length = packet_length -
		MT6797_WIFI_EVENT_HEADER_LENGTH;
	event->packet_length = packet_length;
	event->id = packet[4];
	event->sequence = packet[5];

	return 0;
}
