/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_JOIN_H
#define __MT6797_WIFI_AIS_JOIN_H

#include "ais_protocol.h"

#define MT6797_WIFI_AIS_BSS_INFO_LENGTH 96U
#define MT6797_WIFI_AIS_PM_CONNECTED_LENGTH 20U
#define MT6797_WIFI_AIS_PM_ABORT_LENGTH 12U

struct mt6797_wifi_ais_pm_params {
	u16 association_id;
	u16 beacon_interval;
	u8 dtim_period;
};

/* The caller binds this structurally valid beacon to its associated AP. */
int mt6797_wifi_ais_pm_beacon_parse(const u8 *frame, size_t length,
	struct mt6797_wifi_ais_pm_params *params);
int mt6797_wifi_ais_pm_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_pm_params *params, bool connected,
	u8 sequence, u8 *command, size_t capacity);

enum mt6797_wifi_ais_security {
	MT6797_WIFI_AIS_SECURITY_OPEN,
	MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP,
};

/* Negotiated 20 MHz non-HT parameters, matching the current radio capability.
 * Rate sets use GEN3 software indices (including holes at bits 4 and 5),
 * not positions in cfg80211's rate array. Regulatory admission is owner policy.
 */
struct mt6797_wifi_ais_join_params {
	struct mt6797_wifi_ais_channel channel;
	u8 ssid[32];
	u8 ssid_length;
	u8 phy_types;
	u8 basic_phy;
	u8 rcpi;
	u8 disconnect_threshold;
	u16 association_id;
	u16 listen_interval;
	u16 operational_rates;
	u16 desired_rates;
	u16 basic_rates;
	u16 default_rate;
	enum mt6797_wifi_ais_security security;
	bool erp_protection;
	bool short_preamble;
	bool short_slot;
};

/* Parse peer IEs into the negotiated rate/PHY/protection fields only.
 * Other params, including the caller-selected band, remain unchanged.
 * Unsupported mandatory rates/selectors reject the join before any TX.
 */
int mt6797_wifi_ais_join_rates_parse(const u8 *ies, size_t length,
	u16 capability, struct mt6797_wifi_ais_join_params *params);
int mt6797_wifi_ais_join_rates_ies_build(
	const struct mt6797_wifi_ais_join_params *params,
	u8 *ies, size_t capacity, size_t *length);

/* BSS update precedes STA state3; entry to state3 requires its activation event.
 * State1->state2 is local only, as in vendor cnmStaRecChangeState().
 */
int mt6797_wifi_ais_join_bss_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_join_params *params, u8 sequence,
	u8 *command, size_t capacity);
int mt6797_wifi_ais_join_sta_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_join_params *params, u8 sequence,
	u8 *command, size_t capacity,
	struct mt6797_wifi_ais_sta_pending *pending);

#endif
