// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "protocol.h"

static void mt6797_wifi_command_test(struct kunit *test)
{
	u8 command[16];
	int ret;

	ret = mt6797_wifi_command_init(command, sizeof(command), 8, 2, true,
				       0x5a);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command), (u16)sizeof(command));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 2), (u16)0x8000);
	KUNIT_EXPECT_EQ(test, command[4], (u8)2);
	KUNIT_EXPECT_EQ(test, command[5], (u8)0xa0);
	KUNIT_EXPECT_EQ(test, command[6], (u8)1);
	KUNIT_EXPECT_EQ(test, command[7], (u8)0x5a);
}

static void mt6797_wifi_basic_config_test(struct kunit *test)
{
	u8 expected[MT6797_WIFI_BASIC_CONFIG_LENGTH] = {
		0x10, 0x00, 0x00, 0x80, 0x02, 0xa0, 0x01, 0x02,
	};
	u8 command[MT6797_WIFI_BASIC_CONFIG_LENGTH];
	int ret;

	ret = mt6797_wifi_basic_config_build(command, sizeof(command), 2);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, command, expected, sizeof(expected));

	ret = mt6797_wifi_basic_config_build(command, sizeof(command) - 1, 2);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_wifi_command_bounds_test(struct kunit *test)
{
	u8 command[MT6797_WIFI_COMMAND_HEADER_LENGTH];
	int ret;

	ret = mt6797_wifi_command_init(NULL, sizeof(command), 0, 1, false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_wifi_command_init(command, sizeof(command) - 1, 0, 1,
				       false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	ret = mt6797_wifi_command_init(command, SIZE_MAX, SIZE_MAX, 1, false,
				       0);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
	ret = mt6797_wifi_command_init(command, SIZE_MAX,
				       MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH,
				       1, false, 0);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_wifi_event_test(struct kunit *test)
{
	u8 packet[12] = {
		0x0c, 0x00, 0x00, 0xe0, 0x27, 0x5a, 0x00, 0x00,
		0x11, 0x22, 0x33, 0x44,
	};
	struct mt6797_wifi_event_view event;
	int ret;

	ret = mt6797_wifi_event_parse(packet, sizeof(packet), &event);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, event.id, (u8)0x27);
	KUNIT_EXPECT_EQ(test, event.sequence, (u8)0x5a);
	KUNIT_EXPECT_EQ(test, event.packet_length, (size_t)12);
	KUNIT_EXPECT_EQ(test, event.payload_length, (size_t)4);
	KUNIT_EXPECT_PTR_EQ(test, event.payload, packet + 8);
	KUNIT_EXPECT_MEMEQ(test, event.payload, packet + 8, 4);
}

static void mt6797_wifi_event_bounds_test(struct kunit *test)
{
	u8 packet[12] = {
		0x0c, 0x00, 0x00, 0xe0, 0x27, 0x5a, 0x00, 0x00,
	};
	struct mt6797_wifi_event_view event;

	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(NULL, sizeof(packet),
						      &event), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, 7, &event),
			-EMSGSIZE);
	put_unaligned_le16(7, packet);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EMSGSIZE);
	put_unaligned_le16(13, packet);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EMSGSIZE);
	put_unaligned_le16(sizeof(packet), packet);
	packet[2] = 1;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), -EPROTO);
	packet[2] = 0;
	/* Vendor WIFI_EVENT_T reserves these bytes without requiring zero. */
	packet[6] = 0xa5;
	packet[7] = 0x5a;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_event_parse(packet, sizeof(packet),
						      &event), 0);
}

static void mt6797_wifi_rx_event_test(struct kunit *test)
{
	u8 packet[12] = {
		0x0c, 0x00, 0x00, 0xe0, 0x0d, 0x03, 0x00, 0x00,
		0x03, 0x00, 0x00, 0x00,
	};
	struct mt6797_wifi_rx_view frame;
	int ret;

	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, frame.kind, MT6797_WIFI_RX_EVENT);
	KUNIT_EXPECT_PTR_EQ(test, frame.packet, (const u8 *)packet);
	KUNIT_EXPECT_EQ(test, frame.event_id, (u8)0x0d);
	KUNIT_EXPECT_EQ(test, frame.sequence, (u8)3);
	KUNIT_EXPECT_EQ(test, frame.payload_length, (size_t)4);
}

static void mt6797_wifi_rx_descriptor_test(struct kunit *test)
{
	u8 packet[96] = {};
	struct mt6797_wifi_rx_view frame;
	int ret;

	put_unaligned_le16(sizeof(packet), packet);
	put_unaligned_le16(0xf << 9, packet + 2);
	packet[5] = 6;
	packet[6] = BIT(7) | ETH_HLEN;
	packet[8] = 31;
	packet[80] = 0x02;
	put_unaligned_be16(ETH_P_IP, packet + 80 + 12);

	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, frame.kind, MT6797_WIFI_RX_ETHERNET);
	KUNIT_EXPECT_PTR_EQ(test, frame.packet, (const u8 *)packet);
	KUNIT_EXPECT_PTR_EQ(test, frame.payload, packet + 80);
	KUNIT_EXPECT_EQ(test, frame.payload_length, (size_t)16);
	KUNIT_EXPECT_EQ(test, frame.channel, (u8)6);
	KUNIT_EXPECT_EQ(test, frame.header_length, (u8)ETH_HLEN);
	KUNIT_EXPECT_EQ(test, frame.wlan_index, (u8)31);
	KUNIT_EXPECT_TRUE(test, frame.header_translated);

	memset(packet, 0, sizeof(packet));
	put_unaligned_le16(42, packet);
	put_unaligned_le16(0xe001, packet + 2);
	packet[6] = 24;
	ret = mt6797_wifi_rx_parse(packet, 44, &frame);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, frame.kind, MT6797_WIFI_RX_IEEE80211);
	KUNIT_EXPECT_PTR_EQ(test, frame.packet, (const u8 *)packet);
	KUNIT_EXPECT_PTR_EQ(test, frame.payload, packet + 16);
	KUNIT_EXPECT_EQ(test, frame.payload_length, (size_t)26);
}

