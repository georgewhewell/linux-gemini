// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_auth.h"
#include "ais_mgmt_tx.h"

static const u8 mt6797_wifi_ais_mgmt_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_ais_mgmt_peer_addr[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static void mt6797_wifi_ais_mgmt_handles(
	struct kunit *test, struct mt6797_wifi_ais_resources *resources,
	struct mt6797_wifi_ais_bss_handle *bss,
	struct mt6797_wifi_ais_sta_handle *sta)
{
	mt6797_wifi_ais_resources_init(resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(resources), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_reserve(resources, 3, 1,
			mt6797_wifi_ais_mgmt_own_addr, bss), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_reserve(resources, bss,
			mt6797_wifi_ais_mgmt_peer_addr, sta), 0);
}

static void mt6797_wifi_ais_mgmt_auth_frame(
	struct kunit *test, const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta, u8 *frame)
{
	struct mt6797_wifi_ais_auth_pending pending;

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_open_auth_request_build(bss, sta, frame,
			MT6797_WIFI_AIS_AUTH_FRAME_LENGTH, &pending), 0);
}

static void mt6797_wifi_ais_mgmt_owner_open(
	struct kunit *test, struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	struct mt6797_wifi_ais_mgmt_pid_lease *lease)
{
	mt6797_wifi_ais_mgmt_pid_init(owner);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(owner, bss, sta), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(owner, lease), 0);
}

static void mt6797_wifi_ais_mgmt_pid_lifecycle_test(struct kunit *test)
{
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease first;
	struct mt6797_wifi_ais_mgmt_pid_lease second;
	struct mt6797_wifi_ais_mgmt_pid_lease wrong;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_pid_init(&owner);
	memset(&first, 0xa5, sizeof(first));
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(&owner, &first), -ENOTCONN);
	KUNIT_EXPECT_EQ(test, first.cookie, (u64)0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(&owner, &bss, &sta), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(&owner, &first), 0);
	KUNIT_EXPECT_EQ(test, first.pid, (u8)1);
	KUNIT_EXPECT_EQ(test, first.wlan_index, sta.pairwise.index);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(&owner, &second), -EBUSY);
	KUNIT_EXPECT_EQ(test, second.cookie, (u64)0);

	wrong = first;
	wrong.cookie++;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_release(&owner, &wrong), -ESTALE);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_release(&owner, &first), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_release(&owner, &first), -ESTALE);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_mgmt_pid_close(&owner), 0);

	/* Do not immediately recycle a wire PID across normal sessions. */
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(&owner, &bss, &sta), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(&owner, &second), 0);
	KUNIT_EXPECT_EQ(test, second.pid, (u8)2);
	KUNIT_EXPECT_NE(test, second.owner_generation,
		first.owner_generation);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_mgmt_pid_close(&owner), -EBUSY);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&owner), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&owner), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_release(&owner, &second), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_mgmt_pid_close(&owner),
		-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(&owner, &bss, &sta), -ESHUTDOWN);
}

static void mt6797_wifi_ais_mgmt_pid_overflow_test(struct kunit *test)
{
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_pid_init(&owner);
	owner.generation = U64_MAX;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(&owner, &bss, &sta), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, owner.state,
		(u8)MT6797_WIFI_AIS_MGMT_PID_TERMINAL);

	mt6797_wifi_ais_mgmt_pid_init(&owner);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_open(&owner, &bss, &sta), 0);
	owner.next_cookie = U64_MAX;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_acquire(&owner, &lease), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, lease.cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, owner.state,
		(u8)MT6797_WIFI_AIS_MGMT_PID_TERMINAL);
}

static void mt6797_wifi_ais_mgmt_descriptor_test(struct kunit *test)
{
	static const u8 expected_descriptor[MT6797_WIFI_TX_DESCRIPTOR_LENGTH] = {
		0x3a, 0x00, 0x1a, 0x88, 0x01, 0xcc, 0x00, 0x04,
		0x0b, 0x00, 0x00, 0x80, 0x01, 0xf0, 0x23, 0x81,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
		0x01, 0x00, 0x2c, 0x01,
	};
	struct mt6797_wifi_ais_mgmt_tx_params params = {
		.sequence_number = 0x123,
		.fixed_rate = 0x4b,
	};
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 packet[64];
	size_t transfer_length = 99;

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_owner_open(test, &owner, &bss, &sta, &lease);
	mt6797_wifi_ais_mgmt_auth_frame(test, &bss, &sta, frame);
	memset(packet, 0xa5, sizeof(packet));
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)60);
	KUNIT_EXPECT_MEMEQ(test, packet, expected_descriptor,
		sizeof(expected_descriptor));
	KUNIT_EXPECT_MEMEQ(test, packet + MT6797_WIFI_TX_DESCRIPTOR_LENGTH,
		frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, packet[58], (u8)0);
	KUNIT_EXPECT_EQ(test, packet[59], (u8)0);
}

