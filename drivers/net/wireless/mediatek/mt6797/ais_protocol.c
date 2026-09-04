// SPDX-License-Identifier: GPL-2.0-only
/* Pure codecs for the minimal MT6797 GEN3 infrastructure AIS firmware ABI. */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_protocol.h"

#define MT6797_WIFI_AIS_CMD_BSS_ACTIVATE	0x11U
#define MT6797_WIFI_AIS_CMD_STA_UPDATE		0x13U
#define MT6797_WIFI_AIS_CMD_STA_REMOVE		0x14U
#define MT6797_WIFI_AIS_CMD_CHANNEL		0x1cU
#define MT6797_WIFI_AIS_EVENT_STA_ACTIVE	0x0cU
#define MT6797_WIFI_AIS_EVENT_CHANNEL		0x10U

#define MT6797_WIFI_AIS_NETWORK_TYPE		0U
#define MT6797_WIFI_AIS_STA_TYPE_LEGACY_AP	0x41U
#define MT6797_WIFI_AIS_STA_STATE1		0U
#define MT6797_WIFI_AIS_STA_REMOVE_ONE		0U
#define MT6797_WIFI_AIS_CHANNEL_REQUEST		0U
#define MT6797_WIFI_AIS_CHANNEL_ABORT		1U
#define MT6797_WIFI_AIS_CHANNEL_GRANT		0U
#define MT6797_WIFI_AIS_CHANNEL_SCO_NONE	0U
#define MT6797_WIFI_AIS_CHANNEL_WIDTH_20_40	0U
#define MT6797_WIFI_AIS_CHANNEL_REQUEST_JOIN	0U
#define MT6797_WIFI_AIS_JOIN_GRANT_GUARD_MS	10U

#define MT6797_WIFI_AIS_BODY_OFFSET		8U
#define MT6797_WIFI_AIS_BSS_ACTIVE_OFFSET	9U
#define MT6797_WIFI_AIS_BSS_NETWORK_OFFSET	10U
#define MT6797_WIFI_AIS_BSS_OWN_MAC_OFFSET	11U
#define MT6797_WIFI_AIS_BSS_ADDR_OFFSET		12U
#define MT6797_WIFI_AIS_BSS_BMC_OFFSET		18U

#define MT6797_WIFI_AIS_STA_TYPE_OFFSET		9U
#define MT6797_WIFI_AIS_STA_ADDR_OFFSET		10U
#define MT6797_WIFI_AIS_STA_BSS_OFFSET		20U
#define MT6797_WIFI_AIS_STA_STATE_OFFSET	28U
#define MT6797_WIFI_AIS_STA_NEED_RESP_OFFSET	59U
#define MT6797_WIFI_AIS_STA_PAIRWISE_OFFSET	62U
#define MT6797_WIFI_AIS_STA_BMC_OFFSET		63U

#define MT6797_WIFI_AIS_CHANNEL_TOKEN_OFFSET	9U
#define MT6797_WIFI_AIS_CHANNEL_ACTION_OFFSET	10U
#define MT6797_WIFI_AIS_CHANNEL_PRIMARY_OFFSET	11U
#define MT6797_WIFI_AIS_CHANNEL_SCO_OFFSET	12U
#define MT6797_WIFI_AIS_CHANNEL_BAND_OFFSET	13U
#define MT6797_WIFI_AIS_CHANNEL_WIDTH_OFFSET	14U
#define MT6797_WIFI_AIS_CHANNEL_CENTER1_OFFSET	15U
#define MT6797_WIFI_AIS_CHANNEL_CENTER2_OFFSET	16U
#define MT6797_WIFI_AIS_CHANNEL_TYPE_OFFSET	17U
#define MT6797_WIFI_AIS_CHANNEL_INTERVAL_OFFSET	20U

static bool mt6797_wifi_ais_wtbl_valid(
	const struct mt6797_wifi_ais_wtbl_handle *wtbl,
	enum mt6797_wifi_ais_wtbl_role role, u64 generation, u64 owner_cookie)
{
	return wtbl->generation == generation && wtbl->cookie &&
		wtbl->owner_cookie == owner_cookie && wtbl->role == role &&
		wtbl->index < MT6797_WIFI_AIS_WTBL_COUNT;
}

static bool mt6797_wifi_ais_bss_valid(
	const struct mt6797_wifi_ais_bss_handle *bss)
{
	return bss && bss->generation && bss->cookie &&
		is_valid_ether_addr(bss->own_addr) &&
		mt6797_wifi_ais_wtbl_valid(&bss->bmc,
			MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC, bss->generation,
			bss->cookie);
}

