// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/sizes.h>

#include "mtk_seninf_core.h"

#define MT6797_TEST_CORE_WORDS (MT6797_SENINF_CORE_MIN_SIZE / sizeof(u32))
#define MT6797_TEST_ANA_WORDS (MT6797_SENINF_ANA_MIN_SIZE / sizeof(u32))

struct mt6797_seninf_test_io {
	u32 *core;
	u32 *ana;
	unsigned int delay_min[3];
	unsigned int delay_max[3];
	unsigned int num_delays;
	bool invalid_access;
};

static u32 mt6797_seninf_test_read(void *context, enum mt6797_seninf_bank bank,
				   u32 offset)
{
	struct mt6797_seninf_test_io *test_io = context;
	u32 *registers = bank == MT6797_SENINF_BANK_CORE ? test_io->core :
							   test_io->ana;
	unsigned int words = bank == MT6797_SENINF_BANK_CORE ?
				     MT6797_TEST_CORE_WORDS :
				     MT6797_TEST_ANA_WORDS;

	if (!IS_ALIGNED(offset, sizeof(u32)) || offset / sizeof(u32) >= words) {
		test_io->invalid_access = true;
		return 0;
	}

	return registers[offset / sizeof(u32)];
}

static void mt6797_seninf_test_write(void *context,
				     enum mt6797_seninf_bank bank, u32 offset,
				     u32 value)
{
	struct mt6797_seninf_test_io *test_io = context;
	u32 *registers = bank == MT6797_SENINF_BANK_CORE ? test_io->core :
							   test_io->ana;
	unsigned int words = bank == MT6797_SENINF_BANK_CORE ?
				     MT6797_TEST_CORE_WORDS :
				     MT6797_TEST_ANA_WORDS;

	if (!IS_ALIGNED(offset, sizeof(u32)) || offset / sizeof(u32) >= words) {
		test_io->invalid_access = true;
		return;
	}

	registers[offset / sizeof(u32)] = value;
}

static void mt6797_seninf_test_delay(void *context, unsigned int min,
				     unsigned int max)
{
	struct mt6797_seninf_test_io *test_io = context;

	if (test_io->num_delays >= ARRAY_SIZE(test_io->delay_min)) {
		test_io->invalid_access = true;
		return;
	}

	test_io->delay_min[test_io->num_delays] = min;
	test_io->delay_max[test_io->num_delays] = max;
	test_io->num_delays++;
}

static int mt6797_seninf_test_io_init(struct kunit *test,
				      struct mt6797_seninf_test_io *test_io,
				      struct mt6797_seninf_io *io)
{
	test_io->core = kunit_kcalloc(test, MT6797_TEST_CORE_WORDS,
				      sizeof(*test_io->core), GFP_KERNEL);
	test_io->ana = kunit_kcalloc(test, MT6797_TEST_ANA_WORDS,
				     sizeof(*test_io->ana), GFP_KERNEL);
	if (!test_io->core || !test_io->ana)
		return -ENOMEM;

	io->read = mt6797_seninf_test_read;
	io->write = mt6797_seninf_test_write;
	io->delay_us = mt6797_seninf_test_delay;
	io->context = test_io;
	return 0;
}

static const struct mt6797_seninf_res mt6797_valid_resources = {
	.core = { .start = 0x1a040000, .size = SZ_32K },
	.csi0_ana = { .start = 0x10217000, .size = SZ_4K },
	.csi1_ana = { .start = 0x10218000, .size = SZ_4K },
};

static void mt6797_seninf_resources_valid(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_resources_validate(&mt6797_valid_resources),
			0);
}

static void mt6797_seninf_resources_reject_short_core(struct kunit *test)
{
	struct mt6797_seninf_res resources = mt6797_valid_resources;

	resources.core.size = SZ_16K;
	KUNIT_EXPECT_EQ(test, mt6797_seninf_resources_validate(&resources),
			-EINVAL);
}

static void mt6797_seninf_resources_reject_short_analog(struct kunit *test)
{
	struct mt6797_seninf_res resources = mt6797_valid_resources;

	resources.csi1_ana.size = SZ_2K;
	KUNIT_EXPECT_EQ(test, mt6797_seninf_resources_validate(&resources),
			-EINVAL);
}

static void mt6797_seninf_resources_reject_overlap(struct kunit *test)
{
	struct mt6797_seninf_res resources = mt6797_valid_resources;

	resources.csi0_ana.start = resources.core.start + SZ_4K;
	KUNIT_EXPECT_EQ(test, mt6797_seninf_resources_validate(&resources),
			-EINVAL);
}

static void mt6797_seninf_resources_reject_overflow(struct kunit *test)
{
	struct mt6797_seninf_res resources = mt6797_valid_resources;

	resources.csi0_ana.start = U64_MAX - SZ_2K;
	KUNIT_EXPECT_EQ(test, mt6797_seninf_resources_validate(&resources),
			-EINVAL);
}

