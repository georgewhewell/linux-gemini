// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"
#include "rx_metadata.h"

#define MT6797_WIFI_TEST_GROUP_3	BIT(2)
#define MT6797_WIFI_TEST_GROUP_4	BIT(3)
#define MT6797_WIFI_TEST_FRAME_OFFSET	40U
#define MT6797_WIFI_TEST_IE_OFFSET	36U

static const u8 mt6797_wifi_test_bssid[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static size_t mt6797_wifi_test_packet(u8 *packet, size_t capacity,
				      u16 subtype, u8 channel, u8 rcpi)
{
	u8 *frame = packet + MT6797_WIFI_TEST_FRAME_OFFSET;
	u8 *ies = frame + MT6797_WIFI_TEST_IE_OFFSET;
	size_t length = MT6797_WIFI_TEST_FRAME_OFFSET +
		MT6797_WIFI_TEST_IE_OFFSET + 9;

	if (capacity < length)
		return 0;
	memset(packet, 0, capacity);
	put_unaligned_le16(length, packet);
	put_unaligned_le16(0xe001 | (MT6797_WIFI_TEST_GROUP_3 << 9),
			   packet + 2);
	packet[5] = channel;
	packet[6] = 24;
	put_unaligned_le32((u32)rcpi << 8, packet + 16 + 8);
	put_unaligned_le16(subtype, frame);
	memcpy(frame + 10, mt6797_wifi_test_bssid, ETH_ALEN);
	memcpy(frame + 16, mt6797_wifi_test_bssid, ETH_ALEN);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + 34);
	ies[0] = WLAN_EID_SSID;
	ies[1] = 4;
	memcpy(ies + 2, "test", 4);
	ies[6] = WLAN_EID_DS_PARAMS;
	ies[7] = 1;
	ies[8] = channel;
	return length;
}

static void mt6797_wifi_rx_metadata_beacon_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view bss;
	u8 packet[128];
	size_t length;

	length = mt6797_wifi_test_packet(packet, sizeof(packet),
					 IEEE80211_STYPE_BEACON, 6, 100);
	KUNIT_ASSERT_NE(test, length, (size_t)0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), 0);
	KUNIT_EXPECT_PTR_EQ(test, metadata.frame,
			    packet + MT6797_WIFI_TEST_FRAME_OFFSET);
	KUNIT_EXPECT_EQ(test, metadata.frame_length,
			length - MT6797_WIFI_TEST_FRAME_OFFSET);
	KUNIT_EXPECT_EQ(test, metadata.band,
			(enum mt6797_wifi_rx_band)MT6797_WIFI_RX_BAND_2GHZ);
	KUNIT_EXPECT_EQ(test, metadata.channel, (u8)6);
	KUNIT_EXPECT_EQ(test, metadata.rcpi, (u8)100);
	KUNIT_EXPECT_EQ(test, metadata.signal_mbm, (s32)-6000);

	KUNIT_ASSERT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), 0);
	KUNIT_EXPECT_EQ(test, bss.frame_type,
			(enum mt6797_wifi_bss_frame_type)MT6797_WIFI_BSS_BEACON);
	KUNIT_EXPECT_MEMEQ(test, bss.bssid, mt6797_wifi_test_bssid,
			   ETH_ALEN);
	KUNIT_EXPECT_EQ(test, bss.capability, (u16)WLAN_CAPABILITY_ESS);
	KUNIT_EXPECT_EQ(test, bss.ssid_length, (size_t)4);
	KUNIT_EXPECT_MEMEQ(test, bss.ssid, "test", 4);
	KUNIT_EXPECT_FALSE(test, bss.hidden_ssid);
	KUNIT_EXPECT_PTR_EQ(test, bss.ies,
			    metadata.frame + MT6797_WIFI_TEST_IE_OFFSET);
	KUNIT_EXPECT_EQ(test, bss.ies_length, (size_t)9);
}