static bool mt6797_wifi_ais_sta_valid(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta)
{
	if (!mt6797_wifi_ais_bss_valid(bss) || !sta || !sta->cookie ||
	    sta->generation != bss->generation ||
	    sta->bss_cookie != bss->cookie || sta->bss_index != bss->bss_index ||
	    sta->sta_index >= MT6797_WIFI_AIS_STA_COUNT ||
	    !is_valid_ether_addr(sta->peer_addr))
		return false;
	if (!mt6797_wifi_ais_wtbl_valid(&sta->pairwise,
		    MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE, sta->generation,
		    sta->cookie) ||
	    !mt6797_wifi_ais_wtbl_valid(&sta->ap_bmc_group,
		    MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP,
		    sta->generation, sta->cookie))
		return false;
	return bss->bmc.index != sta->pairwise.index &&
		bss->bmc.index != sta->ap_bmc_group.index &&
		sta->pairwise.index != sta->ap_bmc_group.index;
}

static bool mt6797_wifi_ais_channel_valid(u8 band, u8 primary)
{
	if (band == MT6797_WIFI_AIS_BAND_2GHZ)
		return primary >= 1 && primary <= 14;
	if (band == MT6797_WIFI_AIS_BAND_5GHZ)
		return primary != 0;
	return false;
}

static int mt6797_wifi_ais_command_init(u8 *command, size_t capacity,
				       size_t length, u8 id, u8 sequence)
{
	return mt6797_wifi_command_init(command, capacity,
			length - MT6797_WIFI_COMMAND_HEADER_LENGTH, id, true,
			sequence);
}

int mt6797_wifi_ais_bss_activate_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 command_sequence,
	u8 *command, size_t capacity)
{
	int ret;

	if (!command || !mt6797_wifi_ais_bss_valid(bss))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_BSS_COMMAND_LENGTH,
			MT6797_WIFI_AIS_CMD_BSS_ACTIVATE, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] = bss->bss_index;
	command[MT6797_WIFI_AIS_BSS_ACTIVE_OFFSET] = 1;
	command[MT6797_WIFI_AIS_BSS_NETWORK_OFFSET] =
		MT6797_WIFI_AIS_NETWORK_TYPE;
	command[MT6797_WIFI_AIS_BSS_OWN_MAC_OFFSET] = bss->own_mac_index;
	memcpy(command + MT6797_WIFI_AIS_BSS_ADDR_OFFSET, bss->own_addr,
	       ETH_ALEN);
	command[MT6797_WIFI_AIS_BSS_BMC_OFFSET] = bss->bmc.index;
	return 0;
}

int mt6797_wifi_ais_bss_deactivate_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 command_sequence,
	u8 *command, size_t capacity)
{
	int ret;

	if (!command || !mt6797_wifi_ais_bss_valid(bss))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_BSS_COMMAND_LENGTH,
			MT6797_WIFI_AIS_CMD_BSS_ACTIVATE, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] = bss->bss_index;
	command[MT6797_WIFI_AIS_BSS_OWN_MAC_OFFSET] = bss->own_mac_index;
	return 0;
}

int mt6797_wifi_ais_sta_state1_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	u8 *command, size_t capacity)
{
	int ret;

	if (!command || !mt6797_wifi_ais_sta_valid(bss, sta))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_STA_UPDATE_LENGTH,
			MT6797_WIFI_AIS_CMD_STA_UPDATE, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] = sta->sta_index;
	command[MT6797_WIFI_AIS_STA_TYPE_OFFSET] =
		MT6797_WIFI_AIS_STA_TYPE_LEGACY_AP;
	memcpy(command + MT6797_WIFI_AIS_STA_ADDR_OFFSET, sta->peer_addr,
	       ETH_ALEN);
	command[MT6797_WIFI_AIS_STA_BSS_OFFSET] = sta->bss_index;
	command[MT6797_WIFI_AIS_STA_STATE_OFFSET] =
		MT6797_WIFI_AIS_STA_STATE1;
	command[MT6797_WIFI_AIS_STA_NEED_RESP_OFFSET] = 0;
	command[MT6797_WIFI_AIS_STA_PAIRWISE_OFFSET] = sta->pairwise.index;
	command[MT6797_WIFI_AIS_STA_BMC_OFFSET] = sta->ap_bmc_group.index;
	return 0;
}

