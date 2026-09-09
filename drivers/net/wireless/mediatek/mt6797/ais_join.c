// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 nicUpdateBss/cnmStaSendUpdateCmd layouts; explicit little-endian writes. */
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_join.h"

#define MT6797_JOIN_LEGACY_RATES 0x3fcfU
#define MT6797_JOIN_CCK_RATES 0x000fU

int mt6797_wifi_ais_pm_beacon_parse(const u8 *frame, size_t length,
	struct mt6797_wifi_ais_pm_params *params)
{
	struct mt6797_wifi_ais_pm_params candidate = {};
	size_t offset;
	u16 control;
	bool have_tim = false;

	if (!params)
		return -EINVAL;
	*params = candidate;
	if (!frame || length < 36)
		return -EMSGSIZE;
	control = get_unaligned_le16(frame);
	if ((control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
			IEEE80211_FCTL_STYPE)) != IEEE80211_STYPE_BEACON ||
	    (control & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
			IEEE80211_FCTL_MOREFRAGS | IEEE80211_FCTL_PROTECTED |
			IEEE80211_FCTL_ORDER)) ||
	    (get_unaligned_le16(frame + 22) & IEEE80211_SCTL_FRAG))
		return -EPROTO;
	candidate.beacon_interval = get_unaligned_le16(frame + 32);
	if (!candidate.beacon_interval)
		return -EPROTO;
	for (offset = 36; offset < length; offset += frame[offset + 1] + 2) {
		if (length - offset < 2 || frame[offset + 1] > length - offset - 2)
			return -EPROTO;
		if (frame[offset] != WLAN_EID_TIM)
			continue;
		if (have_tim || frame[offset + 1] < 4 || !frame[offset + 3] ||
		    frame[offset + 2] >= frame[offset + 3])
			return -EPROTO;
		have_tim = true;
		candidate.dtim_period = frame[offset + 3];
	}
	if (!have_tim)
		return -ENOENT;
	*params = candidate;
	return 0;
}

int mt6797_wifi_ais_pm_build(
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_pm_params *params, bool connected,
	u8 sequence, u8 *command, size_t capacity)
{
	struct mt6797_wifi_ais_sta_pending owner;
	u8 *body;
	int ret;

	if (!command || (connected && (!params || !params->association_id ||
	    params->association_id > 2007 || !params->beacon_interval || !params->dtim_period)))
		return -EINVAL;
	ret = mt6797_wifi_ais_sta_pending_init(bss, sta, sequence, &owner);
	if (ret)
		return ret;
	/* GEN3 CMD_INDICATE_PM_BSS_CONNECTED/ABORT; non-QoS, no U-APSD. */
	ret = mt6797_wifi_command_init(command, capacity, connected ? 12 : 4,
		connected ? 0x16 : 0x17, true, sequence);
	if (ret)
		return ret;
	body = command + MT6797_WIFI_COMMAND_HEADER_LENGTH;
	body[0] = bss->bss_index;
	if (connected) {
		body[1] = params->dtim_period;
		put_unaligned_le16(params->association_id, body + 2);
		put_unaligned_le16(params->beacon_interval, body + 4);
	}
	return 0;
}

/* GEN3 wlan_def.h software-index and long-preamble hardware encodings. */
static const struct {
	u8 rate;
	u8 index;
	u8 hardware;
} mt6797_join_rates[] = {
	{ 2, 0, 0x00 }, { 4, 1, 0x01 }, { 11, 2, 0x02 }, { 22, 3, 0x03 },
	{ 12, 6, 0x4b }, { 18, 7, 0x4f }, { 24, 8, 0x4a }, { 36, 9, 0x4e },
	{ 48, 10, 0x49 }, { 72, 11, 0x4d }, { 96, 12, 0x48 }, { 108, 13, 0x4c },
};

