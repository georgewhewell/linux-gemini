/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_GEN3_WIRE_H
#define __MT6797_WIFI_GEN3_WIRE_H

#include <linux/types.h>

#define MT6797_WIFI_GEN3_KEY_CMD_LEN 72U
#define MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN 12U

enum mt6797_wifi_gen3_cipher {
	MT6797_WIFI_GEN3_CIPHER_NONE = 0,
	MT6797_WIFI_GEN3_CIPHER_CCMP = 4,
	MT6797_WIFI_GEN3_CIPHER_BIP = 6,
};

int mt6797_wifi_gen3_build_key_cmd(u8 *buf, size_t buf_len, u8 sequence,
				   bool add, bool pairwise, const u8 *peer,
				   u8 key_index, u8 wlan_index,
				   enum mt6797_wifi_gen3_cipher cipher,
				   const u8 *key, size_t key_len);

int mt6797_wifi_gen3_build_default_key_cmd(u8 *buf, size_t buf_len,
					   u8 sequence, u8 key_index,
					   bool unicast, bool multicast);

#endif
