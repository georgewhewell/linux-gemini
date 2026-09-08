// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 nicUpdateBss/cnmStaSendUpdateCmd layouts; explicit little-endian writes. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_join.h"

#define MT6797_JOIN_LEGACY_RATES 0x3fcfU
#define MT6797_JOIN_CCK_RATES 0x000fU

static bool mt6797_join_params_valid(const struct mt6797_wifi_ais_join_params *p)
{
	if (!p || !p->ssid_length || p->ssid_length > sizeof(p->ssid) ||
	    !p->association_id || p->association_id > 2007 ||
	    !p->listen_interval || !p->operational_rates || !p->desired_rates ||
	    !p->basic_rates || p->operational_rates & ~MT6797_JOIN_LEGACY_RATES ||
	    p->desired_rates & ~p->operational_rates ||
	    p->basic_rates & ~p->desired_rates || p->default_rate > 0xfff ||
	    p->rcpi > 220 ||
	    (p->security != MT6797_WIFI_AIS_SECURITY_OPEN &&
	     p->security != MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP))
		return false;
	if (p->channel.band == MT6797_WIFI_AIS_BAND_2GHZ)
		return p->channel.primary >= 1 && p->channel.primary <= 14 &&
			p->phy_types && !(p->phy_types & ~3U) &&
			p->basic_phy <= 1 && (p->phy_types & (1U << p->basic_phy));
	if (p->channel.band == MT6797_WIFI_AIS_BAND_5GHZ)
		return p->channel.primary && p->phy_types == 8 && p->basic_phy == 3 &&
			!(p->operational_rates & MT6797_JOIN_CCK_RATES) &&
			!p->erp_protection && !p->short_preamble;
	return false;
}

int mt6797_wifi_ais_join_bss_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_join_params *params, u8 sequence,
	u8 *command, size_t capacity)
{
	struct mt6797_wifi_ais_sta_pending owner;
	u8 *body, *rlm;
	int ret;

	if (!command || !mt6797_join_params_valid(params))
		return -EINVAL;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, sequence, &owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_command_init(command, capacity,
		MT6797_WIFI_AIS_BSS_INFO_LENGTH - MT6797_WIFI_COMMAND_HEADER_LENGTH,
		0x12, true, sequence);
	if (ret)
		return ret;
	body = command + MT6797_WIFI_COMMAND_HEADER_LENGTH;
	body[0] = bss->bss_index;
	/* Media connected and infrastructure mode both encode as zero. */
	body[3] = params->ssid_length;
	memcpy(body + 4, params->ssid, params->ssid_length);
	memcpy(body + 36, sta->peer_addr, ETH_ALEN);
	put_unaligned_le16(params->operational_rates, body + 44);
	put_unaligned_le16(params->basic_rates, body + 46);
	body[48] = sta->sta_index;
	put_unaligned_le16(params->default_rate, body + 50);
	body[52] = params->basic_phy;
	/* wlan_oid.h: OPEN=0, WPA2_PSK=7, ENCRYPTION_DISABLED=1, CCMP=6. */
	body[53] = params->security == MT6797_WIFI_AIS_SECURITY_OPEN ? 0 : 7;
	body[54] = params->security == MT6797_WIFI_AIS_SECURITY_OPEN ? 1 : 6;
	body[55] = params->phy_types;
	body[58] = bss->bmc.index;
	body[60] = params->disconnect_threshold;
	rlm = body + 68;
	rlm[0] = bss->bss_index;
	rlm[1] = params->channel.band;
	rlm[2] = params->channel.primary;
	rlm[4] = params->erp_protection;
	rlm[13] = params->short_preamble;
	rlm[14] = params->short_slot;
	/* GEN3 rlmFillSyncCmdParam copies basic legacy rates here even without VHT. */
	put_unaligned_le16(params->basic_rates, rlm + 18);
	return 0;
}

int mt6797_wifi_ais_join_sta_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_join_params *params, u8 sequence,
	u8 *command, size_t capacity,
	struct mt6797_wifi_ais_sta_pending *pending)
{
	struct mt6797_wifi_ais_sta_pending owner;
	u8 *body;
	int ret;

	if (pending)
		memset(pending, 0, sizeof(*pending));
	if (!command || !pending || !mt6797_join_params_valid(params))
		return -EINVAL;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, sequence, &owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_ais_sta_state1_build(bss, sta, sequence, command, capacity);
	if (ret)
		return ret;
	body = command + MT6797_WIFI_COMMAND_HEADER_LENGTH;
	put_unaligned_le16(params->association_id, body + 8);
	put_unaligned_le16(params->listen_interval, body + 10);
	body[13] = params->phy_types;
	put_unaligned_le16(params->desired_rates, body + 14);
	put_unaligned_le16(params->basic_rates, body + 16);
	body[20] = 2; /* STA_STATE_3 */
	body[50] = params->rcpi;
	body[51] = 1; /* Require matching EVENT_ID_ACTIVATE_STA_REC. */
	put_unaligned_le16(params->default_rate, body + 74);
	*pending = owner;
	return 0;
}
