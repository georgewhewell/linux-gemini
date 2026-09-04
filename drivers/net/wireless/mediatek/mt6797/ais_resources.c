// SPDX-License-Identifier: GPL-2.0-only
/* Pure, caller-serialized AIS STA and WTBL ownership ledger. */

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "ais_resources.h"

static int mt6797_wifi_ais_require_open(
	const struct mt6797_wifi_ais_resources *resources)
{
	if (!resources)
		return -EINVAL;
	return resources->state == MT6797_WIFI_AIS_RESOURCES_OPEN ? 0 :
		-ESHUTDOWN;
}

static bool mt6797_wifi_ais_cookie_capacity(
	struct mt6797_wifi_ais_resources *resources, u64 count)
{
	if (resources->next_cookie &&
	    count <= U64_MAX - resources->next_cookie)
		return true;
	resources->state = MT6797_WIFI_AIS_RESOURCES_TERMINAL;
	return false;
}

static int mt6797_wifi_ais_wtbl_reserve(
	struct mt6797_wifi_ais_resources *resources,
	enum mt6797_wifi_ais_wtbl_role role, u64 owner_cookie,
	u8 bss_index, u8 sta_index,
	struct mt6797_wifi_ais_wtbl_handle *handle)
{
	struct mt6797_wifi_ais_wtbl_entry *entry;
	unsigned int index;

	index = find_first_zero_bit(resources->wtbl_used,
				    MT6797_WIFI_AIS_WTBL_COUNT);
	if (index >= MT6797_WIFI_AIS_WTBL_COUNT)
		return -ENOSPC;

	entry = &resources->wtbl[index];
	entry->cookie = resources->next_cookie++;
	entry->owner_cookie = owner_cookie;
	entry->bss_index = bss_index;
	entry->sta_index = sta_index;
	entry->role = role;
	__set_bit(index, resources->wtbl_used);
	*handle = (struct mt6797_wifi_ais_wtbl_handle) {
		.generation = resources->generation,
		.cookie = entry->cookie,
		.owner_cookie = owner_cookie,
		.index = index,
		.role = role,
	};
	return 0;
}

static bool mt6797_wifi_ais_wtbl_matches(
	const struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_wtbl_handle *handle,
	enum mt6797_wifi_ais_wtbl_role role, u64 owner_cookie,
	u8 bss_index, u8 sta_index)
{
	const struct mt6797_wifi_ais_wtbl_entry *entry;

	if (handle->generation != resources->generation ||
	    handle->index >= MT6797_WIFI_AIS_WTBL_COUNT ||
	    handle->role != role || handle->owner_cookie != owner_cookie ||
	    !test_bit(handle->index, resources->wtbl_used))
		return false;
	entry = &resources->wtbl[handle->index];
	return entry->cookie == handle->cookie && entry->role == role &&
		entry->owner_cookie == owner_cookie &&
		entry->bss_index == bss_index && entry->sta_index == sta_index;
}

static void mt6797_wifi_ais_wtbl_release(
	struct mt6797_wifi_ais_resources *resources, u8 index)
{
	__clear_bit(index, resources->wtbl_used);
	memset(&resources->wtbl[index], 0, sizeof(resources->wtbl[index]));
}

static bool mt6797_wifi_ais_bss_matches(
	const struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_bss_handle *handle)
{
	return resources->bss_used &&
		handle->generation == resources->generation &&
		handle->cookie == resources->bss_cookie &&
		handle->bss_index == resources->bss_index &&
		handle->own_mac_index == resources->bss_own_mac_index &&
		!memcmp(handle->own_addr, resources->bss_own_addr, ETH_ALEN) &&
		mt6797_wifi_ais_wtbl_matches(resources, &handle->bmc,
			MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC, handle->cookie,
			handle->bss_index, MT6797_WIFI_AIS_INVALID_INDEX);
}

void mt6797_wifi_ais_resources_init(struct mt6797_wifi_ais_resources *resources)
{
	if (resources)
		memset(resources, 0, sizeof(*resources));
}

