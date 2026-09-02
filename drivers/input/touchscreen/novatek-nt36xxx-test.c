// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "novatek-nt36xxx.h"

static void nvt36xxx_trim_test(struct kunit *test)
{
	u8 match[NVT36XXX_TRIM_LEN] = { 0x00, 0x00, 0x03, 0x72, 0x66, 0x03 };
	u8 wrong_model[NVT36XXX_TRIM_LEN] = { 0x00, 0x00, 0x03, 0x70, 0x66, 0x03 };
	u8 wrong_family[NVT36XXX_TRIM_LEN] = { 0x00, 0x00, 0x03, 0x72, 0x66, 0x04 };

	KUNIT_EXPECT_TRUE(test, nvt36xxx_trim_is_nt36772(match));
	KUNIT_EXPECT_FALSE(test, nvt36xxx_trim_is_nt36772(wrong_model));
	KUNIT_EXPECT_FALSE(test, nvt36xxx_trim_is_nt36772(wrong_family));
}

static void nvt36xxx_contact_decode_test(struct kunit *test)
{
	struct nvt36xxx_contact contact;
	u8 event[NVT36XXX_EVENT_DATA_LEN] = { };

	event[0] = (2 << 3) | 0x01;
	event[1] = 0x12;
	event[2] = 0x34;
	event[3] = 0x56;
	event[4] = 0;
	event[5] = 0x78;
	event[62] = 0x03;

	KUNIT_ASSERT_TRUE(test, nvt36xxx_decode_contact(event, 0, &contact));
	KUNIT_EXPECT_EQ(test, contact.slot, 2U);
	KUNIT_EXPECT_EQ(test, contact.x, 0x125U);
	KUNIT_EXPECT_EQ(test, contact.y, 0x346U);
	KUNIT_EXPECT_EQ(test, contact.width, 1U);
	KUNIT_EXPECT_EQ(test, contact.pressure, 0x378U);
}

static void nvt36xxx_contact_status_test(struct kunit *test)
{
	struct nvt36xxx_contact contact;
	u8 event[NVT36XXX_EVENT_DATA_LEN] = { };

	event[0] = (1 << 3) | 0x02;
	KUNIT_EXPECT_TRUE(test, nvt36xxx_decode_contact(event, 0, &contact));

	event[0] = (1 << 3) | 0x03;
	KUNIT_EXPECT_FALSE(test, nvt36xxx_decode_contact(event, 0, &contact));

	event[0] = 0x01;
	KUNIT_EXPECT_FALSE(test, nvt36xxx_decode_contact(event, 0, &contact));

	event[0] = (11 << 3) | 0x01;
	KUNIT_EXPECT_FALSE(test, nvt36xxx_decode_contact(event, 0, &contact));
}

static struct kunit_case nvt36xxx_test_cases[] = {
	KUNIT_CASE(nvt36xxx_trim_test),
	KUNIT_CASE(nvt36xxx_contact_decode_test),
	KUNIT_CASE(nvt36xxx_contact_status_test),
	{}
};

static struct kunit_suite nvt36xxx_test_suite = {
	.name = "novatek-nt36xxx",
	.test_cases = nvt36xxx_test_cases,
};

kunit_test_suite(nvt36xxx_test_suite);

MODULE_LICENSE("GPL");
