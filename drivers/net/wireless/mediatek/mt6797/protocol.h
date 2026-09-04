/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_PROTOCOL_H
#define __MT6797_WIFI_PROTOCOL_H

#include <linux/types.h>

#define MT6797_WIFI_COMMAND_HEADER_LENGTH	8U
#define MT6797_WIFI_EVENT_HEADER_LENGTH		8U
#define MT6797_WIFI_BASIC_CONFIG_LENGTH		16U
#define MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH	2352U

struct mt6797_wifi_event_view {
	const u8 *payload;
	size_t payload_length;
	size_t packet_length;
	u8 id;
	u8 sequence;
};

int mt6797_wifi_command_init(u8 *command, size_t capacity,
			     size_t payload_length, u8 id, bool set,
			     u8 sequence);
int mt6797_wifi_basic_config_build(u8 *command, size_t capacity,
				   u8 sequence);
int mt6797_wifi_event_parse(const u8 *packet, size_t available,
			    struct mt6797_wifi_event_view *event);

#endif /* __MT6797_WIFI_PROTOCOL_H */