static void mt6797_seninf_endpoint_accepts_dphy(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_endpoint_validate(V4L2_MBUS_CSI2_DPHY, 2),
			0);
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_endpoint_validate(V4L2_MBUS_CSI2_DPHY, 4),
			0);
}

static void mt6797_seninf_endpoint_rejects_other_phy(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_endpoint_validate(V4L2_MBUS_CSI2_CPHY, 2),
			-EINVAL);
}

static void mt6797_seninf_endpoint_rejects_lane_count(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_endpoint_validate(V4L2_MBUS_CSI2_DPHY, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_endpoint_validate(V4L2_MBUS_CSI2_DPHY, 5),
			-EINVAL);
}

static void mt6797_seninf_format_defaults(struct kunit *test)
{
	struct v4l2_mbus_framefmt format = {};

	mt6797_seninf_format_adjust(&format);
	KUNIT_EXPECT_EQ(test, format.code, (u32)MEDIA_BUS_FMT_SGRBG10_1X10);
	KUNIT_EXPECT_EQ(test, format.width, (u32)MT6797_SENINF_DEFAULT_WIDTH);
	KUNIT_EXPECT_EQ(test, format.height, (u32)MT6797_SENINF_DEFAULT_HEIGHT);
	KUNIT_EXPECT_EQ(test, format.field, V4L2_FIELD_NONE);
	KUNIT_EXPECT_EQ(test, format.colorspace, V4L2_COLORSPACE_RAW);
}

static void mt6797_seninf_format_preserves_dimensions(struct kunit *test)
{
	struct v4l2_mbus_framefmt format = {
		.code = MEDIA_BUS_FMT_YUYV8_1X16,
		.width = 640,
		.height = 480,
	};

	mt6797_seninf_format_adjust(&format);
	KUNIT_EXPECT_EQ(test, format.code, (u32)MEDIA_BUS_FMT_SGRBG10_1X10);
	KUNIT_EXPECT_EQ(test, format.width, 640U);
	KUNIT_EXPECT_EQ(test, format.height, 480U);
}

static void mt6797_seninf_route_accepts_gemini_full_mode(struct kunit *test)
{
	struct v4l2_mbus_framefmt format = {};
	const unsigned int lanes[] = { 1, 2 };

	mt6797_seninf_format_adjust(&format);
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_route_validate(lanes, ARRAY_SIZE(lanes),
						     &format,
						     MT6797_SENINF_FULL_LINK_FREQ),
			0);
}

static void mt6797_seninf_route_rejects_unproved_paths(struct kunit *test)
{
	struct v4l2_mbus_framefmt format = {};
	const unsigned int swapped_lanes[] = { 2, 1 };
	const unsigned int lanes[] = { 1, 2 };

	mt6797_seninf_format_adjust(&format);
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_route_validate(swapped_lanes,
						     ARRAY_SIZE(swapped_lanes),
						     &format,
						     MT6797_SENINF_FULL_LINK_FREQ),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_route_validate(lanes, ARRAY_SIZE(lanes),
						     &format, 220000000),
			-EOPNOTSUPP);
	format.width = 1296;
	format.height = 972;
	KUNIT_EXPECT_EQ(test,
			mt6797_seninf_route_validate(lanes, ARRAY_SIZE(lanes),
						     &format,
						     MT6797_SENINF_FULL_LINK_FREQ),
			-EOPNOTSUPP);
}

