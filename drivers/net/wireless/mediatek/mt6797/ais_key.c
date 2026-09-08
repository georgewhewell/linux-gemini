// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 CMD_802_11_KEY; byte layout from nic_cmd_event.h/wlanoidSetAddKey. */
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/string.h>

#include "ais_key.h"

int mt6797_wifi_ais_ccmp_key_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_key_params *params, u8 sequence,
	u8 *command, size_t capacity)
{
	struct mt6797_wifi_ais_sta_pending owner;
	u8 *body;
	int ret;

	if (!command || !params || params->index > 3 ||
	    (params->pairwise && params->index))
		return -EINVAL;
	if (params->remove) {
		if (params->material || params->material_length)
			return -EINVAL;
	} else if (!params->material || params->material_length != MT6797_WIFI_AIS_CCMP_KEY_LENGTH) {
		return -EINVAL;
	}
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, sequence, &owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_command_init(command, capacity,
		MT6797_WIFI_AIS_KEY_COMMAND_LENGTH - MT6797_WIFI_COMMAND_HEADER_LENGTH,
		0x07, true, sequence);
	if (ret)
		return ret;
	body = command + MT6797_WIFI_COMMAND_HEADER_LENGTH;
	body[0] = !params->remove;
	body[1] = !params->remove && params->pairwise;
	body[2] = params->pairwise;
	/* Infrastructure station, not authenticator; group RX keys also name the AP. */
	memcpy(body + 4, sta->peer_addr, ETH_ALEN);
	body[10] = bss->bss_index;
	body[11] = params->remove ? 0 : 4; /* CIPHER_SUITE_CCMP */
	body[12] = params->index;
	body[14] = params->pairwise ? sta->pairwise.index : sta->ap_bmc_group.index;
	if (!params->remove) {
		body[13] = MT6797_WIFI_AIS_CCMP_KEY_LENGTH;
		memcpy(body + 16, params->material, MT6797_WIFI_AIS_CCMP_KEY_LENGTH);
		memcpy(body + 48, params->rsc, MT6797_WIFI_AIS_KEY_RSC_LENGTH);
	}
	return 0;
}

int mt6797_wifi_ais_ccmp_ready_match(
	const struct mt6797_wifi_ais_sta_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition)
{
	if (!disposition)
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	if (!pending || !event || !pending->generation || !pending->bss_cookie ||
	    !pending->sta_cookie || !is_valid_ether_addr(pending->peer_addr))
		return -EINVAL;
	if (event->id != 0x24)
		return 0;
	if (!event->payload || event->payload_length != 8 || event->packet_length != 16) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return -EPROTO;
	}
	/* Vendor ignores the reserved byte and header sequence on this unsolicited event. */
	if (event->payload[0] != pending->bss_index ||
	    !ether_addr_equal(event->payload + 2, pending->peer_addr))
		return 0;
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;
}
