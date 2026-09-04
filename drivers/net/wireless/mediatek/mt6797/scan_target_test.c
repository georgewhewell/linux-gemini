// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"
#include "scan_target.h"

#define MT6797_WIFI_SCAN_TARGET_TEST_GROUP3	BIT(2)
#define MT6797_WIFI_SCAN_TARGET_TEST_FRAME	40U
#define MT6797_WIFI_SCAN_TARGET_TEST_IES		36U
#define MT6797_WIFI_SCAN_TARGET_TEST_MAX_AGE	100U

static const u8 mt6797_wifi_scan_target_test_bssid[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_scan_target_test_foreign[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static size_t mt6797_wifi_scan_target_test_packet(
	u8 *packet, size_t capacity, const u8 bssid[ETH_ALEN],
	const char *ssid, size_t ssid_length, u8 channel, u8 rcpi,
	u16 capability, u16 subtype)
{
	u8 *frame = packet + MT6797_WIFI_SCAN_TARGET_TEST_FRAME;
	u8 *ies = frame + MT6797_WIFI_SCAN_TARGET_TEST_IES;
	size_t channel_ie_length;
	size_t length;

	channel_ie_length = channel <= 14 ? 3 :
		2 + sizeof(struct ieee80211_ht_operation);
	length = MT6797_WIFI_SCAN_TARGET_TEST_FRAME +
		MT6797_WIFI_SCAN_TARGET_TEST_IES + 2 + ssid_length +
		channel_ie_length;
	if (capacity < length || ssid_length > IEEE80211_MAX_SSID_LEN)
		return 0;
	memset(packet, 0, capacity);
	put_unaligned_le16(length, packet);
	put_unaligned_le16(0xe001 |
		(MT6797_WIFI_SCAN_TARGET_TEST_GROUP3 << 9), packet + 2);
	packet[5] = channel;
	packet[6] = 24;
	put_unaligned_le32((u32)rcpi << 8, packet + 24);
	put_unaligned_le16(subtype, frame);
	memcpy(frame + 10, bssid, ETH_ALEN);
	memcpy(frame + 16, bssid, ETH_ALEN);
	put_unaligned_le16(capability, frame + 34);
	ies[0] = WLAN_EID_SSID;
	ies[1] = ssid_length;
	if (ssid_length)
		memcpy(ies + 2, ssid, ssid_length);
	ies += 2 + ssid_length;
	if (channel <= 14) {
		ies[0] = WLAN_EID_DS_PARAMS;
		ies[1] = 1;
		ies[2] = channel;
	} else {
		ies[0] = WLAN_EID_HT_OPERATION;
		ies[1] = sizeof(struct ieee80211_ht_operation);
		ies[2] = channel;
	}
	return length;
}

static void mt6797_wifi_scan_target_test_key(
	struct mt6797_wifi_scan_target_key *key)
{
	memset(key, 0, sizeof(*key));
	memcpy(key->bssid, mt6797_wifi_scan_target_test_bssid, ETH_ALEN);
	memcpy(key->ssid, "test", 4);
	key->ssid_length = 4;
}

static void mt6797_wifi_scan_target_test_open(
	struct kunit *test, struct mt6797_wifi_scan_target_owner *owner)
{
	struct mt6797_wifi_scan_target_key key;

	mt6797_wifi_scan_target_test_key(&key);
	mt6797_wifi_scan_target_init(owner);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_open(owner, &key,
			MT6797_WIFI_SCAN_TARGET_TEST_MAX_AGE), 0);
}

static void mt6797_wifi_scan_target_owned_copy_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target target;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY,
		IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_NE(test, length, (size_t)0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 10, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_ADDED);
	memset(packet, 0xa5, sizeof(packet));
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 10, &target), 0);
	KUNIT_EXPECT_EQ(test, target.generation, 1ULL);
	KUNIT_EXPECT_EQ(test, target.cookie, 1ULL);
	KUNIT_EXPECT_MEMEQ(test, target.bssid,
			   mt6797_wifi_scan_target_test_bssid, ETH_ALEN);
	KUNIT_EXPECT_MEMEQ(test, target.ssid, "test", 4);
	KUNIT_EXPECT_EQ(test, target.ssid_length, (u8)4);
	KUNIT_EXPECT_EQ(test, target.band, (u8)MT6797_WIFI_RX_BAND_2GHZ);
	KUNIT_EXPECT_EQ(test, target.channel, (u8)6);
	KUNIT_EXPECT_EQ(test, target.rcpi, (u8)100);
	KUNIT_EXPECT_EQ(test, target.signal_mbm, (s32)-6000);
	KUNIT_EXPECT_EQ(test, target.capability,
			(u16)(WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY));
	KUNIT_EXPECT_FALSE(test, target.last_frame_hidden);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_validate(&owner, 10, &target), 0);
	target.channel = 11;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_validate(&owner, 10, &target), -ESTALE);
}

