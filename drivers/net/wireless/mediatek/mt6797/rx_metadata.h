/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_RX_METADATA_H
#define __MT6797_WIFI_RX_METADATA_H

#include <linux/if_ether.h>
#include <linux/types.h>

enum mt6797_wifi_rx_band {
	MT6797_WIFI_RX_BAND_2GHZ = 1,
	MT6797_WIFI_RX_BAND_5GHZ = 2,
};

enum mt6797_wifi_bss_frame_type {
	MT6797_WIFI_BSS_BEACON,
	MT6797_WIFI_BSS_PROBE_RESPONSE,
};

/* All pointers are borrowed from the input packet and must not be retained. */
struct mt6797_wifi_rx_metadata {
	const u8 *frame;
	size_t frame_length;
	enum mt6797_wifi_rx_band band;
	s32 signal_mbm;
	u8 channel;
	u8 rcpi;
};

/* All pointers are borrowed from metadata->frame and must not be retained. */
struct mt6797_wifi_bss_view {
	const u8 *ies;
	size_t ies_length;
	const u8 *ssid;
	size_t ssid_length;
	enum mt6797_wifi_bss_frame_type frame_type;
	u8 bssid[ETH_ALEN];
	u16 capability;
	bool hidden_ssid;
};

int mt6797_wifi_rx_channel_decode(u8 encoded,
	enum mt6797_wifi_rx_band *band, u8 *channel);
int mt6797_wifi_rx_metadata_decode(const u8 *packet, size_t available,
				   struct mt6797_wifi_rx_metadata *metadata);
int mt6797_wifi_bss_parse(const struct mt6797_wifi_rx_metadata *metadata,
			  struct mt6797_wifi_bss_view *bss);

#endif /* __MT6797_WIFI_RX_METADATA_H */
