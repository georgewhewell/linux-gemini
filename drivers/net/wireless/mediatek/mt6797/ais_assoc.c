// SPDX-License-Identifier: GPL-2.0-only
/* Association frames used by GEN3 mgmt/assoc.c; no firmware side effects. */
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_assoc.h"

static int mt6797_assoc_ies_validate(const u8 *ies, size_t length,
				    bool request)
{
	bool ssid = false, rates = false, extended_rates = false;
	size_t offset = 0;
	u8 id, size;

	if (!ies && length)
		return -EINVAL;
	while (offset < length) {
		if (length - offset < 2)
			return -EPROTO;
		id = ies[offset];
		size = ies[offset + 1];
		if (size > length - offset - 2)
			return -EPROTO;
		switch (id) {
		case WLAN_EID_SSID:
			if (ssid || size > IEEE80211_MAX_SSID_LEN)
				return -EPROTO;
			ssid = true;
			break;
		case WLAN_EID_SUPP_RATES:
			if (rates || !size || size > 8)
				return -EPROTO;
			rates = true;
			break;
		case WLAN_EID_EXT_SUPP_RATES:
			if (extended_rates || !size)
				return -EPROTO;
			extended_rates = true;
			break;
		}
		offset += size + 2;
	}
	return request && (!ssid || !rates) ? -EPROTO : 0;
}

int mt6797_wifi_ais_assoc_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_assoc_params *params,
	u8 *frame, size_t capacity, size_t *length,
	struct mt6797_wifi_ais_assoc_pending *pending)
{
	struct mt6797_wifi_ais_sta_pending owner;
	size_t total;
	int ret;

	if (pending)
		memset(pending, 0, sizeof(*pending));
	if (length)
		*length = 0;
	if (!params || !frame || !length || !pending ||
	    !params->listen_interval)
		return -EINVAL;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, 0, &owner);
	if (ret || ether_addr_equal(bss->own_addr, sta->peer_addr))
		return -EINVAL;
	if (params->ies_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH -
	    MT6797_WIFI_TX_DESCRIPTOR_LENGTH -
	    MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH)
		return -EMSGSIZE;
	total = MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH + params->ies_length;
	if (capacity < total)
		return -EMSGSIZE;
	ret = mt6797_assoc_ies_validate(params->ies, params->ies_length, true);
	if (ret)
		return ret;

	memset(frame, 0, MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH);
	put_unaligned_le16(IEEE80211_STYPE_ASSOC_REQ, frame);
	memcpy(frame + 4, sta->peer_addr, ETH_ALEN);
	memcpy(frame + 10, bss->own_addr, ETH_ALEN);
	memcpy(frame + 16, sta->peer_addr, ETH_ALEN);
	put_unaligned_le16(params->capability, frame + 24);
	put_unaligned_le16(params->listen_interval, frame + 26);
	memcpy(frame + MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH,
	       params->ies, params->ies_length);
	pending->generation = owner.generation;
	pending->bss_cookie = owner.bss_cookie;
	pending->sta_cookie = owner.sta_cookie;
	memcpy(pending->own_addr, bss->own_addr, ETH_ALEN);
	memcpy(pending->peer_addr, sta->peer_addr, ETH_ALEN);
	*length = total;
	return 0;
}

int mt6797_wifi_ais_assoc_response_match(
	const struct mt6797_wifi_ais_assoc_pending *pending,
	const u8 *frame, size_t length,
	enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_assoc_response *response)
{
	u16 fc, status, aid;
	int ret;

	if (!disposition || !response)
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	memset(response, 0, sizeof(*response));
	if (!pending || !frame || !pending->generation ||
	    !pending->bss_cookie || !pending->sta_cookie ||
	    !is_valid_ether_addr(pending->own_addr) ||
	    !is_valid_ether_addr(pending->peer_addr) ||
	    ether_addr_equal(pending->own_addr, pending->peer_addr))
		return -EINVAL;
	if (length < 24)
		return 0;
	fc = get_unaligned_le16(frame);
	if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
		   IEEE80211_FCTL_STYPE)) != IEEE80211_STYPE_ASSOC_RESP ||
	    !ether_addr_equal(frame + 4, pending->own_addr) ||
	    !ether_addr_equal(frame + 10, pending->peer_addr) ||
	    !ether_addr_equal(frame + 16, pending->peer_addr))
		return 0;
	if (length < MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH ||
	    length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
		  IEEE80211_FCTL_PROTECTED | IEEE80211_FCTL_MOREFRAGS |
		  IEEE80211_FCTL_ORDER) ||
	    (get_unaligned_le16(frame + 22) & IEEE80211_SCTL_FRAG))
		goto malformed;
	ret = mt6797_assoc_ies_validate(
		frame + MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH,
		length - MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH, false);
	if (ret)
		goto malformed;
	status = get_unaligned_le16(frame + 26);
	/* Vendor assoc.c masks the two reserved high bits of a successful AID. */
	aid = get_unaligned_le16(frame + 28) & 0x3fff;
	if (!status && (!aid || aid > IEEE80211_MAX_AID))
		goto malformed;
	response->capability = get_unaligned_le16(frame + 24);
	response->status_code = status;
	response->association_id = status ? 0 : aid;
	response->ies = frame + MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH;
	response->ies_length = length - MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH;
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;

malformed:
	*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
	return -EPROTO;
}