int mt6797_wifi_ais_sta_remove_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	u8 *command, size_t capacity)
{
	int ret;

	if (!command || !mt6797_wifi_ais_sta_valid(bss, sta))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_STA_REMOVE_LENGTH,
			MT6797_WIFI_AIS_CMD_STA_REMOVE, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] =
		MT6797_WIFI_AIS_STA_REMOVE_ONE;
	command[MT6797_WIFI_AIS_BODY_OFFSET + 1] = sta->sta_index;
	command[MT6797_WIFI_AIS_BODY_OFFSET + 2] = sta->bss_index;
	return 0;
}

int mt6797_wifi_ais_channel_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_channel *channel, u8 token,
	u8 command_sequence, u8 *command, size_t capacity,
	struct mt6797_wifi_ais_channel_pending *pending)
{
	struct mt6797_wifi_ais_channel_pending candidate = {};
	int ret;

	if (pending)
		memset(pending, 0, sizeof(*pending));
	if (!command || !pending || !mt6797_wifi_ais_bss_valid(bss) ||
	    !channel || !mt6797_wifi_ais_channel_valid(channel->band,
						       channel->primary))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH,
			MT6797_WIFI_AIS_CMD_CHANNEL, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] = bss->bss_index;
	command[MT6797_WIFI_AIS_CHANNEL_TOKEN_OFFSET] = token;
	command[MT6797_WIFI_AIS_CHANNEL_ACTION_OFFSET] =
		MT6797_WIFI_AIS_CHANNEL_REQUEST;
	command[MT6797_WIFI_AIS_CHANNEL_PRIMARY_OFFSET] = channel->primary;
	command[MT6797_WIFI_AIS_CHANNEL_SCO_OFFSET] =
		MT6797_WIFI_AIS_CHANNEL_SCO_NONE;
	command[MT6797_WIFI_AIS_CHANNEL_BAND_OFFSET] = channel->band;
	command[MT6797_WIFI_AIS_CHANNEL_WIDTH_OFFSET] =
		MT6797_WIFI_AIS_CHANNEL_WIDTH_20_40;
	command[MT6797_WIFI_AIS_CHANNEL_TYPE_OFFSET] =
		MT6797_WIFI_AIS_CHANNEL_REQUEST_JOIN;
	put_unaligned_le32(MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS,
			   command + MT6797_WIFI_AIS_CHANNEL_INTERVAL_OFFSET);

	candidate.generation = bss->generation;
	candidate.bss_cookie = bss->cookie;
	candidate.max_interval_ms = MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS;
	candidate.token = token;
	candidate.bss_index = bss->bss_index;
	candidate.band = channel->band;
	candidate.primary = channel->primary;
	*pending = candidate;
	return 0;
}

int mt6797_wifi_ais_channel_abort_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 token,
	u8 command_sequence, u8 *command, size_t capacity)
{
	int ret;

	if (!command || !mt6797_wifi_ais_bss_valid(bss))
		return -EINVAL;
	ret = mt6797_wifi_ais_command_init(command, capacity,
			MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH,
			MT6797_WIFI_AIS_CMD_CHANNEL, command_sequence);
	if (ret)
		return ret;
	command[MT6797_WIFI_AIS_BODY_OFFSET] = bss->bss_index;
	command[MT6797_WIFI_AIS_CHANNEL_TOKEN_OFFSET] = token;
	command[MT6797_WIFI_AIS_CHANNEL_ACTION_OFFSET] =
		MT6797_WIFI_AIS_CHANNEL_ABORT;
	return 0;
}

int mt6797_wifi_ais_sta_pending_init(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	struct mt6797_wifi_ais_sta_pending *pending)
{
	if (pending)
		memset(pending, 0, sizeof(*pending));
	if (!pending || !mt6797_wifi_ais_sta_valid(bss, sta))
		return -EINVAL;
	*pending = (struct mt6797_wifi_ais_sta_pending) {
		.generation = sta->generation,
		.bss_cookie = sta->bss_cookie,
		.sta_cookie = sta->cookie,
		.command_sequence = command_sequence,
		.bss_index = sta->bss_index,
		.sta_index = sta->sta_index,
	};
	memcpy(pending->peer_addr, sta->peer_addr, ETH_ALEN);
	return 0;
}