static void mt6797_wifi_rx_metadata_group_order_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view bss;
	u8 packet[160] = {};
	u8 *frame = packet + 82;
	u8 *ies = frame + MT6797_WIFI_TEST_IE_OFFSET;
	size_t length = 82 + MT6797_WIFI_TEST_IE_OFFSET + 26;

	put_unaligned_le16(length, packet);
	put_unaligned_le16(0xe001 | (0xf << 9), packet + 2);
	packet[5] = 36;
	packet[6] = BIT(6) | 24;
	/* Group order is 4, 1, 2, 3; vector 2 starts eight bytes into group 3. */
	put_unaligned_le32(91U << 8, packet + 56 + 8);
	put_unaligned_le16(IEEE80211_STYPE_PROBE_RESP, frame);
	memcpy(frame + 10, mt6797_wifi_test_bssid, ETH_ALEN);
	memcpy(frame + 16, mt6797_wifi_test_bssid, ETH_ALEN);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + 34);
	ies[0] = WLAN_EID_SSID;
	ies[1] = 0;
	ies[2] = WLAN_EID_HT_OPERATION;
	ies[3] = sizeof(struct ieee80211_ht_operation);
	ies[4] = 36;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), 0);
	KUNIT_EXPECT_PTR_EQ(test, metadata.frame, frame);
	KUNIT_EXPECT_EQ(test, metadata.band,
			(enum mt6797_wifi_rx_band)MT6797_WIFI_RX_BAND_5GHZ);
	KUNIT_EXPECT_EQ(test, metadata.channel, (u8)36);
	KUNIT_EXPECT_EQ(test, metadata.rcpi, (u8)91);
	KUNIT_EXPECT_EQ(test, metadata.signal_mbm, (s32)-6500);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), 0);
	KUNIT_EXPECT_EQ(test, bss.frame_type,
			(enum mt6797_wifi_bss_frame_type)
			MT6797_WIFI_BSS_PROBE_RESPONSE);
	KUNIT_EXPECT_TRUE(test, bss.hidden_ssid);
}

static void mt6797_wifi_rx_metadata_rejects_descriptor_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata = {
		.frame = (const u8 *)1,
	};
	u8 packet[128];
	size_t length;

	length = mt6797_wifi_test_packet(packet, sizeof(packet),
					 IEEE80211_STYPE_BEACON, 6, 100);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(NULL, length, &metadata), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, metadata.frame, NULL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, 15, &metadata), -EMSGSIZE);

	put_unaligned_le16(0xe000 | (MT6797_WIFI_TEST_GROUP_3 << 9),
			   packet + 2);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EPROTO);
	put_unaligned_le16(0xe001, packet + 2);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -ENODATA);
	put_unaligned_le16(0xe001 | (MT6797_WIFI_TEST_GROUP_3 << 9),
			   packet + 2);
	packet[6] = BIT(7) | 24;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EPROTO);
	packet[6] = 23;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EPROTO);
	packet[6] = 24;
	put_unaligned_le16(BIT(1), packet + 10);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EBADMSG);
	put_unaligned_le16(0, packet + 10);
	packet[5] = 0;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EPROTO);
	packet[5] = 6;
	put_unaligned_le32(221U << 8, packet + 16 + 8);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -ERANGE);
	put_unaligned_le32(100U << 8, packet + 16 + 8);
	put_unaligned_le16(length + 1, packet);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), -EMSGSIZE);
	KUNIT_EXPECT_PTR_EQ(test, metadata.frame, NULL);
}

