// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 translated RX: nic_rx.h group4/1 layout, mt7603/mac.c PN byte order. */
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/unaligned.h>
#include "data_rx.h"
#include "protocol.h"

u64 mt6797_wifi_data_rx_pn(const u8 sequence[6])
{
	u64 pn = 0;
	unsigned int i;

	for (i = 0; i < 6; i++)
		pn |= (u64)sequence[i] << (8 * i);
	return pn;
}

int mt6797_wifi_data_rx_replay(u64 *last, u64 pn)
{
	if (!last || pn > 0xffffffffffffULL)
		return -EINVAL;
	if (pn <= *last)
		return -EALREADY;
	*last = pn;
	return 0;
}

int mt6797_wifi_data_rx_decode(const u8 *packet, size_t available,
	struct mt6797_wifi_data_rx *data)
{
	struct mt6797_wifi_rx_view frame;
	struct mt6797_wifi_data_rx candidate = {};
	const u8 *group4;
	u16 type, status, control, qos;
	u8 groups, cipher;
	int ret;

	if (!data)
		return -EINVAL;
	*data = (struct mt6797_wifi_data_rx) {};
	ret = mt6797_wifi_rx_parse(packet, available, &frame);
	if (ret)
		return ret;
	if (frame.kind != MT6797_WIFI_RX_ETHERNET || frame.header_length != ETH_HLEN)
		return -EOPNOTSUPP;
	type = get_unaligned_le16(packet + 2);
	/* RX_DATA=2. Lower bits contain the EtherType offset/checksum flags. */
	if ((type >> 13) != 2 || (packet[7] & 3))
		return -EOPNOTSUPP; /* No deaggregated A-MSDU fragments. */
	status = get_unaligned_le16(packet + 10);
	/* Bits 0..12 are class/error/fragment/null; bit13 means non-data.
	 * Cipher mismatch is permitted only for plaintext EAPOL below.
	 * Bits14/15 use inverse AMPDU flags and are not error indications.
	 */
	if (status & (0x3fffU & ~BIT(2)))
		return -EBADMSG;
	groups = (type >> 9) & 15;
	if (!(groups & BIT(3)))
		return -EPROTO; /* Cannot establish original transmitter/header. */
	/* rx_parse checked the full group layout before exposing the payload. */
	group4 = packet + MT6797_WIFI_RX_DESCRIPTOR_LENGTH;
	control = get_unaligned_le16(group4);
	if ((control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
		       IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) !=
	    (IEEE80211_FTYPE_DATA | IEEE80211_FCTL_FROMDS) ||
	    (control & (IEEE80211_FCTL_MOREFRAGS | IEEE80211_FCTL_ORDER)) ||
	    (get_unaligned_le16(group4 + 8) & IEEE80211_SCTL_FRAG))
		return -EPROTO;
	if ((control & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_DATA &&
	    (control & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_QOS_DATA)
		return -EOPNOTSUPP;
	if (control & IEEE80211_STYPE_QOS_DATA) {
		qos = get_unaligned_le16(group4 + 10);
		if (qos & IEEE80211_QOS_CTL_A_MSDU_PRESENT)
			return -EOPNOTSUPP;
		candidate.tid = qos & IEEE80211_QOS_CTL_TID_MASK;
		if (candidate.tid != (packet[9] & 15))
			return -EPROTO;
	}
	if (!is_valid_ether_addr(group4 + 2) || !is_valid_ether_addr(frame.payload + ETH_ALEN))
		return -EPROTO;
	candidate.ethernet = frame.payload;
	candidate.length = frame.payload_length;
	/* Ethernet-II only. An encapsulated LLC frame cannot bypass port policy. */
	if (get_unaligned_be16(frame.payload + 12) < ETH_P_802_3_MIN)
		return -EOPNOTSUPP;
	candidate.eapol = get_unaligned_be16(frame.payload + 12) == ETH_P_PAE;
	candidate.multicast = is_multicast_ether_addr(frame.payload);
	candidate.protected = control & IEEE80211_FCTL_PROTECTED;
	cipher = packet[9] >> 4;
	if (candidate.protected) {
		if (cipher != 4 || (status & BIT(2)) || !(groups & BIT(0)))
			return -EBADMSG;
		/* Group4 precedes group1. PN0 is the least-significant byte. */
		candidate.pn = mt6797_wifi_data_rx_pn(group4 + 16);
	} else if (cipher || ((status & BIT(2)) && !candidate.eapol)) {
		return -EBADMSG;
	}
	memcpy(candidate.transmitter, group4 + 2, sizeof(candidate.transmitter));
	candidate.bss_index = packet[7] >> 2;
	candidate.wlan_index = frame.wlan_index;
	candidate.key_index = packet[4] >> 6;
	*data = candidate;
	return 0;
}
