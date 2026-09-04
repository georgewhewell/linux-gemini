// SPDX-License-Identifier: GPL-2.0-only
/* Owned, explicitly keyed MT6797 GEN3 scan result selection. */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "scan_target.h"

static int mt6797_wifi_scan_target_require_open(
	const struct mt6797_wifi_scan_target_owner *owner)
{
	if (!owner)
		return -EINVAL;
	if (owner->state == MT6797_WIFI_SCAN_TARGET_TERMINAL)
		return -ESHUTDOWN;
	return owner->state == MT6797_WIFI_SCAN_TARGET_OPEN ? 0 : -ENOTCONN;
}

static int mt6797_wifi_scan_target_terminal(
	struct mt6797_wifi_scan_target_owner *owner, int reason)
{
	owner->state = MT6797_WIFI_SCAN_TARGET_TERMINAL;
	owner->have_selected = false;
	memset(&owner->selected, 0, sizeof(owner->selected));
	return reason;
}

static int mt6797_wifi_scan_target_advance(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms)
{
	if (owner->have_last_now && now_ms < owner->last_now_ms)
		return mt6797_wifi_scan_target_terminal(owner, -ERANGE);
	owner->last_now_ms = now_ms;
	owner->have_last_now = true;
	return 0;
}

static bool mt6797_wifi_scan_target_is_expired(
	const struct mt6797_wifi_scan_target_owner *owner, u64 now_ms)
{
	return owner->have_selected &&
		now_ms - owner->selected.observed_at_ms >= owner->max_age_ms;
}

static void mt6797_wifi_scan_target_clear(
	struct mt6797_wifi_scan_target_owner *owner)
{
	owner->have_selected = false;
	memset(&owner->selected, 0, sizeof(owner->selected));
}

static bool mt6797_wifi_scan_target_same(
	const struct mt6797_wifi_scan_target *first,
	const struct mt6797_wifi_scan_target *second)
{
	return first->observed_at_ms == second->observed_at_ms &&
		first->signal_mbm == second->signal_mbm &&
		first->capability == second->capability &&
		first->ssid_length == second->ssid_length &&
		first->band == second->band && first->channel == second->channel &&
		first->rcpi == second->rcpi &&
		first->frame_type == second->frame_type &&
		first->last_frame_hidden == second->last_frame_hidden &&
		ether_addr_equal(first->bssid, second->bssid) &&
		!memcmp(first->ssid, second->ssid, first->ssid_length);
}

static int mt6797_wifi_scan_target_candidate(
	const struct mt6797_wifi_scan_target_owner *owner,
	const struct mt6797_wifi_rx_metadata *metadata,
	const struct mt6797_wifi_bss_view *bss, u64 now_ms,
	struct mt6797_wifi_scan_target *candidate)
{
	if (!(bss->capability & WLAN_CAPABILITY_ESS) ||
	    bss->capability & WLAN_CAPABILITY_IBSS)
		return -EOPNOTSUPP;
	if (!ether_addr_equal(bss->bssid, owner->key.bssid))
		return -ENOENT;
	if (bss->hidden_ssid) {
		if (!owner->have_selected)
			return -ENODATA;
	} else if (bss->ssid_length != owner->key.ssid_length ||
		   memcmp(bss->ssid, owner->key.ssid, owner->key.ssid_length)) {
		return -ENOENT;
	}

	*candidate = (struct mt6797_wifi_scan_target) {
		.generation = owner->generation,
		.observed_at_ms = now_ms,
		.signal_mbm = metadata->signal_mbm,
		.capability = bss->capability,
		.ssid_length = owner->key.ssid_length,
		.band = metadata->band,
		.channel = metadata->channel,
		.rcpi = metadata->rcpi,
		.frame_type = bss->frame_type,
		.last_frame_hidden = bss->hidden_ssid,
	};
	ether_addr_copy(candidate->bssid, bss->bssid);
	memcpy(candidate->ssid, owner->key.ssid, owner->key.ssid_length);
	return 0;
}

void mt6797_wifi_scan_target_init(
	struct mt6797_wifi_scan_target_owner *owner)
{
	if (owner)
		memset(owner, 0, sizeof(*owner));
}

