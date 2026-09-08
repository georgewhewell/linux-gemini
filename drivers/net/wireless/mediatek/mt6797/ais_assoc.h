/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_ASSOC_H
#define __MT6797_WIFI_AIS_ASSOC_H

#include "ais_protocol.h"

#define MT6797_WIFI_AIS_ASSOC_REQUEST_FIXED_LENGTH 28U
#define MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH 30U

struct mt6797_wifi_ais_assoc_params {
	/* Complete IE stream, including SSID and supported rates. */
	const u8 *ies;
	size_t ies_length;
	u16 capability;
	u16 listen_interval;
};

/* Discard when the owner releases/fences its resource generation. */
struct mt6797_wifi_ais_assoc_pending {
	u64 generation;
	u64 bss_cookie;
	u64 sta_cookie;
	u8 own_addr[ETH_ALEN];
	u8 peer_addr[ETH_ALEN];
};

struct mt6797_wifi_ais_assoc_response {
	/* Borrowed from the RX frame; copy before leaving the RX callback. */
	const u8 *ies;
	size_t ies_length;
	u16 capability;
	u16 status_code;
	u16 association_id;
};

int mt6797_wifi_ais_assoc_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_assoc_params *params,
	u8 *frame, size_t capacity, size_t *length,
	struct mt6797_wifi_ais_assoc_pending *pending);
int mt6797_wifi_ais_assoc_response_match(
	const struct mt6797_wifi_ais_assoc_pending *pending,
	const u8 *frame, size_t length,
	enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_assoc_response *response);

#endif
