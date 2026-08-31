// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb.h>

static const u8 mt6797_wifi_w1_sha256_abc[32] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
	0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
	0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static void mt6797_wifi_w1_accepts_exact_size_and_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image) - 1,
						       mt6797_wifi_w1_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void mt6797_wifi_w1_rejects_wrong_size_before_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image),
						       mt6797_wifi_w1_sha256_abc);
	KUNIT_EXPECT_EQ(test, ret, -EFBIG);
}

static void mt6797_wifi_w1_rejects_wrong_digest(struct kunit *test)
{
	static const u8 image[] = "abc";
	u8 wrong_digest[sizeof(mt6797_wifi_w1_sha256_abc)] = { };
	int ret;

	ret = mt6797_wifi_ahb_w1_firmware_validate(image, sizeof(image) - 1,
						       sizeof(image) - 1, wrong_digest);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static struct kunit_case mt6797_wifi_w1_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_w1_accepts_exact_size_and_digest),
	KUNIT_CASE(mt6797_wifi_w1_rejects_wrong_size_before_digest),
	KUNIT_CASE(mt6797_wifi_w1_rejects_wrong_digest),
	{}
};

static struct kunit_suite mt6797_wifi_w1_test_suite = {
	.name = "mt6797-wifi-ahb-w1-firmware",
	.test_cases = mt6797_wifi_w1_test_cases,
};

kunit_test_suite(mt6797_wifi_w1_test_suite);

MODULE_LICENSE("GPL");
