/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_SCAN_TARGET_H
#define __MT6797_WIFI_SCAN_TARGET_H

#include <linux/if_ether.h>
#include <linux/ieee80211.h>
#include <linux/types.h>

#include "rx_metadata.h"

enum mt6797_wifi_scan_target_state {
	MT6797_WIFI_SCAN_TARGET_CLOSED,
	MT6797_WIFI_SCAN_TARGET_OPEN,
	MT6797_WIFI_SCAN_TARGET_TERMINAL,
};

enum mt6797_wifi_scan_target_update {
	MT6797_WIFI_SCAN_TARGET_IGNORED,
	MT6797_WIFI_SCAN_TARGET_ADDED,
	MT6797_WIFI_SCAN_TARGET_UPDATED,
	MT6797_WIFI_SCAN_TARGET_DUPLICATE,
};

struct mt6797_wifi_scan_target_key {
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_length;
};

/*
 * A snapshot owns every byte and contains no pointer into an RX packet. It is
 * valid only while its generation and cookie still match its single owner.
 * Security IEs are deliberately not retained or interpreted by this layer.
 */
struct mt6797_wifi_scan_target {
	u64 generation;
	u64 cookie;
	u64 observed_at_ms;
	s32 signal_mbm;
	u16 capability;
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_length;
	u8 band;
	u8 channel;
	u8 rcpi;
	u8 frame_type;
	bool last_frame_hidden;
};

/* Externally serialized by the future scan/AIS lifecycle owner. */
struct mt6797_wifi_scan_target_owner {
	struct mt6797_wifi_scan_target_key key;
	struct mt6797_wifi_scan_target selected;
	u64 generation;
	u64 next_cookie;
	u64 max_age_ms;
	u64 last_now_ms;
	u8 state;
	bool have_last_now;
	bool have_selected;
};

void mt6797_wifi_scan_target_init(
	struct mt6797_wifi_scan_target_owner *owner);
int mt6797_wifi_scan_target_open(
	struct mt6797_wifi_scan_target_owner *owner,
	const struct mt6797_wifi_scan_target_key *key, u64 max_age_ms);
int mt6797_wifi_scan_target_observe(
	struct mt6797_wifi_scan_target_owner *owner, const u8 *packet,
	size_t length, u64 now_ms, enum mt6797_wifi_scan_target_update *update);
int mt6797_wifi_scan_target_snapshot(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms,
	struct mt6797_wifi_scan_target *target);
int mt6797_wifi_scan_target_validate(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms,
	const struct mt6797_wifi_scan_target *target);
int mt6797_wifi_scan_target_expire(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms, bool *expired);
int mt6797_wifi_scan_target_close(
	struct mt6797_wifi_scan_target_owner *owner);

#endif /* __MT6797_WIFI_SCAN_TARGET_H */
