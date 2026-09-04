// SPDX-License-Identifier: GPL-2.0-only
/* Pure MT6797 GEN3 receive metadata and scan-frame parsing. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"
#include "rx_metadata.h"

#define MT6797_WIFI_RX_PACKET_TYPE_MASK		0xe00fU
#define MT6797_WIFI_RX_PACKET_TYPE_SW_FRAME	0xe001U
#define MT6797_WIFI_RX_GROUPS_MASK		GENMASK(12, 9)
#define MT6797_WIFI_RX_GROUPS_SHIFT		9
#define MT6797_WIFI_RX_GROUP_1			BIT(0)
#define MT6797_WIFI_RX_GROUP_2			BIT(1)
#define MT6797_WIFI_RX_GROUP_3			BIT(2)
#define MT6797_WIFI_RX_GROUP_4			BIT(3)
#define MT6797_WIFI_RX_GROUP_1_LENGTH		16U
#define MT6797_WIFI_RX_GROUP_2_LENGTH		8U
#define MT6797_WIFI_RX_GROUP_3_LENGTH		24U
#define MT6797_WIFI_RX_GROUP_4_LENGTH		16U
#define MT6797_WIFI_RX_GROUP_3_VECTOR_2_OFFSET	8U
#define MT6797_WIFI_RX_RCPI_SHIFT		8
#define MT6797_WIFI_RX_RCPI_MAX			220U
#define MT6797_WIFI_RX_HEADER_LENGTH_MASK	GENMASK(5, 0)
#define MT6797_WIFI_RX_HEADER_OFFSET		BIT(6)
#define MT6797_WIFI_RX_HEADER_TRANSLATED	BIT(7)
#define MT6797_WIFI_RX_STATUS_ERROR_MASK		(BIT(1) | BIT(3) | BIT(4))
#define MT6797_WIFI_RX_5GHZ_CHANNEL_BIAS		241U
#define MT6797_WIFI_RX_MAX_REGULAR_5GHZ_CHANNEL	199U
#define MT6797_WIFI_IEEE80211_HEADER_LENGTH	24U
#define MT6797_WIFI_BSS_FIXED_LENGTH		12U
#define MT6797_WIFI_BSS_FRAME_MIN_LENGTH		\
	(MT6797_WIFI_IEEE80211_HEADER_LENGTH + MT6797_WIFI_BSS_FIXED_LENGTH)
#define MT6797_WIFI_BSS_CAPABILITY_OFFSET	34U

static int mt6797_wifi_rx_add_group(size_t *offset, size_t length)
{
	return check_add_overflow(*offset, length, offset) ? -EOVERFLOW : 0;
}

static int mt6797_wifi_rx_group3_offset(u8 groups, size_t *group3,
					size_t *frame)
{
	size_t offset = MT6797_WIFI_RX_DESCRIPTOR_LENGTH;
	int ret;

	if (!(groups & MT6797_WIFI_RX_GROUP_3))
		return -ENODATA;
	if (groups & MT6797_WIFI_RX_GROUP_4) {
		ret = mt6797_wifi_rx_add_group(&offset, MT6797_WIFI_RX_GROUP_4_LENGTH);
		if (ret)
			return ret;
	}
	if (groups & MT6797_WIFI_RX_GROUP_1) {
		ret = mt6797_wifi_rx_add_group(&offset, MT6797_WIFI_RX_GROUP_1_LENGTH);
		if (ret)
			return ret;
	}
	if (groups & MT6797_WIFI_RX_GROUP_2) {
		ret = mt6797_wifi_rx_add_group(&offset, MT6797_WIFI_RX_GROUP_2_LENGTH);
		if (ret)
			return ret;
	}
	*group3 = offset;
	ret = mt6797_wifi_rx_add_group(&offset, MT6797_WIFI_RX_GROUP_3_LENGTH);
	if (ret)
		return ret;
	*frame = offset;
	return 0;
}

static int mt6797_wifi_rx_channel_decode(u8 encoded,
					 enum mt6797_wifi_rx_band *band,
					 u8 *channel)
{
	if (!encoded)
		return -EPROTO;
	if (encoded <= 14) {
		*band = MT6797_WIFI_RX_BAND_2GHZ;
		*channel = encoded;
		return 0;
	}

	*band = MT6797_WIFI_RX_BAND_5GHZ;
	if (encoded > MT6797_WIFI_RX_5GHZ_CHANNEL_BIAS)
		*channel = encoded - MT6797_WIFI_RX_5GHZ_CHANNEL_BIAS;
	else
		*channel = encoded;
	if (!*channel || *channel > MT6797_WIFI_RX_MAX_REGULAR_5GHZ_CHANNEL)
		return -EPROTO;
	return 0;
}

int mt6797_wifi_rx_metadata_decode(const u8 *packet, size_t available,
				   struct mt6797_wifi_rx_metadata *metadata)
{
	struct mt6797_wifi_rx_metadata decoded = {};
	size_t packet_length;
	size_t group3;
	size_t frame;
	u32 vector2;
	u16 packet_type;
	u8 groups;
	u8 header;
	int ret;

	if (!metadata)
		return -EINVAL;
	*metadata = (struct mt6797_wifi_rx_metadata) {};
	if (!packet)
		return -EINVAL;
	if (available < MT6797_WIFI_RX_DESCRIPTOR_LENGTH)
		return -EMSGSIZE;

	packet_length = get_unaligned_le16(packet);
	if (packet_length < MT6797_WIFI_RX_DESCRIPTOR_LENGTH ||
	    packet_length > MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH ||
	    packet_length > available)
		return -EMSGSIZE;
	packet_type = get_unaligned_le16(packet + 2);
	if ((packet_type & MT6797_WIFI_RX_PACKET_TYPE_MASK) !=
	    MT6797_WIFI_RX_PACKET_TYPE_SW_FRAME)
		return -EPROTO;
	if (get_unaligned_le16(packet + 10) &
	    MT6797_WIFI_RX_STATUS_ERROR_MASK)
		return -EBADMSG;

	header = packet[6];
	if (header & MT6797_WIFI_RX_HEADER_TRANSLATED)
		return -EPROTO;
	if ((header & MT6797_WIFI_RX_HEADER_LENGTH_MASK) !=
	    MT6797_WIFI_IEEE80211_HEADER_LENGTH)
		return -EPROTO;
	groups = (packet_type & MT6797_WIFI_RX_GROUPS_MASK) >>
		MT6797_WIFI_RX_GROUPS_SHIFT;
	ret = mt6797_wifi_rx_group3_offset(groups, &group3, &frame);
	if (ret)
		return ret;
	if (header & MT6797_WIFI_RX_HEADER_OFFSET) {
		if (check_add_overflow(frame, (size_t)2, &frame))
			return -EOVERFLOW;
	}
	if (group3 + MT6797_WIFI_RX_GROUP_3_LENGTH > packet_length ||
	    frame > packet_length ||
	    packet_length - frame < MT6797_WIFI_BSS_FRAME_MIN_LENGTH)
		return -EPROTO;

	ret = mt6797_wifi_rx_channel_decode(packet[5], &decoded.band,
					    &decoded.channel);
	if (ret)
		return ret;
	vector2 = get_unaligned_le32(packet + group3 +
				     MT6797_WIFI_RX_GROUP_3_VECTOR_2_OFFSET);
	decoded.rcpi = (vector2 >> MT6797_WIFI_RX_RCPI_SHIFT) & 0xff;
	if (decoded.rcpi > MT6797_WIFI_RX_RCPI_MAX)
		return -ERANGE;
	decoded.signal_mbm = ((s32)(decoded.rcpi >> 1) - 110) * 100;
	decoded.frame = packet + frame;
	decoded.frame_length = packet_length - frame;
	*metadata = decoded;
	return 0;
}

static int mt6797_wifi_bss_channel_check(const struct mt6797_wifi_rx_metadata *metadata,
					 bool have_ds, u8 ds, bool have_ht, u8 ht)
{
	u8 advertised = have_ds ? ds : ht;

	if (have_ds && have_ht && ds != ht)
		return -EPROTO;
	if (!have_ds && !have_ht)
		return 0;
	if (!advertised || advertised != metadata->channel)
		return -EPROTO;
	if (metadata->band == MT6797_WIFI_RX_BAND_2GHZ)
		return advertised <= 14 ? 0 : -EPROTO;
	return have_ds ? -EPROTO : 0;
}

int mt6797_wifi_bss_parse(const struct mt6797_wifi_rx_metadata *metadata,
			  struct mt6797_wifi_bss_view *bss)
{
	struct mt6797_wifi_bss_view parsed = {};
	const u8 *position;
	size_t remaining;
	bool have_ssid = false;
	bool have_ds = false;
	bool have_ht = false;
	u8 ds_channel = 0;
	u8 ht_channel = 0;
	u16 frame_control;
	int ret;

	if (!bss)
		return -EINVAL;
	*bss = (struct mt6797_wifi_bss_view) {};
	if (!metadata || !metadata->frame)
		return -EINVAL;
	if (metadata->frame_length < MT6797_WIFI_BSS_FRAME_MIN_LENGTH ||
	    (metadata->band != MT6797_WIFI_RX_BAND_2GHZ &&
	     metadata->band != MT6797_WIFI_RX_BAND_5GHZ) ||
	    !metadata->channel || metadata->rcpi > MT6797_WIFI_RX_RCPI_MAX)
		return -EPROTO;

	frame_control = get_unaligned_le16(metadata->frame);
	if (frame_control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
			     IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS |
			     IEEE80211_FCTL_PROTECTED))
		return -EPROTO;
	switch (frame_control & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_BEACON:
		parsed.frame_type = MT6797_WIFI_BSS_BEACON;
		break;
	case IEEE80211_STYPE_PROBE_RESP:
		parsed.frame_type = MT6797_WIFI_BSS_PROBE_RESPONSE;
		break;
	default:
		return -EPROTO;
	}
	if (!is_valid_ether_addr(metadata->frame + 16))
		return -EPROTO;
	ether_addr_copy(parsed.bssid, metadata->frame + 16);
	parsed.capability = get_unaligned_le16(metadata->frame +
					       MT6797_WIFI_BSS_CAPABILITY_OFFSET);
	if ((parsed.capability & (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS)) ==
	    (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS))
		return -EPROTO;

	parsed.ies = metadata->frame + MT6797_WIFI_BSS_FRAME_MIN_LENGTH;
	parsed.ies_length = metadata->frame_length -
		MT6797_WIFI_BSS_FRAME_MIN_LENGTH;
	position = parsed.ies;
	remaining = parsed.ies_length;
	while (remaining) {
		size_t element_length;

		if (remaining < 2)
			return -EMSGSIZE;
		element_length = position[1];
		if (element_length > remaining - 2)
			return -EMSGSIZE;
		switch (position[0]) {
		case WLAN_EID_SSID:
			if (have_ssid || element_length > IEEE80211_MAX_SSID_LEN)
				return -EPROTO;
			have_ssid = true;
			parsed.ssid = position + 2;
			parsed.ssid_length = element_length;
			break;
		case WLAN_EID_DS_PARAMS:
			if (have_ds || element_length != 1)
				return -EPROTO;
			have_ds = true;
			ds_channel = position[2];
			break;
		case WLAN_EID_HT_OPERATION:
			if (have_ht ||
			    element_length != sizeof(struct ieee80211_ht_operation))
				return -EPROTO;
			have_ht = true;
			ht_channel = position[2];
			break;
		default:
			break;
		}
		position += element_length + 2;
		remaining -= element_length + 2;
	}
	if (!have_ssid)
		return -EPROTO;
	ret = mt6797_wifi_bss_channel_check(metadata, have_ds, ds_channel,
					    have_ht, ht_channel);
	if (ret)
		return ret;
	parsed.hidden_ssid = !parsed.ssid_length ||
		!memchr_inv(parsed.ssid, 0, parsed.ssid_length);
	*bss = parsed;
	return 0;
}
