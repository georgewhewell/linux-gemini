/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_DATA_RX_H
#define __MT6797_WIFI_DATA_RX_H

#include <linux/types.h>

/* Borrowed input bytes; caller must copy before returning from the RX sink. */
struct mt6797_wifi_data_rx {
	u8 ethernet[14];
	const u8 *payload;
	size_t length; /* Ethernet header plus borrowed payload. */
	u64 pn;
	u8 transmitter[6];
	u8 bss_index;
	u8 wlan_index;
	u8 key_index;
	u8 tid; /* Non-QoS data shares the priority-zero counter. */
	bool protected;
	bool multicast;
	bool eapol;
};

/* Decode complete translated or native infrastructure data MPDUs. AP/WTBL/key
 * ownership and controlled-port policy are enforced by the connection owner.
 */
int mt6797_wifi_data_rx_decode(const u8 *packet, size_t available,
	struct mt6797_wifi_data_rx *data);
/* GEN3 group1 and cfg80211 CCMP sequence arrays are PN0..PN5, little endian. */
u64 mt6797_wifi_data_rx_pn(const u8 sequence[6]);
/* Caller serializes per-key/per-TID counters. Reject duplicate/older/zero PN. */
int mt6797_wifi_data_rx_replay(u64 *last, u64 pn);

#endif