int mt6797_wifi_ais_sta_event_parse(
	const struct mt6797_wifi_event_view *event,
	struct mt6797_wifi_ais_sta_event *sta_event)
{
	if (!event || !sta_event)
		return -EINVAL;
	memset(sta_event, 0, sizeof(*sta_event));
	if (event->id != MT6797_WIFI_AIS_EVENT_STA_ACTIVE || !event->payload ||
	    event->payload_length != MT6797_WIFI_AIS_STA_EVENT_LENGTH -
				     MT6797_WIFI_EVENT_HEADER_LENGTH ||
	    event->packet_length != MT6797_WIFI_AIS_STA_EVENT_LENGTH)
		return -EPROTO;
	memcpy(sta_event->peer_addr, event->payload, ETH_ALEN);
	sta_event->sta_index = event->payload[6];
	sta_event->bss_index = event->payload[7];
	return 0;
}

int mt6797_wifi_ais_sta_event_match(
	const struct mt6797_wifi_ais_sta_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition)
{
	struct mt6797_wifi_ais_sta_event sta_event;
	int ret;

	if (!pending || !event || !disposition || !pending->generation ||
	    !pending->bss_cookie || !pending->sta_cookie ||
	    !is_valid_ether_addr(pending->peer_addr) ||
	    pending->sta_index >= MT6797_WIFI_AIS_STA_COUNT)
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	if (event->sequence != pending->command_sequence)
		return 0;
	if (event->id != MT6797_WIFI_AIS_EVENT_STA_ACTIVE) {
		*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
		return -EPROTO;
	}
	ret = mt6797_wifi_ais_sta_event_parse(event, &sta_event);
	if (ret)
		goto terminal;
	if (sta_event.bss_index != pending->bss_index ||
	    sta_event.sta_index != pending->sta_index ||
	    !ether_addr_equal(sta_event.peer_addr, pending->peer_addr)) {
		ret = -EPROTO;
		goto terminal;
	}
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;

terminal:
	*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
	return ret;
}

int mt6797_wifi_ais_channel_event_parse(
	const struct mt6797_wifi_event_view *event,
	struct mt6797_wifi_ais_channel_event *channel_event)
{
	const u8 *payload;

	if (!event || !channel_event)
		return -EINVAL;
	memset(channel_event, 0, sizeof(*channel_event));
	if (event->id != MT6797_WIFI_AIS_EVENT_CHANNEL || !event->payload ||
	    event->payload_length != MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH -
				     MT6797_WIFI_EVENT_HEADER_LENGTH ||
	    event->packet_length != MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH)
		return -EPROTO;
	payload = event->payload;
	if (payload[2] != MT6797_WIFI_AIS_CHANNEL_GRANT ||
	    payload[4] != MT6797_WIFI_AIS_CHANNEL_SCO_NONE ||
	    payload[6] != MT6797_WIFI_AIS_CHANNEL_WIDTH_20_40 || payload[7] ||
	    payload[8] || payload[9] != MT6797_WIFI_AIS_CHANNEL_REQUEST_JOIN ||
	    payload[10] || payload[11])
		return -EPROTO;
	channel_event->bss_index = payload[0];
	channel_event->token = payload[1];
	channel_event->primary = payload[3];
	channel_event->band = payload[5];
	channel_event->grant_interval_ms = get_unaligned_le32(payload + 12);
	if (!mt6797_wifi_ais_channel_valid(channel_event->band,
					    channel_event->primary) ||
	    channel_event->grant_interval_ms <=
			MT6797_WIFI_AIS_JOIN_GRANT_GUARD_MS)
		return -EPROTO;
	return 0;
}

int mt6797_wifi_ais_channel_event_match(
	const struct mt6797_wifi_ais_channel_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition)
{
	struct mt6797_wifi_ais_channel_event channel_event;
	int ret;

	if (!pending || !event || !disposition || !pending->generation ||
	    !pending->bss_cookie ||
	    pending->max_interval_ms != MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS ||
	    !mt6797_wifi_ais_channel_valid(pending->band, pending->primary))
		return -EINVAL;
	*disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
	if (event->id != MT6797_WIFI_AIS_EVENT_CHANNEL)
		return 0;
	if (!event->payload || event->payload_length < 2)
		goto terminal_protocol;
	if (event->payload[0] != pending->bss_index ||
	    event->payload[1] != pending->token)
		return 0;
	ret = mt6797_wifi_ais_channel_event_parse(event, &channel_event);
	if (ret)
		goto terminal;
	if (channel_event.band != pending->band ||
	    channel_event.primary != pending->primary ||
	    channel_event.grant_interval_ms > pending->max_interval_ms)
		goto terminal_protocol;
	*disposition = MT6797_WIFI_AIS_EVENT_COMPLETE;
	return 0;

terminal_protocol:
	ret = -EPROTO;
terminal:
	*disposition = MT6797_WIFI_AIS_EVENT_TERMINAL;
	return ret;
}
