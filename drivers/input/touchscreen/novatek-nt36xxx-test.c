// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "novatek-nt36xxx.h"

static void nvt36xxx_trim_table_test(struct kunit *test)
{
	const struct nvt36xxx_chip *chip;
	u8 nt36772[NVT36XXX_TRIM_LEN] = { 0x23, 0x42, 0x99, 0x72, 0x66, 0x03 };
	u8 nt36525[NVT36XXX_TRIM_LEN] = { 0x23, 0x42, 0x99, 0x25, 0x65, 0x03 };
	u8 nt36870[NVT36XXX_TRIM_LEN] = { 0x23, 0x42, 0x99, 0x70, 0x68, 0x03 };
	u8 unknown[NVT36XXX_TRIM_LEN] = { 0, 0, 0, 0, 0, 0 };

	chip = nvt36xxx_find_chip(nt36772);
	KUNIT_ASSERT_NOT_NULL(test, chip);
	KUNIT_EXPECT_STREQ(test, chip->name, "NT36772");
	KUNIT_EXPECT_EQ(test, chip->mem_map->event_buf_addr, 0x11e00U);

	chip = nvt36xxx_find_chip(nt36525);
	KUNIT_ASSERT_NOT_NULL(test, chip);
	KUNIT_EXPECT_STREQ(test, chip->name, "NT36525");
	KUNIT_EXPECT_EQ(test, chip->mem_map->event_buf_addr, 0x11a00U);

	chip = nvt36xxx_find_chip(nt36870);
	KUNIT_ASSERT_NOT_NULL(test, chip);
	KUNIT_EXPECT_STREQ(test, chip->name, "NT36870");
	KUNIT_EXPECT_EQ(test, chip->mem_map->event_buf_addr, 0x25000U);

	KUNIT_EXPECT_NULL(test, nvt36xxx_find_chip(unknown));
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

	event[6] = (3 << 3) | 0x03;
	KUNIT_EXPECT_FALSE(test, nvt36xxx_decode_contact(event, 1, &contact));
}

static struct kunit_case nvt36xxx_test_cases[] = {
	KUNIT_CASE(nvt36xxx_trim_table_test),
	KUNIT_CASE(nvt36xxx_contact_decode_test),
	{}
};

static struct kunit_suite nvt36xxx_test_suite = {
	.name = "novatek-nt36xxx",
	.test_cases = nvt36xxx_test_cases,
};

kunit_test_suite(nvt36xxx_test_suite);

MODULE_LICENSE("GPL");