int mt6797_wifi_ais_resources_open(struct mt6797_wifi_ais_resources *resources)
{
	if (!resources)
		return -EINVAL;
	if (resources->state == MT6797_WIFI_AIS_RESOURCES_TERMINAL)
		return -ESHUTDOWN;
	if (resources->state != MT6797_WIFI_AIS_RESOURCES_CLOSED)
		return -EBUSY;
	if (resources->bss_used || !bitmap_empty(resources->sta_used,
						 MT6797_WIFI_AIS_STA_COUNT) ||
	    !bitmap_empty(resources->wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT))
		return -EUCLEAN;
	if (resources->generation == U64_MAX) {
		resources->state = MT6797_WIFI_AIS_RESOURCES_TERMINAL;
		return -EOVERFLOW;
	}

	resources->generation++;
	resources->next_cookie = 1;
	resources->state = MT6797_WIFI_AIS_RESOURCES_OPEN;
	return 0;
}

int mt6797_wifi_ais_resources_close(struct mt6797_wifi_ais_resources *resources)
{
	int ret = mt6797_wifi_ais_require_open(resources);

	if (ret)
		return ret;
	if (resources->bss_used || !bitmap_empty(resources->sta_used,
						 MT6797_WIFI_AIS_STA_COUNT) ||
	    !bitmap_empty(resources->wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT))
		return -EBUSY;
	resources->state = MT6797_WIFI_AIS_RESOURCES_CLOSED;
	return 0;
}

int mt6797_wifi_ais_resources_mark_terminal(
	struct mt6797_wifi_ais_resources *resources)
{
	if (!resources)
		return -EINVAL;
	if (resources->state == MT6797_WIFI_AIS_RESOURCES_TERMINAL)
		return 0;
	if (resources->state != MT6797_WIFI_AIS_RESOURCES_OPEN)
		return -ESHUTDOWN;
	resources->state = MT6797_WIFI_AIS_RESOURCES_TERMINAL;
	return 0;
}

int mt6797_wifi_ais_bss_reserve(
	struct mt6797_wifi_ais_resources *resources, u8 bss_index,
	u8 own_mac_index, const u8 own_addr[ETH_ALEN],
	struct mt6797_wifi_ais_bss_handle *handle)
{
	struct mt6797_wifi_ais_bss_handle candidate = {};
	u64 next_cookie;
	int ret;

	if (!own_addr || !handle)
		return -EINVAL;
	memset(handle, 0, sizeof(*handle));
	ret = mt6797_wifi_ais_require_open(resources);
	if (ret)
		return ret;
	if (resources->bss_used)
		return -EBUSY;
	if (!mt6797_wifi_ais_cookie_capacity(resources, 2))
		return -EOVERFLOW;

	next_cookie = resources->next_cookie;
	candidate.generation = resources->generation;
	candidate.cookie = resources->next_cookie++;
	candidate.bss_index = bss_index;
	candidate.own_mac_index = own_mac_index;
	memcpy(candidate.own_addr, own_addr, ETH_ALEN);
	ret = mt6797_wifi_ais_wtbl_reserve(resources,
			MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC, candidate.cookie,
			bss_index, MT6797_WIFI_AIS_INVALID_INDEX, &candidate.bmc);
	if (ret) {
		resources->next_cookie = next_cookie;
		return ret;
	}

	resources->bss_used = true;
	resources->bss_cookie = candidate.cookie;
	resources->bss_index = bss_index;
	resources->bss_own_mac_index = own_mac_index;
	memcpy(resources->bss_own_addr, own_addr, ETH_ALEN);
	*handle = candidate;
	return 0;
}

int mt6797_wifi_ais_bss_release(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_bss_handle *handle)
{
	int ret;

	if (!handle)
		return -EINVAL;
	ret = mt6797_wifi_ais_require_open(resources);
	if (ret)
		return ret;
	if (!mt6797_wifi_ais_bss_matches(resources, handle))
		return -ESTALE;
	if (!bitmap_empty(resources->sta_used, MT6797_WIFI_AIS_STA_COUNT))
		return -EBUSY;

	mt6797_wifi_ais_wtbl_release(resources, handle->bmc.index);
	resources->bss_used = false;
	resources->bss_cookie = 0;
	resources->bss_index = 0;
	resources->bss_own_mac_index = 0;
	memset(resources->bss_own_addr, 0, ETH_ALEN);
	return 0;
}

