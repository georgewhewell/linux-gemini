/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_GEN3_WIRE_H
#define __MT6797_WIFI_GEN3_WIRE_H

#include <linux/types.h>

#define MT6797_WIFI_GEN3_KEY_CMD_LEN 72U
#define MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN 12U
#define MT6797_WIFI_GEN3_BSS_INFO_CMD_LEN 96U

enum mt6797_wifi_gen3_cipher {
	MT6797_WIFI_GEN3_CIPHER_NONE = 0,
	MT6797_WIFI_GEN3_CIPHER_CCMP = 4,
	MT6797_WIFI_GEN3_CIPHER_BIP = 6,
};

int mt6797_wifi_gen3_build_key_cmd(u8 *buf, size_t buf_len, u8 sequence,
				   bool add, bool pairwise, const u8 *peer,
				   u8 key_index, u8 wlan_index,
				   enum mt6797_wifi_gen3_cipher cipher,
				   const u8 *key, size_t key_len,
				   const u8 *rsc, size_t rsc_len);

int mt6797_wifi_gen3_build_default_key_cmd(u8 *buf, size_t buf_len,
					   u8 sequence, u8 key_index,
					   bool unicast, bool multicast);

int mt6797_wifi_gen3_build_bss_info_cmd(u8 *buf, size_t buf_len, u8 sequence,
					bool connected, bool protected,
					const u8 *ssid, size_t ssid_len,
					const u8 *bssid, u8 channel);

#endif