static void mt6797_wifi_scan_target_duplicate_update_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target first;
	struct mt6797_wifi_scan_target second;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 20, &update), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 20, &first), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 20, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_DUPLICATE);
	KUNIT_EXPECT_EQ(test, owner.selected.cookie, first.cookie);

	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 36, 120,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_PROBE_RESP);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 21, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_UPDATED);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 21, &second), 0);
	KUNIT_EXPECT_EQ(test, second.cookie, first.cookie + 1);
	KUNIT_EXPECT_EQ(test, second.band, (u8)MT6797_WIFI_RX_BAND_5GHZ);
	KUNIT_EXPECT_EQ(test, second.channel, (u8)36);
	KUNIT_EXPECT_EQ(test, second.rcpi, (u8)120);
	KUNIT_EXPECT_EQ(test, second.frame_type,
			(u8)MT6797_WIFI_BSS_PROBE_RESPONSE);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_validate(&owner, 21, &first), -ESTALE);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 120, &second), 0);
}

static void mt6797_wifi_scan_target_filter_hidden_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target target;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_foreign, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 1, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_IGNORED);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "nope", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 2, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_IGNORED);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, NULL, 0, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 3, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_IGNORED);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 3, &target), -ENODATA);

	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_PROBE_RESP);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 4, &update), 0);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, NULL, 0, 6, 110,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 5, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_UPDATED);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 5, &target), 0);
	KUNIT_EXPECT_MEMEQ(test, target.ssid, "test", 4);
	KUNIT_EXPECT_TRUE(test, target.last_frame_hidden);
}

static void mt6797_wifi_scan_target_expiry_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target target;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;
	bool expired;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 10, &update), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 109, &target), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 110, &target), -ESTALE);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_expire(&owner, 110, &expired), 0);
	KUNIT_EXPECT_FALSE(test, expired);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 110, &target), -ENODATA);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 111, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_ADDED);
	KUNIT_EXPECT_EQ(test, owner.selected.cookie, 2ULL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_expire(&owner, 211, &expired), 0);
	KUNIT_EXPECT_TRUE(test, expired);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_validate(&owner, 211, &target), -ESTALE);
}

static void mt6797_wifi_scan_target_generation_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target_key key;
	struct mt6797_wifi_scan_target old;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 1, &update), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_scan_target_snapshot(&owner, 1, &old), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_close(&owner), 0);
	mt6797_wifi_scan_target_test_key(&key);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_open(&owner, &key,
			MT6797_WIFI_SCAN_TARGET_TEST_MAX_AGE), 0);
	KUNIT_EXPECT_EQ(test, owner.generation, 2ULL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_scan_target_validate(&owner, 2, &old), -ESTALE);
}

static void mt6797_wifi_scan_target_reject_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target_key key;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_init(&owner);
	mt6797_wifi_scan_target_test_key(&key);
	key.bssid[0] = 0x01;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_open(&owner, &key, 100),
			-EINVAL);
	mt6797_wifi_scan_target_test_key(&key);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_open(&owner, &key, 100), 0);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100, 0,
		IEEE80211_STYPE_BEACON);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 1, &update), 0);
	KUNIT_EXPECT_EQ(test, update, MT6797_WIFI_SCAN_TARGET_IGNORED);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	packet[MT6797_WIFI_SCAN_TARGET_TEST_FRAME +
	       MT6797_WIFI_SCAN_TARGET_TEST_IES + 8] = 11;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 2, &update), -EPROTO);
}

static void mt6797_wifi_scan_target_terminal_test(struct kunit *test)
{
	struct mt6797_wifi_scan_target_owner owner;
	struct mt6797_wifi_scan_target_key key;
	enum mt6797_wifi_scan_target_update update;
	u8 packet[160];
	size_t length;

	mt6797_wifi_scan_target_test_open(test, &owner);
	length = mt6797_wifi_scan_target_test_packet(packet, sizeof(packet),
		mt6797_wifi_scan_target_test_bssid, "test", 4, 6, 100,
		WLAN_CAPABILITY_ESS, IEEE80211_STYPE_BEACON);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 10, &update), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 9, &update), -ERANGE);
	KUNIT_EXPECT_EQ(test, owner.state, MT6797_WIFI_SCAN_TARGET_TERMINAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_close(&owner), -ESHUTDOWN);

	mt6797_wifi_scan_target_init(&owner);
	owner.generation = U64_MAX;
	mt6797_wifi_scan_target_test_key(&key);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_open(&owner, &key, 100),
			-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, owner.state, MT6797_WIFI_SCAN_TARGET_TERMINAL);

	mt6797_wifi_scan_target_test_open(test, &owner);
	owner.next_cookie = U64_MAX;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_scan_target_observe(&owner, packet,
					 length, 1, &update), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, owner.state, MT6797_WIFI_SCAN_TARGET_TERMINAL);
}

static struct kunit_case mt6797_wifi_scan_target_cases[] = {
	KUNIT_CASE(mt6797_wifi_scan_target_owned_copy_test),
	KUNIT_CASE(mt6797_wifi_scan_target_duplicate_update_test),
	KUNIT_CASE(mt6797_wifi_scan_target_filter_hidden_test),
	KUNIT_CASE(mt6797_wifi_scan_target_expiry_test),
	KUNIT_CASE(mt6797_wifi_scan_target_generation_test),
	KUNIT_CASE(mt6797_wifi_scan_target_reject_test),
	KUNIT_CASE(mt6797_wifi_scan_target_terminal_test),
	{}
};

static struct kunit_suite mt6797_wifi_scan_target_suite = {
	.name = "mt6797-wifi-scan-target",
	.test_cases = mt6797_wifi_scan_target_cases,
};

kunit_test_suite(mt6797_wifi_scan_target_suite);

MODULE_LICENSE("GPL");
