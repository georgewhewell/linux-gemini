// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_auth.h"

static const u8 mt6797_wifi_ais_auth_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_ais_auth_peer_addr[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static void mt6797_wifi_ais_auth_handles(
	struct kunit *test, struct mt6797_wifi_ais_resources *resources,
	struct mt6797_wifi_ais_bss_handle *bss,
	struct mt6797_wifi_ais_sta_handle *sta)
{
	mt6797_wifi_ais_resources_init(resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(resources), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_reserve(resources, 3, 1,
			mt6797_wifi_ais_auth_own_addr, bss), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_reserve(resources, bss,
			mt6797_wifi_ais_auth_peer_addr, sta), 0);
}

static void mt6797_wifi_ais_auth_response(
	const struct mt6797_wifi_ais_auth_pending *pending, u8 *frame,
	u16 status)
{
	memset(frame, 0, MT6797_WIFI_AIS_AUTH_FRAME_LENGTH);
	put_unaligned_le16(IEEE80211_STYPE_AUTH, frame);
	memcpy(frame + 4, pending->own_addr, ETH_ALEN);
	memcpy(frame + 10, pending->peer_addr, ETH_ALEN);
	memcpy(frame + 16, pending->peer_addr, ETH_ALEN);
	put_unaligned_le16(2, frame + 26);
	put_unaligned_le16(status, frame + 28);
}

static void mt6797_wifi_ais_open_auth_request_test(struct kunit *test)
{
	static const u8 expected[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH] = {
		0xb0, 0x00, 0x00, 0x00,
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
		0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	};
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, frame,
			sizeof(frame), &pending), 0);
	KUNIT_EXPECT_MEMEQ(test, frame, expected, sizeof(frame));
	KUNIT_EXPECT_EQ(test, pending.generation, sta.generation);
	KUNIT_EXPECT_EQ(test, pending.bss_cookie, sta.bss_cookie);
	KUNIT_EXPECT_EQ(test, pending.sta_cookie, sta.cookie);
	KUNIT_EXPECT_MEMEQ(test, pending.own_addr, bss.own_addr, ETH_ALEN);
	KUNIT_EXPECT_MEMEQ(test, pending.peer_addr, sta.peer_addr, ETH_ALEN);
}

static void mt6797_wifi_ais_open_auth_builder_rejects_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_pending pending = { .generation = 99 };
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, frame,
			sizeof(frame) - 1, &pending), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, pending.generation, (u64)0);
	sta.bss_cookie++;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, frame,
			sizeof(frame), &pending), -EINVAL);
	KUNIT_EXPECT_EQ(test, pending.generation, (u64)0);
}

static void mt6797_wifi_ais_open_auth_response_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_response response;
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 request[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, request,
			sizeof(request), &pending), 0);
	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_EQ(test, response.status_code, (u16)0);

	put_unaligned_le16(13, frame + 28);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_EQ(test, response.status_code, (u16)13);
}

static void mt6797_wifi_ais_open_auth_ie_tail_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_response response;
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 request[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH + 7];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, request,
			sizeof(request), &pending), 0);
	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	frame[30] = 221;
	frame[31] = 3;
	frame[32] = 1;
	frame[33] = 2;
	frame[34] = 3;
	frame[35] = 127;
	frame[36] = 0;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_COMPLETE);

	frame[31] = 6;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
	frame[31] = 3;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame) - 1, &disposition, &response), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
}

static void mt6797_wifi_ais_open_auth_invalid_pending_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_response response;
	struct mt6797_wifi_ais_auth_pending pending = {
		.generation = 1,
		.bss_cookie = 2,
		.sta_cookie = 3,
		.own_addr = { 0x02, 1, 2, 3, 4, 5 },
		.peer_addr = { 0x02, 6, 7, 8, 9, 10 },
	};
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH] = {};

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(NULL, frame,
			sizeof(frame), &disposition, &response), -EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, NULL,
			sizeof(frame), &disposition, &response), -EINVAL);
	pending.generation = 0;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), -EINVAL);
	pending.generation = 1;
	memcpy(pending.peer_addr, pending.own_addr, ETH_ALEN);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), -EINVAL);
	pending.peer_addr[0] = 0x01;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), -EINVAL);
}

static void mt6797_wifi_ais_open_auth_foreign_ignored_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_response response;
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 request[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, request,
			sizeof(request), &pending), 0);
	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	frame[10] ^= 1;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_IGNORED);
	put_unaligned_le16(IEEE80211_STYPE_ASSOC_RESP, frame);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_IGNORED);
}

static void mt6797_wifi_ais_open_auth_malformed_owned_test(struct kunit *test)
{
	struct mt6797_wifi_ais_auth_response response;
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 request[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH + 1];

	mt6797_wifi_ais_auth_handles(test, &resources, &bss, &sta);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(&bss, &sta, request,
			sizeof(request), &pending), 0);
	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	put_unaligned_le16(3, frame + 26);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			MT6797_WIFI_AIS_AUTH_FRAME_LENGTH, &disposition, &response),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);

	put_unaligned_le16(2, frame + 26);
	frame[30] = 221;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			sizeof(frame), &disposition, &response), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);

	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	put_unaligned_le16(1, frame + 24);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			MT6797_WIFI_AIS_AUTH_FRAME_LENGTH, &disposition, &response),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);

	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	put_unaligned_le16(IEEE80211_STYPE_AUTH | IEEE80211_FCTL_PROTECTED,
			   frame);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			MT6797_WIFI_AIS_AUTH_FRAME_LENGTH, &disposition, &response),
		-EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);

	mt6797_wifi_ais_auth_response(&pending, frame, 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_open_auth_response_match(&pending, frame,
			25, &disposition, &response), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
}

static struct kunit_case mt6797_wifi_ais_auth_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_open_auth_request_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_builder_rejects_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_response_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_ie_tail_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_invalid_pending_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_foreign_ignored_test),
	KUNIT_CASE(mt6797_wifi_ais_open_auth_malformed_owned_test),
	{},
};

static struct kunit_suite mt6797_wifi_ais_auth_suite = {
	.name = "mt6797-wifi-ais-auth",
	.test_cases = mt6797_wifi_ais_auth_cases,
};

kunit_test_suite(mt6797_wifi_ais_auth_suite);

MODULE_LICENSE("GPL");