static void mt6797_wifi_rx_bounds_test(struct kunit *test)
{
	u8 packet[64] = {};
	struct mt6797_wifi_rx_view frame;
	int ret;

	ret = mt6797_wifi_rx_parse(packet, 3, &frame);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);

	put_unaligned_le16(32, packet);
	packet[6] = BIT(7) | ETH_HLEN;
	put_unaligned_le16(BIT(1), packet + 10);
	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);

	put_unaligned_le16(16, packet);
	put_unaligned_le16(BIT(12), packet + 2);
	put_unaligned_le16(0, packet + 10);
	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);

	put_unaligned_le16(32, packet);
	put_unaligned_le16(0, packet + 2);
	packet[6] = 17;
	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);

	packet[6] = 14;
	ret = mt6797_wifi_rx_parse(packet, sizeof(packet), &frame);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
}

static void mt6797_wifi_tx_data_test(struct kunit *test)
{
	u8 payload[ETH_HLEN + 2] = {};
	u8 packet[48];
	struct mt6797_wifi_tx_params params = {
		.traffic = MT6797_WIFI_TX_DATA,
		.wlan_index = 31,
		.protected = true,
	};
	size_t transfer_length;
	int ret;

	put_unaligned_be16(ETH_P_IP, payload + 12);
	ret = mt6797_wifi_tx_build(packet, sizeof(packet), payload,
				   sizeof(payload), &params, &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)44);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet), (u16)44);
	KUNIT_EXPECT_EQ(test, packet[2], (u8)20);
	KUNIT_EXPECT_EQ(test, packet[3], (u8)0x20);
	KUNIT_EXPECT_EQ(test, packet[4], (u8)31);
	KUNIT_EXPECT_EQ(test, packet[5], (u8)0x90);
	KUNIT_EXPECT_EQ(test, packet[6], (u8)BIT(7));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 12), (u16)0xf000);
	KUNIT_EXPECT_MEMEQ(test, packet + MT6797_WIFI_TX_DESCRIPTOR_LENGTH,
			   payload, sizeof(payload));
}

static void mt6797_wifi_tx_eapol_test(struct kunit *test)
{
	u8 payload[ETH_HLEN + 1] = {};
	u8 packet[44];
	struct mt6797_wifi_tx_params params = {
		.traffic = MT6797_WIFI_TX_EAPOL,
		.fixed_rate = 0x4b,
		.wlan_index = 1,
	};
	size_t transfer_length;
	int ret;

	memset(packet, 0xa5, sizeof(packet));
	put_unaligned_be16(ETH_P_PAE, payload + 12);
	ret = mt6797_wifi_tx_build(packet, sizeof(packet), payload,
				   sizeof(payload), &params, &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, sizeof(packet));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet), (u16)43);
	KUNIT_EXPECT_EQ(test, packet[3], (u8)0x88);
	KUNIT_EXPECT_EQ(test, packet[11], (u8)BIT(7));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 26), (u16)(0x4b << 2));
	KUNIT_EXPECT_EQ(test, packet[43], (u8)0);
}

static void mt6797_wifi_tx_bounds_test(struct kunit *test)
{
	u8 payload[ETH_HLEN] = {};
	u8 packet[ALIGN(MT6797_WIFI_TX_DESCRIPTOR_LENGTH + ETH_HLEN, 4)];
	struct mt6797_wifi_tx_params params = {
		.traffic = MT6797_WIFI_TX_EAPOL,
		.fixed_rate = 0x4b,
	};
	size_t transfer_length;
	int ret;

	put_unaligned_be16(ETH_P_IP, payload + 12);
	ret = mt6797_wifi_tx_build(packet, sizeof(packet), payload,
				   sizeof(payload), &params, &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	put_unaligned_be16(ETH_P_PAE, payload + 12);
	params.fixed_rate = 0x4000;
	ret = mt6797_wifi_tx_build(packet, sizeof(packet), payload,
				   sizeof(payload), &params, &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	params.fixed_rate = 0x4b;
	ret = mt6797_wifi_tx_build(packet, sizeof(packet) - 1, payload,
				   sizeof(payload), &params, &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	ret = mt6797_wifi_tx_build(packet, SIZE_MAX, payload, SIZE_MAX, &params,
				   &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
}

static struct kunit_case mt6797_wifi_protocol_cases[] = {
	KUNIT_CASE(mt6797_wifi_command_test),
	KUNIT_CASE(mt6797_wifi_basic_config_test),
	KUNIT_CASE(mt6797_wifi_command_bounds_test),
	KUNIT_CASE(mt6797_wifi_event_test),
	KUNIT_CASE(mt6797_wifi_event_bounds_test),
	KUNIT_CASE(mt6797_wifi_rx_event_test),
	KUNIT_CASE(mt6797_wifi_rx_descriptor_test),
	KUNIT_CASE(mt6797_wifi_rx_bounds_test),
	KUNIT_CASE(mt6797_wifi_tx_data_test),
	KUNIT_CASE(mt6797_wifi_tx_eapol_test),
	KUNIT_CASE(mt6797_wifi_tx_bounds_test),
	{}
};

static struct kunit_suite mt6797_wifi_protocol_suite = {
	.name = "mt6797-wifi-protocol",
	.test_cases = mt6797_wifi_protocol_cases,
};

kunit_test_suite(mt6797_wifi_protocol_suite);

MODULE_LICENSE("GPL");
