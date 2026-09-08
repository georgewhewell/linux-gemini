/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_KEY_H
#define __MT6797_WIFI_AIS_KEY_H

#include "ais_protocol.h"

#define MT6797_WIFI_AIS_KEY_COMMAND_LENGTH 72U
#define MT6797_WIFI_AIS_CCMP_KEY_LENGTH 16U
#define MT6797_WIFI_AIS_KEY_RSC_LENGTH 16U

struct mt6797_wifi_ais_key_params {
	const u8 *material;
	size_t material_length;
	/* Raw firmware RSC field, not an assertion about hardware replay policy. */
	u8 rsc[MT6797_WIFI_AIS_KEY_RSC_LENGTH];
	u8 index;
	bool pairwise;
	bool remove;
};

/* Command contains key material: never log it; wipe after synchronous send.
 * Uses the existing station's pairwise or AP-group WTBL allocation.
 * A group rekey/delete owner must track which key ID occupies that allocation.
 */
int mt6797_wifi_ais_ccmp_key_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_key_params *params, u8 sequence,
	u8 *command, size_t capacity);

/* EVENT_ID_ADD_PKEY_DONE is unsolicited and has no key ID/cookie on the wire.
 * Invoke only while one pairwise installation is armed for this station.
 * Caller owns epoch/reinstall policy; this matcher cannot disambiguate rekeys.
 */
int mt6797_wifi_ais_ccmp_ready_match(
	const struct mt6797_wifi_ais_sta_pending *pending,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition);

#endif
