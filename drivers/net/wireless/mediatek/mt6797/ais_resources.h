/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_RESOURCES_H
#define __MT6797_WIFI_AIS_RESOURCES_H

#include <linux/bitmap.h>
#include <linux/if_ether.h>
#include <linux/limits.h>
#include <linux/types.h>

#define MT6797_WIFI_AIS_STA_COUNT		27U
#define MT6797_WIFI_AIS_WTBL_COUNT		31U
#define MT6797_WIFI_AIS_INVALID_INDEX		U8_MAX

enum mt6797_wifi_ais_resource_state {
	MT6797_WIFI_AIS_RESOURCES_CLOSED,
	MT6797_WIFI_AIS_RESOURCES_OPEN,
	MT6797_WIFI_AIS_RESOURCES_TERMINAL,
};

enum mt6797_wifi_ais_wtbl_role {
	MT6797_WIFI_AIS_WTBL_ROLE_INVALID,
	MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC,
	MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE,
	MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP,
};

/* Tokens are immutable and meaningful only to the ledger which issued them. */
struct mt6797_wifi_ais_wtbl_handle {
	u64 generation;
	u64 cookie;
	u64 owner_cookie;
	u8 index;
	u8 role;
};

struct mt6797_wifi_ais_bss_handle {
	u64 generation;
	u64 cookie;
	struct mt6797_wifi_ais_wtbl_handle bmc;
	u8 own_addr[ETH_ALEN];
	u8 bss_index;
	u8 own_mac_index;
};

struct mt6797_wifi_ais_sta_handle {
	u64 generation;
	u64 cookie;
	u64 bss_cookie;
	struct mt6797_wifi_ais_wtbl_handle pairwise;
	/* Association-scoped BMC receive context for the single AP STA. */
	struct mt6797_wifi_ais_wtbl_handle ap_bmc_group;
	u8 peer_addr[ETH_ALEN];
	u8 bss_index;
	u8 sta_index;
};

struct mt6797_wifi_ais_wtbl_entry {
	u64 cookie;
	u64 owner_cookie;
	u8 bss_index;
	u8 sta_index;
	u8 role;
};

struct mt6797_wifi_ais_sta_entry {
	u64 cookie;
	u64 bss_cookie;
	u8 peer_addr[ETH_ALEN];
	u8 bss_index;
};

/*
 * This small ledger is deliberately lockless. Its single lifecycle owner must
 * serialize every operation and must not reopen a terminal instance. It models
 * one infrastructure AIS BSS and one current AP association; roaming and TDLS
 * require distinct ownership roles before they may share this ledger.
 */
struct mt6797_wifi_ais_resources {
	DECLARE_BITMAP(sta_used, MT6797_WIFI_AIS_STA_COUNT);
	DECLARE_BITMAP(wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT);
	struct mt6797_wifi_ais_sta_entry sta[MT6797_WIFI_AIS_STA_COUNT];
	struct mt6797_wifi_ais_wtbl_entry wtbl[MT6797_WIFI_AIS_WTBL_COUNT];
	u64 generation;
	u64 next_cookie;
	u64 bss_cookie;
	u8 bss_own_addr[ETH_ALEN];
	u8 bss_index;
	u8 bss_own_mac_index;
	u8 state;
	bool bss_used;
};

void mt6797_wifi_ais_resources_init(struct mt6797_wifi_ais_resources *resources);
int mt6797_wifi_ais_resources_open(struct mt6797_wifi_ais_resources *resources);
int mt6797_wifi_ais_resources_close(struct mt6797_wifi_ais_resources *resources);
int mt6797_wifi_ais_resources_mark_terminal(
	struct mt6797_wifi_ais_resources *resources);

int mt6797_wifi_ais_bss_reserve(
	struct mt6797_wifi_ais_resources *resources, u8 bss_index,
	u8 own_mac_index, const u8 own_addr[ETH_ALEN],
	struct mt6797_wifi_ais_bss_handle *handle);
int mt6797_wifi_ais_bss_release(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_bss_handle *handle);
int mt6797_wifi_ais_sta_reserve(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const u8 peer_addr[ETH_ALEN],
	struct mt6797_wifi_ais_sta_handle *handle);
int mt6797_wifi_ais_sta_release(
	struct mt6797_wifi_ais_resources *resources,
	const struct mt6797_wifi_ais_sta_handle *handle);

#endif /* __MT6797_WIFI_AIS_RESOURCES_H */