int mt6797_wifi_scan_target_open(
	struct mt6797_wifi_scan_target_owner *owner,
	const struct mt6797_wifi_scan_target_key *key, u64 max_age_ms)
{
	if (!owner || !key || !max_age_ms ||
	    !is_valid_ether_addr(key->bssid) || !key->ssid_length ||
	    key->ssid_length > IEEE80211_MAX_SSID_LEN ||
	    !memchr_inv(key->ssid, 0, key->ssid_length))
		return -EINVAL;
	if (owner->state == MT6797_WIFI_SCAN_TARGET_TERMINAL)
		return -ESHUTDOWN;
	if (owner->state != MT6797_WIFI_SCAN_TARGET_CLOSED)
		return -EBUSY;
	if (owner->generation == U64_MAX)
		return mt6797_wifi_scan_target_terminal(owner, -EOVERFLOW);

	owner->generation++;
	owner->next_cookie = 1;
	owner->max_age_ms = max_age_ms;
	owner->key = *key;
	owner->state = MT6797_WIFI_SCAN_TARGET_OPEN;
	owner->have_last_now = false;
	return 0;
}

int mt6797_wifi_scan_target_observe(
	struct mt6797_wifi_scan_target_owner *owner, const u8 *packet,
	size_t length, u64 now_ms, enum mt6797_wifi_scan_target_update *update)
{
	struct mt6797_wifi_scan_target candidate = {};
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view bss;
	int ret;

	if (!update)
		return -EINVAL;
	*update = MT6797_WIFI_SCAN_TARGET_IGNORED;
	ret = mt6797_wifi_scan_target_require_open(owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_scan_target_advance(owner, now_ms);
	if (ret)
		return ret;
	ret = mt6797_wifi_rx_metadata_decode(packet, length, &metadata);
	if (ret)
		return ret;
	ret = mt6797_wifi_bss_parse(&metadata, &bss);
	if (ret)
		return ret;

	if (mt6797_wifi_scan_target_is_expired(owner, now_ms))
		mt6797_wifi_scan_target_clear(owner);
	ret = mt6797_wifi_scan_target_candidate(owner, &metadata, &bss, now_ms,
						 &candidate);
	if (ret == -ENOENT || ret == -ENODATA || ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;
	if (owner->have_selected &&
	    mt6797_wifi_scan_target_same(&owner->selected, &candidate)) {
		*update = MT6797_WIFI_SCAN_TARGET_DUPLICATE;
		return 0;
	}
	if (owner->next_cookie == U64_MAX)
		return mt6797_wifi_scan_target_terminal(owner, -EOVERFLOW);

	candidate.cookie = owner->next_cookie++;
	*update = owner->have_selected ? MT6797_WIFI_SCAN_TARGET_UPDATED :
		MT6797_WIFI_SCAN_TARGET_ADDED;
	owner->selected = candidate;
	owner->have_selected = true;
	return 0;
}

int mt6797_wifi_scan_target_expire(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms, bool *expired)
{
	int ret;

	if (!expired)
		return -EINVAL;
	*expired = false;
	ret = mt6797_wifi_scan_target_require_open(owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_scan_target_advance(owner, now_ms);
	if (ret)
		return ret;
	if (mt6797_wifi_scan_target_is_expired(owner, now_ms)) {
		mt6797_wifi_scan_target_clear(owner);
		*expired = true;
	}
	return 0;
}

int mt6797_wifi_scan_target_snapshot(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms,
	struct mt6797_wifi_scan_target *target)
{
	bool expired;
	int ret;

	if (!target)
		return -EINVAL;
	memset(target, 0, sizeof(*target));
	ret = mt6797_wifi_scan_target_expire(owner, now_ms, &expired);
	if (ret)
		return ret;
	if (expired)
		return -ESTALE;
	if (!owner->have_selected)
		return -ENODATA;
	*target = owner->selected;
	return 0;
}

int mt6797_wifi_scan_target_validate(
	struct mt6797_wifi_scan_target_owner *owner, u64 now_ms,
	const struct mt6797_wifi_scan_target *target)
{
	struct mt6797_wifi_scan_target selected;
	int ret;

	if (!target)
		return -EINVAL;
	ret = mt6797_wifi_scan_target_snapshot(owner, now_ms, &selected);
	if (ret == -ENODATA || ret == -ESTALE)
		return -ESTALE;
	if (ret)
		return ret;
	return target->generation == selected.generation &&
		target->cookie == selected.cookie &&
		mt6797_wifi_scan_target_same(target, &selected) ? 0 : -ESTALE;
}

int mt6797_wifi_scan_target_close(
	struct mt6797_wifi_scan_target_owner *owner)
{
	int ret = mt6797_wifi_scan_target_require_open(owner);

	if (ret)
		return ret;
	mt6797_wifi_scan_target_clear(owner);
	memset(&owner->key, 0, sizeof(owner->key));
	owner->max_age_ms = 0;
	owner->have_last_now = false;
	owner->state = MT6797_WIFI_SCAN_TARGET_CLOSED;
	return 0;
}
