// SPDX-License-Identifier: GPL-2.0-only
/* Byte-exact host/firmware command encoding for the MT6797 GEN3 WLAN ABI. */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mt6797-wifi-gen3-wire.h"

#define MT6797_WIFI_GEN3_CMD_PQ_ID 0x8000U
#define MT6797_WIFI_GEN3_CMD_PACKET_TYPE 0x20U
#define MT6797_WIFI_GEN3_CMD_ADD_REMOVE_KEY 0x07U
#define MT6797_WIFI_GEN3_CMD_DEFAULT_KEY 0x08U
#define MT6797_WIFI_GEN3_KEY_LEN 16U
#define MT6797_WIFI_GEN3_MAX_RSC_LEN 16U
#define MT6797_WIFI_GEN3_MAX_KEY_INDEX 5U

static void mt6797_wifi_gen3_init_cmd(u8 *buf, size_t len, u8 command,
				      u8 sequence)
{
	put_unaligned_le16(len, buf);
	put_unaligned_le16(MT6797_WIFI_GEN3_CMD_PQ_ID, buf + 2);
	buf[4] = command;
	buf[5] = MT6797_WIFI_GEN3_CMD_PACKET_TYPE;
	buf[6] = 1;
	buf[7] = sequence;
}

static bool mt6797_wifi_gen3_key_valid(bool pairwise, u8 key_index,
				       enum mt6797_wifi_gen3_cipher cipher)
{
	if (key_index > MT6797_WIFI_GEN3_MAX_KEY_INDEX)
		return false;

	if (pairwise)
		return key_index == 0 && cipher == MT6797_WIFI_GEN3_CIPHER_CCMP;

	if (key_index >= 4)
		return cipher == MT6797_WIFI_GEN3_CIPHER_BIP;

	return cipher == MT6797_WIFI_GEN3_CIPHER_CCMP;
}

int mt6797_wifi_gen3_build_key_cmd(u8 *buf, size_t buf_len, u8 sequence,
				   bool add, bool pairwise, const u8 *peer,
				   u8 key_index, u8 wlan_index,
				   enum mt6797_wifi_gen3_cipher cipher,
				   const u8 *key, size_t key_len,
				   const u8 *rsc, size_t rsc_len)
{
	if (!buf || !peer || buf_len < MT6797_WIFI_GEN3_KEY_CMD_LEN)
		return -EINVAL;
	if (add && (!key || key_len != MT6797_WIFI_GEN3_KEY_LEN ||
		    !mt6797_wifi_gen3_key_valid(pairwise, key_index, cipher) ||
		    rsc_len > MT6797_WIFI_GEN3_MAX_RSC_LEN ||
		    (rsc_len && !rsc)))
		return -EINVAL;
	if (!add && (key || key_len || rsc || rsc_len ||
		     key_index > MT6797_WIFI_GEN3_MAX_KEY_INDEX))
		return -EINVAL;

	memset(buf, 0, MT6797_WIFI_GEN3_KEY_CMD_LEN);
	mt6797_wifi_gen3_init_cmd(buf, MT6797_WIFI_GEN3_KEY_CMD_LEN,
				  MT6797_WIFI_GEN3_CMD_ADD_REMOVE_KEY, sequence);
	buf[8] = add;
	buf[9] = add && pairwise;
	buf[10] = pairwise;
	memcpy(buf + 12, peer, ETH_ALEN);
	buf[18] = 0; /* AIS BSS index */
	buf[19] = add ? cipher : MT6797_WIFI_GEN3_CIPHER_NONE;
	buf[20] = key_index;
	buf[21] = add ? key_len : 0;
	buf[22] = wlan_index;
	if (add)
		memcpy(buf + 24, key, key_len);
	if (rsc_len)
		memcpy(buf + 56, rsc, rsc_len);

	return MT6797_WIFI_GEN3_KEY_CMD_LEN;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_gen3_build_key_cmd);

int mt6797_wifi_gen3_build_default_key_cmd(u8 *buf, size_t buf_len,
					   u8 sequence, u8 key_index,
					   bool unicast, bool multicast)
{
	if (!buf || buf_len < MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN ||
	    key_index > 3 || (!unicast && !multicast))
		return -EINVAL;

	memset(buf, 0, MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN);
	mt6797_wifi_gen3_init_cmd(buf, MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN,
				  MT6797_WIFI_GEN3_CMD_DEFAULT_KEY, sequence);
	buf[8] = 0; /* AIS BSS index */
	buf[9] = key_index;
	buf[10] = unicast;
	buf[11] = multicast;

	return MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_gen3_build_default_key_cmd);