static void mt6797_seninf_register_plan_succeeds(struct kunit *test)
{
	struct mt6797_seninf_test_io test_io = {};
	struct mt6797_seninf_io io = {};
	struct mt6797_seninf_config config = {
		.calibration = { 0, 0, U32_MAX, U32_MAX, 0 },
		.num_data_lanes = 2,
		.settle_ns = MT6797_SENINF_FULL_SETTLE_NS,
	};

	KUNIT_ASSERT_EQ(test, mt6797_seninf_test_io_init(test, &test_io, &io),
			0);
	test_io.core[0x1844 / sizeof(u32)] = BIT(0);
	test_io.core[0x1848 / sizeof(u32)] = BIT(0);

	KUNIT_EXPECT_EQ(test, mt6797_seninf_hw_start(&io, &config), 0);
	KUNIT_EXPECT_FALSE(test, test_io.invalid_access);
	KUNIT_EXPECT_EQ(test, test_io.num_delays, 3U);
	KUNIT_EXPECT_EQ(test, test_io.delay_min[0], 30U);
	KUNIT_EXPECT_EQ(test, test_io.delay_min[1], 1U);
	KUNIT_EXPECT_EQ(test, test_io.delay_min[2], 500U);
	KUNIT_EXPECT_EQ(test, test_io.core[0x1200 / sizeof(u32)],
			BIT(0) | (8U << 12));
	KUNIT_EXPECT_EQ(test, test_io.core[0x1204 / sizeof(u32)], BIT(5));
	KUNIT_EXPECT_EQ(test, test_io.core[0x0d00 / sizeof(u32)],
			BIT(31) | (8U << 12) | 0x06000000 | 0x001e0000);
	KUNIT_EXPECT_EQ(test, test_io.core[0x1708 / sizeof(u32)], 4U << 8);
	KUNIT_EXPECT_EQ(test, test_io.core[0x1700 / sizeof(u32)],
			BIT(26) | BIT(16) | BIT(7) | BIT(4) | GENMASK(1, 0));
	KUNIT_EXPECT_EQ(test, test_io.core[0x1710 / sizeof(u32)], 0x80007fff);
	KUNIT_EXPECT_EQ(test,
			test_io.core[0x0008 / sizeof(u32)] & GENMASK(3, 0), 1U);
	KUNIT_EXPECT_EQ(test, test_io.ana[0x0408 / sizeof(u32)], 0x1f1f1f1fU);
	KUNIT_EXPECT_EQ(test, test_io.ana[0x040c / sizeof(u32)] & 0x1f1f,
			0x1f1fU);
	KUNIT_EXPECT_EQ(test, test_io.ana[0x0608 / sizeof(u32)], 0x1f1f1f1fU);
	KUNIT_EXPECT_EQ(test, test_io.ana[0x060c / sizeof(u32)] & 0x1f1f,
			0x1f1fU);

	mt6797_seninf_hw_stop(&io);
	KUNIT_EXPECT_EQ(test,
			test_io.core[0x1700 / sizeof(u32)] & GENMASK(4, 0), 0U);
	KUNIT_EXPECT_EQ(test, test_io.core[0x0d00 / sizeof(u32)] & BIT(31), 0U);
	KUNIT_EXPECT_EQ(test, test_io.core[0x1200 / sizeof(u32)] & BIT(0), 0U);
	KUNIT_EXPECT_EQ(test,
			test_io.ana[0x0400 / sizeof(u32)] & (BIT(3) | BIT(2)),
			0U);
	KUNIT_EXPECT_EQ(test,
			test_io.ana[0x0600 / sizeof(u32)] & (BIT(3) | BIT(2)),
			0U);
}

static void mt6797_seninf_register_plan_rolls_back(struct kunit *test)
{
	struct mt6797_seninf_test_io test_io = {};
	struct mt6797_seninf_io io = {};
	struct mt6797_seninf_config config = {
		.num_data_lanes = 2,
		.settle_ns = MT6797_SENINF_FULL_SETTLE_NS,
	};

	KUNIT_ASSERT_EQ(test, mt6797_seninf_test_io_init(test, &test_io, &io),
			0);
	KUNIT_EXPECT_EQ(test, mt6797_seninf_hw_start(&io, &config), -EIO);
	KUNIT_EXPECT_FALSE(test, test_io.invalid_access);
	KUNIT_EXPECT_EQ(test,
			test_io.core[0x1700 / sizeof(u32)] & GENMASK(4, 0), 0U);
	KUNIT_EXPECT_EQ(test, test_io.core[0x0d00 / sizeof(u32)] & BIT(31), 0U);
	KUNIT_EXPECT_EQ(test, test_io.core[0x1200 / sizeof(u32)] & BIT(0), 0U);
	KUNIT_EXPECT_EQ(test,
			test_io.ana[0x0400 / sizeof(u32)] & (BIT(3) | BIT(2)),
			0U);
	KUNIT_EXPECT_EQ(test,
			test_io.ana[0x0600 / sizeof(u32)] & (BIT(3) | BIT(2)),
			0U);
}

static struct kunit_case mt6797_seninf_test_cases[] = {
	KUNIT_CASE(mt6797_seninf_resources_valid),
	KUNIT_CASE(mt6797_seninf_resources_reject_short_core),
	KUNIT_CASE(mt6797_seninf_resources_reject_short_analog),
	KUNIT_CASE(mt6797_seninf_resources_reject_overlap),
	KUNIT_CASE(mt6797_seninf_resources_reject_overflow),
	KUNIT_CASE(mt6797_seninf_endpoint_accepts_dphy),
	KUNIT_CASE(mt6797_seninf_endpoint_rejects_other_phy),
	KUNIT_CASE(mt6797_seninf_endpoint_rejects_lane_count),
	KUNIT_CASE(mt6797_seninf_format_defaults),
	KUNIT_CASE(mt6797_seninf_format_preserves_dimensions),
	KUNIT_CASE(mt6797_seninf_route_accepts_gemini_full_mode),
	KUNIT_CASE(mt6797_seninf_route_rejects_unproved_paths),
	KUNIT_CASE(mt6797_seninf_register_plan_succeeds),
	KUNIT_CASE(mt6797_seninf_register_plan_rolls_back),
	{}
};

static struct kunit_suite mt6797_seninf_test_suite = {
	.name = "mt6797-seninf-core",
	.test_cases = mt6797_seninf_test_cases,
};

kunit_test_suite(mt6797_seninf_test_suite);

MODULE_LICENSE("GPL");
