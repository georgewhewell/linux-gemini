/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_AUTH_H
#define __MT6797_WIFI_AIS_AUTH_H

#include <linux/if_ether.h>
#include <linux/types.h>

#include "ais_protocol.h"
#include "ais_resources.h"

#define MT6797_WIFI_AIS_AUTH_FRAME_LENGTH	30U

/*
 * The request builder publishes this immutable ownership snapshot before the
 * frame can be submitted. The single AIS owner must discard the snapshot when
 * its resource generation is released or fenced.
 */
struct mt6797_wifi_ais_auth_pending {
	u64 generation;
	u64 bss_cookie;
	u64 sta_cookie;
	u8 own_addr[ETH_ALEN];
	u8 peer_addr[ETH_ALEN];
};

struct mt6797_wifi_ais_auth_response {
	u16 status_code;
};

int mt6797_wifi_ais_open_auth_request_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 *frame,
	size_t capacity, struct mt6797_wifi_ais_auth_pending *pending);
int mt6797_wifi_ais_open_auth_response_match(
	const struct mt6797_wifi_ais_auth_pending *pending, const u8 *frame,
	size_t length, enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_auth_response *response);

#endif /* __MT6797_WIFI_AIS_AUTH_H */
