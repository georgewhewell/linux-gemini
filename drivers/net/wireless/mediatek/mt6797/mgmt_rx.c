// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 nic_rx.h metadata and mac80211 wpa.c/aes_cmac.c BIP validation rules. */
#include <crypto/aes-cbc-macs.h>
#include <crypto/utils.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "data_rx.h"
#include "mgmt_rx.h"
#include "protocol.h"

#define MT6797_MGMT_HEADER_LENGTH 24U
#define MT6797_MGMT_MMIE_LENGTH 18U
#define MT6797_MGMT_BIP_MIC_LENGTH 8U

static bool mt6797_wifi_mgmt_action_robust(u8 category)
{
	/* Same exclusions as _ieee80211_is_robust_mgmt_frame(). */
	return category != WLAN_CATEGORY_PUBLIC && category != WLAN_CATEGORY_HT &&
		category != WLAN_CATEGORY_WNM_UNPROTECTED &&
		category != WLAN_CATEGORY_SELF_PROTECTED && category != WLAN_CATEGORY_UNPROT_DMG &&
		category != WLAN_CATEGORY_VHT && category != WLAN_CATEGORY_S1G &&
		category != WLAN_CATEGORY_VENDOR_SPECIFIC;
}

int mt6797_wifi_mgmt_rx_decode(const u8 *packet, size_t available,
	struct mt6797_wifi_mgmt_rx *management)
{
	struct mt6797_wifi_rx_view view;
	struct mt6797_wifi_mgmt_rx candidate = {};
	const u8 *mmie, *pn;
	u16 control, status;
	u8 cipher, groups;
	size_t body_length;
	int ret;

	if (!management)
		return -EINVAL;
	*management = (struct mt6797_wifi_mgmt_rx) {};
	ret = mt6797_wifi_rx_parse(packet, available, &view);
	if (ret)
		return ret;
	if (view.kind != MT6797_WIFI_RX_IEEE80211 || view.header_translated ||
	    view.header_length != MT6797_MGMT_HEADER_LENGTH ||
	    view.payload_length < MT6797_MGMT_HEADER_LENGTH + 2 || (packet[7] & 3))
		return -EOPNOTSUPP;
	status = get_unaligned_le16(packet + 10);
	/* GEN3 bits1..12 are error/mismatch/fragment/null indications. Bit0 is
	 * a software flag; bits13..15 describe data/AMPDU format, not integrity.
	 */
	if (status & 0x1ffe)
		return -EBADMSG;
	control = get_unaligned_le16(view.payload);
	if (control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
		       IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
		       IEEE80211_FCTL_MOREFRAGS | IEEE80211_FCTL_ORDER) ||
	    (get_unaligned_le16(view.payload + 22) & IEEE80211_SCTL_FRAG))
		return -EPROTO;
	candidate.subtype = control & IEEE80211_FCTL_STYPE;
	if (candidate.subtype != IEEE80211_STYPE_DEAUTH &&
	    candidate.subtype != IEEE80211_STYPE_DISASSOC &&
	    candidate.subtype != IEEE80211_STYPE_ACTION)
		return -EOPNOTSUPP;
	if (!is_valid_ether_addr(view.payload + 10) || !is_valid_ether_addr(view.payload + 16))
		return -EPROTO;
	candidate.multicast = is_multicast_ether_addr(view.payload + 4);
	cipher = packet[9] >> 4;
	groups = (get_unaligned_le16(packet + 2) >> 9) & 15;
	body_length = view.payload_length - MT6797_MGMT_HEADER_LENGTH;
	if (candidate.multicast) {
		/* BIP authenticates group frames without setting the Protected bit. */
		if (cipher != 6 || (control & IEEE80211_FCTL_PROTECTED) ||
		    body_length < 2 + MT6797_MGMT_MMIE_LENGTH)
			return -EBADMSG;
		mmie = view.payload + view.payload_length - MT6797_MGMT_MMIE_LENGTH;
		if (mmie[0] != WLAN_EID_MMIE || mmie[1] != MT6797_MGMT_MMIE_LENGTH - 2 ||
		    (get_unaligned_le16(mmie + 2) != 4 && get_unaligned_le16(mmie + 2) != 5))
			return -EPROTO;
		candidate.key_index = get_unaligned_le16(mmie + 2);
		candidate.pn = mt6797_wifi_data_rx_pn(mmie + 4);
		body_length -= MT6797_MGMT_MMIE_LENGTH;
	} else {
		if (cipher != 4 || !(control & IEEE80211_FCTL_PROTECTED) || !(groups & BIT(0)))
			return -EBADMSG;
		/* Group4 is first when present. rx_parse validated every group bound.
		 * GEN3 exposes decrypted management body, as used by auth/assoc.c.
		 * Group1 carries the CCMP PN; do not reconstruct it from sequence control.
		 */
		pn = packet + MT6797_WIFI_RX_DESCRIPTOR_LENGTH + ((groups & BIT(3)) ? 16 : 0);
		candidate.pn = mt6797_wifi_data_rx_pn(pn);
		candidate.key_index = packet[4] >> 6;
	}
	if (candidate.subtype == IEEE80211_STYPE_ACTION &&
	    (body_length < 2 || !mt6797_wifi_mgmt_action_robust(view.payload[24])))
		return -EOPNOTSUPP;
	candidate.frame = view.payload;
	candidate.frame_length = view.payload_length;
	candidate.bss_index = packet[7] >> 2;
	candidate.wlan_index = view.wlan_index;
	*management = candidate;
	return 0;
}

int mt6797_wifi_mgmt_rx_bip_verify(const struct mt6797_wifi_mgmt_rx *management,
	const u8 key[16])
{
	struct aes_cmac_key prepared;
	struct aes_cmac_ctx ctx;
	u8 aad[20], mac[AES_BLOCK_SIZE];
	static const u8 zero[MT6797_MGMT_BIP_MIC_LENGTH];
	u16 control;
	int ret;

	if (!management || !key || !management->frame || !management->multicast ||
	    management->frame_length < MT6797_MGMT_HEADER_LENGTH + 2 + MT6797_MGMT_MMIE_LENGTH ||
	    management->frame_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
		return -EINVAL;
	control = get_unaligned_le16(management->frame);
	control &= ~(IEEE80211_FCTL_RETRY | IEEE80211_FCTL_PM | IEEE80211_FCTL_MOREDATA);
	put_unaligned_le16(control, aad);
	memcpy(aad + 2, management->frame + 4, 18); /* A1 || A2 || A3 */
	aes_cmac_preparekey(&prepared, key, AES_KEYSIZE_128);
	aes_cmac_init(&ctx, &prepared);
	aes_cmac_update(&ctx, aad, sizeof(aad));
	aes_cmac_update(&ctx, management->frame + MT6797_MGMT_HEADER_LENGTH,
		management->frame_length - MT6797_MGMT_HEADER_LENGTH - MT6797_MGMT_BIP_MIC_LENGTH);
	aes_cmac_update(&ctx, zero, sizeof(zero));
	aes_cmac_final(&ctx, mac);
	ret = crypto_memneq(mac, management->frame + management->frame_length -
		MT6797_MGMT_BIP_MIC_LENGTH, MT6797_MGMT_BIP_MIC_LENGTH) ? -EBADMSG : 0;
	memzero_explicit(&prepared, sizeof(prepared));
	memzero_explicit(&ctx, sizeof(ctx));
	memzero_explicit(mac, sizeof(mac));
	return ret;
}