static void mt6797_wifi_bss_rejects_frame_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view bss;
	u8 packet[128];
	size_t length;
	u8 *frame;
	struct mt6797_wifi_rx_metadata invalid = {};

	length = mt6797_wifi_test_packet(packet, sizeof(packet),
					 IEEE80211_STYPE_BEACON, 6, 100);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), 0);
	frame = (u8 *)metadata.frame;
	bss.ies = (const u8 *)1;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&invalid, &bss), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, bss.ies, NULL);

	put_unaligned_le16(IEEE80211_STYPE_AUTH, frame);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
	put_unaligned_le16(IEEE80211_STYPE_BEACON, frame);
	frame[16] = 0x01;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
	memcpy(frame + 16, mt6797_wifi_test_bssid, ETH_ALEN);
	put_unaligned_le16(WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS,
			   frame + 34);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + 34);
	frame[MT6797_WIFI_TEST_IE_OFFSET + 6] = WLAN_EID_SSID;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
	KUNIT_EXPECT_PTR_EQ(test, bss.ies, NULL);
}

static void mt6797_wifi_bss_rejects_ie_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view bss;
	u8 packet[128];
	size_t length;
	u8 *ies;

	length = mt6797_wifi_test_packet(packet, sizeof(packet),
					 IEEE80211_STYPE_BEACON, 6, 100);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_rx_metadata_decode(packet, length, &metadata), 0);
	ies = (u8 *)metadata.frame + MT6797_WIFI_TEST_IE_OFFSET;

	ies[7] = 2;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EMSGSIZE);
	ies[7] = 1;
	ies[8] = 7;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
	ies[8] = 6;
	ies[0] = WLAN_EID_SUPP_RATES;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
}

static void mt6797_wifi_bss_rejects_channel_ie_test(struct kunit *test)
{
	struct mt6797_wifi_rx_metadata metadata = {
		.band = MT6797_WIFI_RX_BAND_2GHZ,
		.channel = 6,
		.rcpi = 100,
	};
	struct mt6797_wifi_bss_view bss;
	u8 frame[96] = {};
	u8 *ies = frame + MT6797_WIFI_TEST_IE_OFFSET;

	metadata.frame = frame;
	metadata.frame_length = sizeof(frame);
	put_unaligned_le16(IEEE80211_STYPE_BEACON, frame);
	memcpy(frame + 16, mt6797_wifi_test_bssid, ETH_ALEN);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + 34);
	ies[0] = WLAN_EID_SSID;
	ies[1] = 33;
	metadata.frame_length = MT6797_WIFI_TEST_IE_OFFSET + 35;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);

	memset(ies, 0, sizeof(frame) - MT6797_WIFI_TEST_IE_OFFSET);
	ies[0] = WLAN_EID_SSID;
	ies[1] = 0;
	ies[2] = WLAN_EID_DS_PARAMS;
	ies[3] = 1;
	ies[4] = 6;
	ies[5] = WLAN_EID_HT_OPERATION;
	ies[6] = sizeof(struct ieee80211_ht_operation);
	ies[7] = 11;
	metadata.frame_length = MT6797_WIFI_TEST_IE_OFFSET + 29;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);

	metadata.band = MT6797_WIFI_RX_BAND_5GHZ;
	metadata.channel = 36;
	ies[4] = 36;
	ies[7] = 36;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_bss_parse(&metadata, &bss), -EPROTO);
}

static struct kunit_case mt6797_wifi_rx_metadata_cases[] = {
	KUNIT_CASE(mt6797_wifi_rx_metadata_beacon_test),
	KUNIT_CASE(mt6797_wifi_rx_metadata_group_order_test),
	KUNIT_CASE(mt6797_wifi_rx_metadata_rejects_descriptor_test),
	KUNIT_CASE(mt6797_wifi_bss_rejects_frame_test),
	KUNIT_CASE(mt6797_wifi_bss_rejects_ie_test),
	KUNIT_CASE(mt6797_wifi_bss_rejects_channel_ie_test),
	{}
};

static struct kunit_suite mt6797_wifi_rx_metadata_suite = {
	.name = "mt6797-wifi-rx-metadata",
	.test_cases = mt6797_wifi_rx_metadata_cases,
};

kunit_test_suite(mt6797_wifi_rx_metadata_suite);

MODULE_LICENSE("GPL");
