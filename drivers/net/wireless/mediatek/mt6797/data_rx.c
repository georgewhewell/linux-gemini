// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 data RX: nic_rx.h group4/1 layout, mt7603/mac.c PN byte order. */
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
	const u8 *transmitter, *sequence, *qos_control, *pn;
	const u8 *llc;
	size_t header_length;
	u16 type, status, control, qos;
	u8 groups, cipher;
	int ret;

	if (!data)
		return -EINVAL;
	*data = (struct mt6797_wifi_data_rx) {};
	ret = mt6797_wifi_rx_parse(packet, available, &frame);
	if (ret)
		return ret;
	if (frame.kind != MT6797_WIFI_RX_ETHERNET &&
	    frame.kind != MT6797_WIFI_RX_NATIVE_DATA)
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
	/* rx_parse checked all group and payload bounds. Group4 comes first. */
	pn = packet + MT6797_WIFI_RX_DESCRIPTOR_LENGTH +
		((groups & BIT(3)) ? 16 : 0);
	if (frame.header_translated) {
		const u8 *group4 = packet + MT6797_WIFI_RX_DESCRIPTOR_LENGTH;

		if (frame.header_length != ETH_HLEN || !(groups & BIT(3)))
			return -EPROTO;
		control = get_unaligned_le16(group4);
		transmitter = group4 + 2;
		sequence = group4 + 8;
		qos_control = group4 + 10;
		memcpy(candidate.ethernet, frame.payload, ETH_HLEN);
		candidate.payload = frame.payload + ETH_HLEN;
		candidate.length = frame.payload_length;
	} else {
		if (frame.payload_length < 24)
			return -EPROTO;
		control = get_unaligned_le16(frame.payload);
		header_length = (control & IEEE80211_STYPE_QOS_DATA) ? 26 : 24;
		if (frame.header_length != header_length ||
		    frame.payload_length < header_length + 8)
			return -EPROTO;
		transmitter = frame.payload + 10;
		sequence = frame.payload + 22;
		qos_control = frame.payload + 24;
		/* GEN3 strips the CCMP IV/MIC on decrypted, unfragmented RX.
		 * Only complete RFC1042/bridge-tunnel SNAP Ethernet-II is admitted.
		 * Never interpret an arbitrary LLC or still-encrypted body as EAPOL.
		 */
		llc = frame.payload + header_length;
		if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03 ||
		    llc[3] || llc[4] || (llc[5] && llc[5] != 0xf8) ||
		    (!llc[5] && (get_unaligned_be16(llc + 6) == ETH_P_AARP ||
				get_unaligned_be16(llc + 6) == ETH_P_IPX)))
			return -EOPNOTSUPP;
		memcpy(candidate.ethernet, frame.payload + 4, ETH_ALEN);
		memcpy(candidate.ethernet + ETH_ALEN, frame.payload + 16, ETH_ALEN);
		memcpy(candidate.ethernet + 12, llc + 6, 2);
		candidate.payload = llc + 8;
		candidate.length = ETH_HLEN + frame.payload_length - header_length - 8;
	}
	if ((control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE |
		       IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) !=
	    (IEEE80211_FTYPE_DATA | IEEE80211_FCTL_FROMDS) ||
	    (control & (IEEE80211_FCTL_MOREFRAGS | IEEE80211_FCTL_ORDER)) ||
	    (get_unaligned_le16(sequence) & IEEE80211_SCTL_FRAG))
		return -EPROTO;
	if ((control & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_DATA &&
	    (control & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_QOS_DATA)
		return -EOPNOTSUPP;
	if (control & IEEE80211_STYPE_QOS_DATA) {
		qos = get_unaligned_le16(qos_control);
		if (qos & IEEE80211_QOS_CTL_A_MSDU_PRESENT)
			return -EOPNOTSUPP;
		candidate.tid = qos & IEEE80211_QOS_CTL_TID_MASK;
		if (candidate.tid != (packet[9] & 15))
			return -EPROTO;
	}
	if (!is_valid_ether_addr(transmitter) ||
	    !is_valid_ether_addr(candidate.ethernet + ETH_ALEN))
		return -EPROTO;
	/* Ethernet-II only. An encapsulated LLC frame cannot bypass port policy. */
	if (get_unaligned_be16(candidate.ethernet + 12) < ETH_P_802_3_MIN)
		return -EOPNOTSUPP;
	candidate.eapol = get_unaligned_be16(candidate.ethernet + 12) == ETH_P_PAE;
	candidate.multicast = is_multicast_ether_addr(candidate.ethernet);
	candidate.protected = control & IEEE80211_FCTL_PROTECTED;
	cipher = packet[9] >> 4;
	if (candidate.protected) {
		if (cipher != 4 || (status & BIT(2)) || !(groups & BIT(0)))
			return -EBADMSG;
		/* Group1 PN0 is the least-significant byte. */
		candidate.pn = mt6797_wifi_data_rx_pn(pn);
	} else if (cipher || ((status & BIT(2)) && !candidate.eapol)) {
		return -EBADMSG;
	}
	memcpy(candidate.transmitter, transmitter, sizeof(candidate.transmitter));
	candidate.bss_index = packet[7] >> 2;
	candidate.wlan_index = frame.wlan_index;
	candidate.key_index = packet[4] >> 6;
	*data = candidate;
	return 0;
}