int mt6797_wifi_ais_sta_reserve(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const u8 peer_addr[ETH_ALEN],
	struct mt6797_wifi_ais_sta_handle *handle)
{
	struct mt6797_wifi_ais_sta_handle candidate = {};
	struct mt6797_wifi_ais_sta_entry *entry;
	unsigned int sta_index;
	u64 next_cookie;
	int ret;

	if (!bss || !peer_addr || !handle)
		return -EINVAL;
	memset(handle, 0, sizeof(*handle));
	ret = mt6797_wifi_ais_require_open(resources);
	if (ret)
		return ret;
	if (!mt6797_wifi_ais_bss_matches(resources, bss))
		return -ESTALE;
	if (!bitmap_empty(resources->sta_used, MT6797_WIFI_AIS_STA_COUNT))
		return -EBUSY;
	sta_index = find_first_zero_bit(resources->sta_used,
					MT6797_WIFI_AIS_STA_COUNT);
	if (sta_index >= MT6797_WIFI_AIS_STA_COUNT)
		return -ENOSPC;
	if (!mt6797_wifi_ais_cookie_capacity(resources, 3))
		return -EOVERFLOW;

	next_cookie = resources->next_cookie;
	candidate.generation = resources->generation;
	candidate.cookie = resources->next_cookie++;
	candidate.bss_cookie = bss->cookie;
	candidate.bss_index = bss->bss_index;
	candidate.sta_index = sta_index;
	memcpy(candidate.peer_addr, peer_addr, ETH_ALEN);

	entry = &resources->sta[sta_index];
	entry->cookie = candidate.cookie;
	entry->bss_cookie = bss->cookie;
	entry->bss_index = bss->bss_index;
	memcpy(entry->peer_addr, peer_addr, ETH_ALEN);
	__set_bit(sta_index, resources->sta_used);

	ret = mt6797_wifi_ais_wtbl_reserve(resources,
			MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE,
			candidate.cookie, bss->bss_index, sta_index,
			&candidate.pairwise);
	if (ret)
		goto clear_sta;
	ret = mt6797_wifi_ais_wtbl_reserve(resources,
			MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP,
			candidate.cookie, bss->bss_index, sta_index,
			&candidate.ap_bmc_group);
	if (ret)
		goto clear_pairwise;

	*handle = candidate;
	return 0;

clear_pairwise:
	mt6797_wifi_ais_wtbl_release(resources, candidate.pairwise.index);
clear_sta:
	__clear_bit(sta_index, resources->sta_used);
	memset(entry, 0, sizeof(*entry));
	resources->next_cookie = next_cookie;
	return ret;
}

int mt6797_wifi_ais_sta_release(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_sta_handle *handle)
{
	const struct mt6797_wifi_ais_sta_entry *entry;
	int ret;

	if (!handle)
		return -EINVAL;
	ret = mt6797_wifi_ais_require_open(resources);
	if (ret)
		return ret;
	if (!resources->bss_used ||
	    handle->generation != resources->generation ||
	    handle->bss_cookie != resources->bss_cookie ||
	    handle->bss_index != resources->bss_index ||
	    handle->sta_index >= MT6797_WIFI_AIS_STA_COUNT ||
	    !test_bit(handle->sta_index, resources->sta_used))
		return -ESTALE;
	entry = &resources->sta[handle->sta_index];
	if (entry->cookie != handle->cookie ||
	    entry->bss_cookie != handle->bss_cookie ||
	    entry->bss_index != handle->bss_index ||
	    memcmp(entry->peer_addr, handle->peer_addr, ETH_ALEN) ||
	    !mt6797_wifi_ais_wtbl_matches(resources, &handle->pairwise,
		MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE, handle->cookie,
		handle->bss_index, handle->sta_index) ||
	    !mt6797_wifi_ais_wtbl_matches(resources, &handle->ap_bmc_group,
		MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP, handle->cookie,
		handle->bss_index, handle->sta_index))
		return -ESTALE;

	mt6797_wifi_ais_wtbl_release(resources, handle->ap_bmc_group.index);
	mt6797_wifi_ais_wtbl_release(resources, handle->pairwise.index);
	__clear_bit(handle->sta_index, resources->sta_used);
	memset(&resources->sta[handle->sta_index], 0,
	       sizeof(resources->sta[handle->sta_index]));
	return 0;
}
