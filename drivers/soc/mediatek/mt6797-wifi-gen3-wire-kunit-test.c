// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/if_ether.h>
#include <linux/unaligned.h>

#include "mt6797-wifi-gen3-wire.h"

static const u8 test_peer[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const u8 test_key[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static const u8 test_rsc[6] = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };

static const u8 expected_pairwise_cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN] = {
	0x48, 0x00, 0x00, 0x80, 0x07, 0x20, 0x01, 0x09,
	0x01, 0x01, 0x01, 0x00, 0x02, 0x11, 0x22, 0x33,
	0x44, 0x55, 0x00, 0x04, 0x00, 0x10, 0x01, 0x00,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static const u8 expected_default_cmd[MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN] = {
	0x0c, 0x00, 0x00, 0x80, 0x08, 0x20, 0x01, 0x0c,
	0x00, 0x02, 0x00, 0x01,
};

static void mt6797_wifi_gen3_pairwise_ccmp_test(struct kunit *test)
{
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 9, true, true,
					     test_peer, 0, 1,
					     MT6797_WIFI_GEN3_CIPHER_CCMP,
					     test_key, sizeof(test_key), NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(cmd));
	KUNIT_EXPECT_MEMEQ(test, cmd, expected_pairwise_cmd, sizeof(cmd));
}

static void mt6797_wifi_gen3_igtk_test(struct kunit *test)
{
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 10, true, false,
					     test_peer, 4, 0,
					     MT6797_WIFI_GEN3_CIPHER_BIP,
					     test_key, sizeof(test_key), NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(cmd));
	KUNIT_EXPECT_EQ(test, cmd[9], (u8)0);
	KUNIT_EXPECT_EQ(test, cmd[10], (u8)0);
	KUNIT_EXPECT_EQ(test, cmd[19], (u8)MT6797_WIFI_GEN3_CIPHER_BIP);
	KUNIT_EXPECT_EQ(test, cmd[20], (u8)4);
	KUNIT_EXPECT_EQ(test, cmd[22], (u8)0);
}

static void mt6797_wifi_gen3_rsc_test(struct kunit *test)
{
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	u8 zero[10] = { };
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 10, true,
					     false, test_peer, 2, 0,
					     MT6797_WIFI_GEN3_CIPHER_CCMP,
					     test_key, sizeof(test_key),
					     test_rsc, sizeof(test_rsc));
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(cmd));
	KUNIT_EXPECT_MEMEQ(test, cmd + 56, test_rsc, sizeof(test_rsc));
	KUNIT_EXPECT_MEMEQ(test, cmd + 62, zero, sizeof(zero));
}

static void mt6797_wifi_gen3_rejects_invalid_keys_test(struct kunit *test)
{
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 1, true, true,
					     test_peer, 1, 1,
					     MT6797_WIFI_GEN3_CIPHER_CCMP,
					     test_key, sizeof(test_key), NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 1, true, false,
					     test_peer, 4, 0,
					     MT6797_WIFI_GEN3_CIPHER_CCMP,
					     test_key, sizeof(test_key), NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 1, true, false,
					     test_peer, 1, 0,
					     MT6797_WIFI_GEN3_CIPHER_BIP,
					     test_key, sizeof(test_key), NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd), 1, true, false,
					     test_peer, 1, 0,
					     MT6797_WIFI_GEN3_CIPHER_CCMP,
					     test_key, sizeof(test_key), test_rsc, 17);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_wifi_gen3_remove_and_default_test(struct kunit *test)
{
	u8 key_cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	u8 default_cmd[MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(key_cmd, sizeof(key_cmd), 11,
					     false, false, test_peer, 4, 0,
					     MT6797_WIFI_GEN3_CIPHER_NONE,
					     NULL, 0, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(key_cmd));
	KUNIT_EXPECT_EQ(test, key_cmd[8], (u8)0);
	KUNIT_EXPECT_EQ(test, key_cmd[19], (u8)0);
	KUNIT_EXPECT_EQ(test, key_cmd[20], (u8)4);

	ret = mt6797_wifi_gen3_build_default_key_cmd(default_cmd,
						     sizeof(default_cmd), 12, 2,
						     false, true);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(default_cmd));
	KUNIT_EXPECT_MEMEQ(test, default_cmd, expected_default_cmd,
			   sizeof(default_cmd));
}

static void mt6797_wifi_gen3_bss_info_test(struct kunit *test)
{
	static const u8 ssid[] = "sae-test";
	u8 cmd[MT6797_WIFI_GEN3_BSS_INFO_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_bss_info_cmd(cmd, sizeof(cmd), 13, true,
						  true, ssid, sizeof(ssid) - 1,
						  test_peer, 6);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(cmd));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(cmd), (u16)sizeof(cmd));
	KUNIT_EXPECT_EQ(test, cmd[4], (u8)0x12);
	KUNIT_EXPECT_EQ(test, cmd[7], (u8)13);
	KUNIT_EXPECT_EQ(test, cmd[9], (u8)0);
	KUNIT_EXPECT_EQ(test, cmd[11], (u8)(sizeof(ssid) - 1));
	KUNIT_EXPECT_MEMEQ(test, cmd + 12, ssid, sizeof(ssid) - 1);
	KUNIT_EXPECT_MEMEQ(test, cmd + 44, test_peer, ETH_ALEN);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(cmd + 52), (u16)0x354f);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(cmd + 54), (u16)0x000f);
	KUNIT_EXPECT_EQ(test, cmd[61], (u8)7);
	KUNIT_EXPECT_EQ(test, cmd[62], (u8)6);
	KUNIT_EXPECT_EQ(test, cmd[63], (u8)(BIT(0) | BIT(1)));
	KUNIT_EXPECT_EQ(test, cmd[77], (u8)1);
	KUNIT_EXPECT_EQ(test, cmd[78], (u8)6);
	KUNIT_EXPECT_EQ(test, cmd[95], (u8)0);

	ret = mt6797_wifi_gen3_build_bss_info_cmd(cmd, sizeof(cmd), 14, false,
						  false, NULL, 0, test_peer, 1);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(cmd));
	KUNIT_EXPECT_EQ(test, cmd[9], (u8)1);
	KUNIT_EXPECT_EQ(test, cmd[61], (u8)0);
	KUNIT_EXPECT_EQ(test, cmd[62], (u8)1);

	ret = mt6797_wifi_gen3_build_bss_info_cmd(cmd, sizeof(cmd), 15, false,
						  false, NULL, 0, test_peer, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_wifi_gen3_wire_cases[] = {
	KUNIT_CASE(mt6797_wifi_gen3_pairwise_ccmp_test),
	KUNIT_CASE(mt6797_wifi_gen3_igtk_test),
	KUNIT_CASE(mt6797_wifi_gen3_rsc_test),
	KUNIT_CASE(mt6797_wifi_gen3_rejects_invalid_keys_test),
	KUNIT_CASE(mt6797_wifi_gen3_remove_and_default_test),
	KUNIT_CASE(mt6797_wifi_gen3_bss_info_test),
	{}
};

static struct kunit_suite mt6797_wifi_gen3_wire_suite = {
	.name = "mt6797-wifi-gen3-wire",
	.test_cases = mt6797_wifi_gen3_wire_cases,
};

kunit_test_suite(mt6797_wifi_gen3_wire_suite);

MODULE_LICENSE("GPL");