static void mt6797_wifi_ais_mgmt_descriptor_rejects_test(struct kunit *test)
{
	struct mt6797_wifi_ais_mgmt_tx_params params = {
		.sequence_number = 1,
		.fixed_rate = 1,
	};
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	struct mt6797_wifi_ais_mgmt_pid_lease stale;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	u8 frame[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH];
	u8 packet[64];
	size_t transfer_length;

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_owner_open(test, &owner, &bss, &sta, &lease);
	mt6797_wifi_ais_mgmt_auth_frame(test, &bss, &sta, frame);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, 59,
			&transfer_length), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)0);

	stale = lease;
	stale.cookie++;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &stale, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -ESTALE);
	frame[4] ^= 1;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EINVAL);
	frame[4] ^= 1;
	put_unaligned_le16(IEEE80211_STYPE_DEAUTH, frame);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EOPNOTSUPP);
	put_unaligned_le16(IEEE80211_STYPE_AUTH | IEEE80211_FCTL_PROTECTED,
		frame);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EOPNOTSUPP);
	put_unaligned_le16(IEEE80211_STYPE_AUTH, frame);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, 23, packet, sizeof(packet),
			&transfer_length), -EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, SIZE_MAX, packet, sizeof(packet),
			&transfer_length), -EOVERFLOW);
	bss.own_mac_index = 64;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EINVAL);
	bss.own_mac_index = 1;
	params.fixed_rate = 0;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 26), (u16)0);
	params.fixed_rate = 0x1000;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EINVAL);
	params.fixed_rate = 1;
	params.sequence_number = 0x1000;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -EINVAL);

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&owner), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_build(&owner, &lease, &bss, &sta,
			&params, frame, sizeof(frame), packet, sizeof(packet),
			&transfer_length), -ESHUTDOWN);
}

static void mt6797_wifi_ais_mgmt_event_fill(u8 *payload, u8 pid, u8 wlan)
{
	memset(payload, 0, 16);
	payload[0] = pid;
	payload[1] = 7;
	put_unaligned_le16(0x123, payload + 2);
	payload[4] = wlan;
	payload[5] = 3;
	put_unaligned_le16(0x456, payload + 6);
	payload[8] = 0xa5;
}

static void mt6797_wifi_ais_mgmt_event_test(struct kunit *test)
{
	struct mt6797_wifi_ais_mgmt_tx_status status;
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 payload[16];
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH,
		.id = 0x0f,
	};

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_owner_open(test, &owner, &bss, &sta, &lease);
	mt6797_wifi_ais_mgmt_event_fill(payload, lease.pid, lease.wlan_index);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_EQ(test, status.status, (u8)7);
	KUNIT_EXPECT_EQ(test, status.sequence_number, (u16)0x123);
	KUNIT_EXPECT_EQ(test, status.tx_count, (u8)3);
	KUNIT_EXPECT_EQ(test, status.rate, (u16)0x456);
	KUNIT_EXPECT_EQ(test, status.flags, (u8)0xa5);
	KUNIT_EXPECT_TRUE(test, status.advanced);

	payload[8] = 0;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_COMPLETE);
	KUNIT_EXPECT_FALSE(test, status.advanced);
	KUNIT_EXPECT_EQ(test, status.tx_count, (u8)0);
	KUNIT_EXPECT_EQ(test, status.rate, (u16)0);
	payload[8] = 0xa5;

	payload[0]++;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_IGNORED);
	payload[0]--;
	payload[4]++;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_IGNORED);
	payload[4]--;
	event.id = 1;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), 0);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_IGNORED);
}

static void mt6797_wifi_ais_mgmt_event_rejects_test(struct kunit *test)
{
	struct mt6797_wifi_ais_mgmt_tx_status status;
	struct mt6797_wifi_ais_mgmt_pid_owner owner;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	struct mt6797_wifi_ais_mgmt_pid_lease stale;
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	enum mt6797_wifi_ais_event_disposition disposition;
	u8 payload[16];
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH,
		.id = 0x0f,
	};

	mt6797_wifi_ais_mgmt_handles(test, &resources, &bss, &sta);
	mt6797_wifi_ais_mgmt_owner_open(test, &owner, &bss, &sta, &lease);
	mt6797_wifi_ais_mgmt_event_fill(payload, lease.pid, lease.wlan_index);
	event.payload_length--;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
	event.payload_length++;
	event.payload = NULL;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
	event.payload = payload;
	event.packet_length--;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), -EPROTO);
	KUNIT_EXPECT_EQ(test, disposition, MT6797_WIFI_AIS_EVENT_TERMINAL);
	event.packet_length++;
	stale = lease;
	stale.cookie++;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &stale, &event,
			&disposition, &status), -ESTALE);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_mgmt_pid_mark_terminal(&owner), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_mgmt_tx_event_match(&owner, &lease, &event,
			&disposition, &status), -ESHUTDOWN);
}

static struct kunit_case mt6797_wifi_ais_mgmt_tx_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_mgmt_pid_lifecycle_test),
	KUNIT_CASE(mt6797_wifi_ais_mgmt_pid_overflow_test),
	KUNIT_CASE(mt6797_wifi_ais_mgmt_descriptor_test),
	KUNIT_CASE(mt6797_wifi_ais_mgmt_descriptor_rejects_test),
	KUNIT_CASE(mt6797_wifi_ais_mgmt_event_test),
	KUNIT_CASE(mt6797_wifi_ais_mgmt_event_rejects_test),
	{},
};

static struct kunit_suite mt6797_wifi_ais_mgmt_tx_suite = {
	.name = "mt6797-wifi-ais-mgmt-tx",
	.test_cases = mt6797_wifi_ais_mgmt_tx_cases,
};

kunit_test_suite(mt6797_wifi_ais_mgmt_tx_suite);

MODULE_LICENSE("GPL");
