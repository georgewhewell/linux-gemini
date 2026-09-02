// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/sizes.h>

#include "mtk_seninf_core.h"

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
	KUNIT_EXPECT_EQ(test, format.width,
			(u32)MT6797_SENINF_DEFAULT_WIDTH);
	KUNIT_EXPECT_EQ(test, format.height,
			(u32)MT6797_SENINF_DEFAULT_HEIGHT);
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
	{}
};

static struct kunit_suite mt6797_seninf_test_suite = {
	.name = "mt6797-seninf-core",
	.test_cases = mt6797_seninf_test_cases,
};

kunit_test_suite(mt6797_seninf_test_suite);

MODULE_LICENSE("GPL");
