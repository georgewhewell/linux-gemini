/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_PROTOCOL_H
#define __MT6797_WIFI_PROTOCOL_H

#include <linux/types.h>

#define MT6797_WIFI_COMMAND_HEADER_LENGTH	8U
#define MT6797_WIFI_EVENT_HEADER_LENGTH		8U
#define MT6797_WIFI_BASIC_CONFIG_LENGTH		16U
#define MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH	2352U
#define MT6797_WIFI_RX_DESCRIPTOR_LENGTH		16U
#define MT6797_WIFI_TX_DESCRIPTOR_LENGTH		28U

enum mt6797_wifi_rx_kind {
	MT6797_WIFI_RX_EVENT,
	MT6797_WIFI_RX_IEEE80211,
	MT6797_WIFI_RX_ETHERNET,
};

enum mt6797_wifi_tx_traffic {
	MT6797_WIFI_TX_DATA,
	MT6797_WIFI_TX_EAPOL,
	MT6797_WIFI_TX_ARP,
};

struct mt6797_wifi_event_view {
	const u8 *payload;
	size_t payload_length;
	size_t packet_length;
	u8 id;
	u8 sequence;
};

struct mt6797_wifi_rx_view {
	/* Borrowed complete packet; valid only during synchronous delivery. */
	const u8 *packet;
	const u8 *payload;
	size_t payload_length;
	size_t packet_length;
	enum mt6797_wifi_rx_kind kind;
	u8 channel;
	u8 header_length;
	u8 wlan_index;
	u8 event_id;
	u8 sequence;
	bool header_translated;
};

struct mt6797_wifi_tx_params {
	enum mt6797_wifi_tx_traffic traffic;
	u16 fixed_rate;
	u8 wlan_index;
	u8 own_mac_index;
	bool protected;
};

int mt6797_wifi_command_init(u8 *command, size_t capacity,
			     size_t payload_length, u8 id, bool set,
			     u8 sequence);
int mt6797_wifi_basic_config_build(u8 *command, size_t capacity,
				   u8 sequence);
int mt6797_wifi_event_parse(const u8 *packet, size_t available,
			    struct mt6797_wifi_event_view *event);
int mt6797_wifi_rx_parse(const u8 *packet, size_t available,
			 struct mt6797_wifi_rx_view *frame);
int mt6797_wifi_tx_build(u8 *packet, size_t capacity, const u8 *payload,
			 size_t payload_length,
			 const struct mt6797_wifi_tx_params *params,
			 size_t *transfer_length);

#endif /* __MT6797_WIFI_PROTOCOL_H */
