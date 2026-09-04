// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>

#include "hif.h"

static void mt6797_wifi_ahb_cmd52_encode_test(struct kunit *test)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_pack_cmd52(false, 0, 0x02, 0, &command);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, command, 0x00000400U);

	ret = mt6797_wifi_ahb_pack_cmd52(true, 1, 0x34, 0xa5, &command);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, command, 0x900068a5U);
}

static void mt6797_wifi_ahb_cmd52_reject_test(struct kunit *test)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_pack_cmd52(false, 8, 0, 0, &command);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_pack_cmd52(false, 0, BIT(17), 0, &command);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_pack_cmd52(false, 0, 0, 0, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_wifi_ahb_cmd53_byte_mode_test(struct kunit *test)
{
	size_t transfer_length;
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0, 1, &command,
					 &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)4);
	KUNIT_EXPECT_EQ(test, command, 0x10000004U);

	ret = mt6797_wifi_ahb_plan_cmd53(true, 0x34, 508, &command,
					 &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)508);
	KUNIT_EXPECT_EQ(test, command, 0x900069fcU);
}

static void mt6797_wifi_ahb_cmd53_block_mode_test(struct kunit *test)
{
	size_t transfer_length;
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0, 509, &command,
					 &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)512);
	KUNIT_EXPECT_EQ(test, command, 0x18000001U);

	ret = mt6797_wifi_ahb_plan_cmd53(true, 0x34, 513, &command,
					 &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)1024);
	KUNIT_EXPECT_EQ(test, command, 0x98006802U);

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0,
					 MT6797_WIFI_AHB_MAX_TRANSFER_SIZE,
					 &command, &transfer_length);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transfer_length, (size_t)2560);
	KUNIT_EXPECT_EQ(test, command, 0x18000005U);
}

static void mt6797_wifi_ahb_cmd53_reject_test(struct kunit *test)
{
	size_t transfer_length;
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0, 0, &command,
					 &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0,
					 MT6797_WIFI_AHB_MAX_TRANSFER_SIZE + 1,
					 &command, &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_plan_cmd53(false, BIT(17), 4, &command,
					 &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0, 4, NULL,
					 &transfer_length);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mt6797_wifi_ahb_plan_cmd53(false, 0, 4, &command, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_wifi_ahb_hif_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_ahb_cmd52_encode_test),
	KUNIT_CASE(mt6797_wifi_ahb_cmd52_reject_test),
	KUNIT_CASE(mt6797_wifi_ahb_cmd53_byte_mode_test),
	KUNIT_CASE(mt6797_wifi_ahb_cmd53_block_mode_test),
	KUNIT_CASE(mt6797_wifi_ahb_cmd53_reject_test),
	{}
};

static struct kunit_suite mt6797_wifi_ahb_hif_test_suite = {
	.name = "mt6797-wifi-ahb-hif",
	.test_cases = mt6797_wifi_ahb_hif_test_cases,
};

kunit_test_suite(mt6797_wifi_ahb_hif_test_suite);

MODULE_LICENSE("GPL");
