// SPDX-License-Identifier: GPL-2.0-only
/* Pure open-system authentication codec for the MT6797 GEN3 AIS path. */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_auth.h"

#define MT6797_WIFI_AIS_AUTH_ALGORITHM_OPEN	0U
#define MT6797_WIFI_AIS_AUTH_TRANSACTION_REQUEST	1U
#define MT6797_WIFI_AIS_AUTH_TRANSACTION_RESPONSE	2U
#define MT6797_WIFI_AIS_AUTH_HEADER_LENGTH	24U

#define MT6797_WIFI_AIS_AUTH_DEST_OFFSET		4U
#define MT6797_WIFI_AIS_AUTH_SOURCE_OFFSET	10U
#define MT6797_WIFI_AIS_AUTH_BSSID_OFFSET	16U
#define MT6797_WIFI_AIS_AUTH_ALGORITHM_OFFSET	24U
#define MT6797_WIFI_AIS_AUTH_TRANSACTION_OFFSET	26U
#define MT6797_WIFI_AIS_AUTH_STATUS_OFFSET	28U

static bool mt6797_wifi_ais_auth_pending_valid(
	const struct mt6797_wifi_ais_auth_pending *pending)
{
	return pending && pending->generation && pending->bss_cookie &&
		pending->sta_cookie && is_valid_ether_addr(pending->own_addr) &&
		is_valid_ether_addr(pending->peer_addr) &&
		!ether_addr_equal(pending->own_addr, pending->peer_addr);
}

static int mt6797_wifi_ais_auth_ie_tail_validate(const u8 *frame,
						 size_t length)
{
	size_t offset = MT6797_WIFI_AIS_AUTH_FRAME_LENGTH;

	while (offset < length) {
		size_t next;

		if (length - offset < 2)
			return -EPROTO;
		if (check_add_overflow(offset, (size_t)frame[offset + 1] + 2,
				       &next) || next > length)
			return -EPROTO;
		offset = next;
	}
	return 0;
}

int mt6797_wifi_ais_open_auth_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 *frame,
	size_t capacity, struct mt6797_wifi_ais_auth_pending *pending)
{
	struct mt6797_wifi_ais_auth_pending candidate = {};
	struct mt6797_wifi_ais_sta_pending sta_pending;
	int ret;

	if (pending)
		memset(pending, 0, sizeof(*pending));
	if (!frame || !pending)
		return -EINVAL;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, 0, &sta_pending);
	if (ret || ether_addr_equal(bss->own_addr, sta->peer_addr))
		return -EINVAL;
	if (capacity < MT6797_WIFI_AIS_AUTH_FRAME_LENGTH)
		return -EMSGSIZE;

	memset(frame, 0, MT6797_WIFI_AIS_AUTH_FRAME_LENGTH);
	put_unaligned_le16(IEEE80211_STYPE_AUTH, frame);
	memcpy(frame + MT6797_WIFI_AIS_AUTH_DEST_OFFSET, sta->peer_addr,
	       ETH_ALEN);
	memcpy(frame + MT6797_WIFI_AIS_AUTH_SOURCE_OFFSET, bss->own_addr,
	       ETH_ALEN);
	memcpy(frame + MT6797_WIFI_AIS_AUTH_BSSID_OFFSET, sta->peer_addr,
	       ETH_ALEN);
	put_unaligned_le16(MT6797_WIFI_AIS_AUTH_ALGORITHM_OPEN,
			   frame + MT6797_WIFI_AIS_AUTH_ALGORITHM_OFFSET);
	put_unaligned_le16(MT6797_WIFI_AIS_AUTH_TRANSACTION_REQUEST,
			   frame + MT6797_WIFI_AIS_AUTH_TRANSACTION_OFFSET);

	candidate.generation = sta_pending.generation;
	candidate.bss_cookie = sta_pending.bss_cookie;
	candidate.sta_cookie = sta_pending.sta_cookie;
	memcpy(candidate.own_addr, bss->own_addr, ETH_ALEN);
	memcpy(candidate.peer_addr, sta->peer_addr, ETH_ALEN);
	*pending = candidate;
	return 0;
}

int mt6797_wifi_ais_open_auth_response_match(
	const struct mt6797_wifi_ais_auth_pending *pending, const u8 *frame,
	size_t length, enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_auth_response *response)
{
	u16 frame_control;
	int ret;

	if (!mt6797_wifi_ais_auth_pending_valid(pending) || !frame ||
	    !disposition || !response)
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	memset(response, 0, sizeof(*response));
	if (length < MT6797_WIFI_AIS_AUTH_HEADER_LENGTH)
		return 0;

	frame_control = get_unaligned_le16(frame);
	if ((frame_control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
			      IEEE80211_FCTL_STYPE)) != IEEE80211_STYPE_AUTH)
		return 0;
	if (!ether_addr_equal(frame + MT6797_WIFI_AIS_AUTH_DEST_OFFSET,
			      pending->own_addr) ||
	    !ether_addr_equal(frame + MT6797_WIFI_AIS_AUTH_SOURCE_OFFSET,
			      pending->peer_addr) ||
	    !ether_addr_equal(frame + MT6797_WIFI_AIS_AUTH_BSSID_OFFSET,
			      pending->peer_addr))
		return 0;

	if (length < MT6797_WIFI_AIS_AUTH_FRAME_LENGTH ||
	    length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    frame_control & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
			     IEEE80211_FCTL_PROTECTED) ||
	    get_unaligned_le16(frame + MT6797_WIFI_AIS_AUTH_ALGORITHM_OFFSET) !=
		MT6797_WIFI_AIS_AUTH_ALGORITHM_OPEN ||
	    get_unaligned_le16(frame + MT6797_WIFI_AIS_AUTH_TRANSACTION_OFFSET) !=
		MT6797_WIFI_AIS_AUTH_TRANSACTION_RESPONSE) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return -EPROTO;
	}
	ret = mt6797_wifi_ais_auth_ie_tail_validate(frame, length);
	if (ret) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return ret;
	}

	response->status_code =
		get_unaligned_le16(frame + MT6797_WIFI_AIS_AUTH_STATUS_OFFSET);
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;
}
