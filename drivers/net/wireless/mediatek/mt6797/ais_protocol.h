/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_PROTOCOL_H
#define __MT6797_WIFI_AIS_PROTOCOL_H

#include <linux/types.h>

#include "ais_resources.h"
#include "protocol.h"

#define MT6797_WIFI_AIS_BSS_COMMAND_LENGTH	20U
#define MT6797_WIFI_AIS_STA_UPDATE_LENGTH	132U
#define MT6797_WIFI_AIS_STA_REMOVE_LENGTH	12U
#define MT6797_WIFI_AIS_CHANNEL_COMMAND_LENGTH	24U
#define MT6797_WIFI_AIS_STA_EVENT_LENGTH		16U
#define MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH	24U
#define MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS	4000U

enum mt6797_wifi_ais_band {
	MT6797_WIFI_AIS_BAND_2GHZ = 1,
	MT6797_WIFI_AIS_BAND_5GHZ = 2,
};

enum mt6797_wifi_ais_event_disposition {
	MT6797_WIFI_AIS_EVENT_IGNORED,
	MT6797_WIFI_AIS_EVENT_COMPLETE,
	MT6797_WIFI_AIS_EVENT_TERMINAL,
};

struct mt6797_wifi_ais_channel {
	u8 band;
	u8 primary;
};

/*
 * These snapshots are constructed once, published by the single owner before
 * CMD53, and thereafter treated as immutable. A terminal result or timeout
 * fences the session: its sequence/token and resource handles must not be
 * reused, because the firmware may have observed an ambiguously delivered
 * command.
 */
struct mt6797_wifi_ais_sta_pending {
	u64 generation;
	u64 bss_cookie;
	u64 sta_cookie;
	u8 peer_addr[ETH_ALEN];
	u8 command_sequence;
	u8 bss_index;
	u8 sta_index;
};

struct mt6797_wifi_ais_channel_pending {
	u64 generation;
	u64 bss_cookie;
	u32 max_interval_ms;
	u8 token;
	u8 bss_index;
	u8 band;
	u8 primary;
};

struct mt6797_wifi_ais_sta_event {
	u8 peer_addr[ETH_ALEN];
	u8 bss_index;
	u8 sta_index;
};

struct mt6797_wifi_ais_channel_event {
	u32 grant_interval_ms;
	u8 token;
	u8 bss_index;
	u8 band;
	u8 primary;
};

/* The single owner must pass currently live handles from ais_resources. */
int mt6797_wifi_ais_bss_activate_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 command_sequence,
	u8 *command, size_t capacity);
int mt6797_wifi_ais_bss_deactivate_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 command_sequence,
	u8 *command, size_t capacity);
int mt6797_wifi_ais_sta_state1_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	u8 *command, size_t capacity);
int mt6797_wifi_ais_sta_remove_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	u8 *command, size_t capacity);
int mt6797_wifi_ais_channel_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_channel *channel, u8 token,
	u8 command_sequence, u8 *command, size_t capacity,
	struct mt6797_wifi_ais_channel_pending *pending);
int mt6797_wifi_ais_channel_abort_build(
	const struct mt6797_wifi_ais_bss_handle *bss, u8 token,
	u8 command_sequence, u8 *command, size_t capacity);

int mt6797_wifi_ais_sta_pending_init(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 command_sequence,
	struct mt6797_wifi_ais_sta_pending *pending);
int mt6797_wifi_ais_sta_event_parse(
	const struct mt6797_wifi_event_view *event,
	struct mt6797_wifi_ais_sta_event *sta_event);
int mt6797_wifi_ais_sta_event_match(
	const struct mt6797_wifi_ais_sta_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition);
int mt6797_wifi_ais_channel_event_parse(
	const struct mt6797_wifi_event_view *event,
	struct mt6797_wifi_ais_channel_event *channel_event);
int mt6797_wifi_ais_channel_event_match(
	const struct mt6797_wifi_ais_channel_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition);

#endif /* __MT6797_WIFI_AIS_PROTOCOL_H */
