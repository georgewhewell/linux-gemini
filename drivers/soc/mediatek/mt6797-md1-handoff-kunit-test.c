// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 passive hand-off contract. */

#include <kunit/test.h>
#include <linux/errno.h>

#include "mt6797-md1-handoff.h"

static const struct mt6797_md1_resource mt6797_md1_test_resources[] = {
	{ 0x10014000, 0x1e00 }, { 0x10001000, 0x1000 },
	{ 0x10015000, 0x1e00 }, { 0x10219000, 0x1e00 },
	{ 0x1021a000, 0x1e00 }, { 0x10209000, 0x1000 },
	{ 0x1020a000, 0x1000 }, { 0x1000c000, 0x1000 },
	{ 0x20150000, 0x0118 }, { 0x20130000, 0x1004 },
	{ 0x20060000, 0x00d0 }, { 0x260f0000, 0x0200 },
	{ 0x20140000, 0x00a0 }, { 0x20120000, 0x00d0 },
	{ 0x200f0100, 0x0300 }, { 0x26010000, 0x0040 },
	{ 0x20000024, 0x0004 }, { 0x10201300, 0x0008 },
};

static void mt6797_md1_accepts_vendor_resources(struct kunit *test)
{
	size_t count = ARRAY_SIZE(mt6797_md1_test_resources);
	int ret;

	ret = mt6797_md1_resources_validate(mt6797_md1_test_resources, count);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void mt6797_md1_rejects_changed_resource(struct kunit *test)
{
	struct mt6797_md1_resource resources[ARRAY_SIZE(mt6797_md1_test_resources)];
	int ret;

	memcpy(resources, mt6797_md1_test_resources, sizeof(resources));
	resources[12].size = 0x18;
	ret = mt6797_md1_resources_validate(resources, ARRAY_SIZE(resources));
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void mt6797_md1_accepts_lk_layout(struct kunit *test)
{
	const struct mt6797_md1_layout layout = {
		.firmware_base = 0xb4000000,
		.firmware_size = 0x0a000000,
		.shared_base = 0x88000000,
		.shared_size = 0x00600000,
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_layout_validate(&layout), 0);
}

static void mt6797_md1_rejects_unsafe_layouts(struct kunit *test)
{
	struct mt6797_md1_layout layout = {
		.firmware_base = 0xb4000000,
		.firmware_size = 0x0a000000,
		.shared_base = 0x88000000,
		.shared_size = 0x00600000,
	};

	layout.shared_base = 0x100000000ULL;
	KUNIT_EXPECT_EQ(test, mt6797_md1_layout_validate(&layout), -ERANGE);
	layout.shared_base = 0xb4000000;
	KUNIT_EXPECT_EQ(test, mt6797_md1_layout_validate(&layout), -EINVAL);
	layout.shared_base = 0x88000001;
	KUNIT_EXPECT_EQ(test, mt6797_md1_layout_validate(&layout), -EINVAL);
	layout.shared_base = 0x88000000;
	layout.firmware_size = 0x09ff0000;
	KUNIT_EXPECT_EQ(test, mt6797_md1_layout_validate(&layout), -ENOSPC);
}

static void mt6797_md1_builds_vendor_remap_words(struct kunit *test)
{
	const struct mt6797_md1_layout layout = {
		.firmware_base = 0xb4000000,
		.firmware_size = 0x0a000000,
		.shared_base = 0x88000000,
		.shared_size = 0x00600000,
	};
	struct mt6797_md1_remap_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_plan_build(&layout, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.physical.bank0_map0, 0xbbb9b7b5U);
	KUNIT_EXPECT_EQ(test, plan.physical.bank0_map1, 0xc3c1bfbdU);
	KUNIT_EXPECT_EQ(test, plan.physical.bank4_map0, 0x8f8d8b89U);
	KUNIT_EXPECT_EQ(test, plan.physical.bank4_map1, 0x97959391U);
	KUNIT_EXPECT_EQ(test, plan.emi_relative.bank0_map0, 0x7b797775U);
	KUNIT_EXPECT_EQ(test, plan.emi_relative.bank0_map1, 0x83817f7dU);
	KUNIT_EXPECT_EQ(test, plan.emi_relative.bank4_map0, 0x4f4d4b49U);
	KUNIT_EXPECT_EQ(test, plan.emi_relative.bank4_map1, 0x57555351U);
	KUNIT_EXPECT_EQ(test, plan.bank4_base, 0x88000000ULL);
}

static void mt6797_md1_builds_passive_q0(struct kunit *test)
{
	struct mt6797_md1_q0 q0;

	KUNIT_ASSERT_EQ(test,
			mt6797_md1_q0_init(&q0, 0x81234000, 0x81234010),
			0);
	KUNIT_EXPECT_EQ(test, q0.tx.flags, (u8)MT6797_MD1_GPD_IOC);
	KUNIT_EXPECT_EQ(test, q0.rx.flags, (u8)MT6797_MD1_GPD_IOC);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q0.tx.next), 0x81234000U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q0.rx.next), 0x81234010U);
	KUNIT_EXPECT_EQ(test, q0.tx.flags & MT6797_MD1_GPD_HWO, (u8)0);
	KUNIT_EXPECT_EQ(test, q0.rx.flags & MT6797_MD1_GPD_HWO, (u8)0);
}

static void mt6797_md1_rejects_unusable_q0_roots(struct kunit *test)
{
	struct mt6797_md1_q0 q0;

	KUNIT_EXPECT_EQ(test,
			mt6797_md1_q0_init(&q0, 0x100000000ULL, 0x1000),
			-ERANGE);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_q0_init(&q0, 0x1001, 0x2000),
			-ERANGE);
}

static struct kunit_case mt6797_md1_handoff_test_cases[] = {
	KUNIT_CASE(mt6797_md1_accepts_vendor_resources),
	KUNIT_CASE(mt6797_md1_rejects_changed_resource),
	KUNIT_CASE(mt6797_md1_accepts_lk_layout),
	KUNIT_CASE(mt6797_md1_rejects_unsafe_layouts),
	KUNIT_CASE(mt6797_md1_builds_vendor_remap_words),
	KUNIT_CASE(mt6797_md1_builds_passive_q0),
	KUNIT_CASE(mt6797_md1_rejects_unusable_q0_roots),
	{}
};

static struct kunit_suite mt6797_md1_handoff_test_suite = {
	.name = "mt6797-md1-handoff",
	.test_cases = mt6797_md1_handoff_test_cases,
};

kunit_test_suite(mt6797_md1_handoff_test_suite);

MODULE_LICENSE("GPL");