int mt6797_wifi_ais_join_rates_parse(const u8 *ies, size_t length,
	u16 capability, struct mt6797_wifi_ais_join_params *params)
{
	u16 operational = 0, basic = 0, allowed;
	bool rates = false, extended = false, erp = false, protect = false;
	bool barker = false;
	u8 id, count, index, value;
	size_t offset = 0, j;
	u16 default_rate = 0;

	if (!params || !ies || !length ||
	    length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH)
		return -EINVAL;
	if (params->channel.band == MT6797_WIFI_AIS_BAND_2GHZ)
		allowed = MT6797_JOIN_LEGACY_RATES;
	else if (params->channel.band == MT6797_WIFI_AIS_BAND_5GHZ)
		allowed = MT6797_JOIN_LEGACY_RATES & ~MT6797_JOIN_CCK_RATES;
	else
		return -EINVAL;
	while (offset < length) {
		if (length - offset < 2)
			return -EPROTO;
		id = ies[offset];
		count = ies[offset + 1];
		if (count > length - offset - 2)
			return -EPROTO;
		if (id == WLAN_EID_ERP_INFO) {
			if (erp || count != 1)
				return -EPROTO;
			erp = true;
			protect = ies[offset + 2] & WLAN_ERP_USE_PROTECTION;
			barker = ies[offset + 2] & WLAN_ERP_BARKER_PREAMBLE;
		}
		if (id == WLAN_EID_SUPP_RATES || id == WLAN_EID_EXT_SUPP_RATES) {
			if (!count || (id == WLAN_EID_SUPP_RATES && (rates || count > 8)) ||
			    (id == WLAN_EID_EXT_SUPP_RATES && extended))
				return -EPROTO;
			if (id == WLAN_EID_SUPP_RATES)
				rates = true;
			else
				extended = true;
			for (j = 0; j < count; j++) {
				value = ies[offset + 2 + j];
				for (index = 0; index < ARRAY_SIZE(mt6797_join_rates); index++)
					if ((value & 0x7f) == mt6797_join_rates[index].rate &&
					    (allowed & (1U << mt6797_join_rates[index].index)))
						break;
				if (index == ARRAY_SIZE(mt6797_join_rates)) {
					if (value & 0x80)
						return -EOPNOTSUPP;
					continue;
				}
				operational |= 1U << mt6797_join_rates[index].index;
				if (value & 0x80)
					basic |= 1U << mt6797_join_rates[index].index;
			}
		}
		offset += count + 2;
	}
	if (!rates)
		return -EPROTO;
	if (!operational || !basic)
		return -EOPNOTSUPP;
	/* Match vendor nicTxUpdateBssDefaultRate: lowest software basic-rate index. */
	for (index = 0; index < ARRAY_SIZE(mt6797_join_rates); index++)
		if (basic & (1U << mt6797_join_rates[index].index)) {
			default_rate = mt6797_join_rates[index].hardware;
			break;
		}
	params->operational_rates = operational;
	params->desired_rates = operational;
	params->basic_rates = basic;
	params->default_rate = default_rate;
	params->erp_protection = params->channel.band == MT6797_WIFI_AIS_BAND_2GHZ && protect;
	params->short_preamble = params->channel.band == MT6797_WIFI_AIS_BAND_2GHZ &&
		!barker && (capability & WLAN_CAPABILITY_SHORT_PREAMBLE);
	params->short_slot = params->channel.band == MT6797_WIFI_AIS_BAND_5GHZ ||
		(capability & WLAN_CAPABILITY_SHORT_SLOT_TIME);
	if (params->channel.band == MT6797_WIFI_AIS_BAND_5GHZ) {
		params->phy_types = 8;
		params->basic_phy = 3;
	} else {
		params->phy_types = (operational & MT6797_JOIN_CCK_RATES ? 1 : 0) |
			(operational & ~MT6797_JOIN_CCK_RATES ? 2 : 0);
		params->basic_phy = operational & ~MT6797_JOIN_CCK_RATES ? 1 : 0;
	}
	return 0;
}

int mt6797_wifi_ais_join_rates_ies_build(
	const struct mt6797_wifi_ais_join_params *params,
	u8 *ies, size_t capacity, size_t *length)
{
	u8 values[ARRAY_SIZE(mt6797_join_rates)];
	unsigned int index, count = 0, first;
	size_t total;

	if (length)
		*length = 0;
	if (!params || !ies || !length || !params->desired_rates ||
	    params->desired_rates & ~MT6797_JOIN_LEGACY_RATES ||
	    !params->basic_rates || params->basic_rates & ~params->desired_rates)
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(mt6797_join_rates); index++)
		if (params->desired_rates & (1U << mt6797_join_rates[index].index))
			values[count++] = mt6797_join_rates[index].rate |
				(params->basic_rates & (1U << mt6797_join_rates[index].index) ? 0x80 : 0);
	first = min(count, 8U);
	total = count + 2 + (count > first ? 2 : 0);
	if (capacity < total)
		return -EMSGSIZE;
	ies[0] = WLAN_EID_SUPP_RATES;
	ies[1] = first;
	memcpy(ies + 2, values, first);
	if (count > first) {
		ies[first + 2] = WLAN_EID_EXT_SUPP_RATES;
		ies[first + 3] = count - first;
		memcpy(ies + first + 4, values + first, count - first);
	}
	*length = total;
	return 0;
}

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
