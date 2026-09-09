/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_MGMT_RX_H
#define __MT6797_WIFI_MGMT_RX_H

#include <linux/types.h>

/* Borrowed frame bytes. Decoder does not assert key ownership or advance PN. */
struct mt6797_wifi_mgmt_rx {
	const u8 *frame;
	size_t frame_length;
	u64 pn;
	u16 subtype;
	u8 bss_index;
	u8 wlan_index;
	u8 key_index;
	bool multicast;
};

/* Requires a complete, native robust management frame and supported cipher
 * metadata. BIP requires an intact MMIE; its 16-bit key ID is not the RXD's
 * truncated two-bit key field. Missing PN/MMIE information fails closed.
 */
int mt6797_wifi_mgmt_rx_decode(const u8 *packet, size_t available,
	struct mt6797_wifi_mgmt_rx *management);

/* Classify only plaintext unicast deauth/disassoc reasons 6/7. This is a
 * suspicion trigger, never integrity evidence or permission to disconnect.
 * GEN3's cipher-mismatch bit is allowed here; all other error bits reject.
 */
int mt6797_wifi_mgmt_rx_sa_query_trigger(const u8 *packet, size_t available,
	struct mt6797_wifi_mgmt_rx *management);

/* Verify BIP-CMAC-128 independently of firmware's integrity claim. The caller
 * first checks peer/key ownership, then advances IPN only after this succeeds.
 * Does not support an MMIE that firmware stripped or rewrote.
 */
int mt6797_wifi_mgmt_rx_bip_verify(const struct mt6797_wifi_mgmt_rx *management,
	const u8 key[16]);

#endif
