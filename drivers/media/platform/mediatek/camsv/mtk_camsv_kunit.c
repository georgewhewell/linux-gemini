// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/videodev2.h>

#include "mtk_camsv_core.h"

static const struct mt6797_camsv_reg *
mt6797_camsv_find_reg(struct kunit *test,
		      const struct mt6797_camsv_reg *plan,
		      unsigned int count, u16 offset)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (plan[i].offset == offset)
			return &plan[i];

	KUNIT_FAIL(test, "register %#x is absent", offset);
	return NULL;
}

static void mt6797_camsv_format_test(struct kunit *test)
{
	struct mt6797_camsv_format format = {
		.width = MT6797_CAMSV_WIDTH,
		.height = MT6797_CAMSV_HEIGHT,
		.bytesperline = MT6797_CAMSV_BYTESPERLINE,
		.sizeimage = MT6797_CAMSV_SIZEIMAGE,
		.pixelformat = V4L2_PIX_FMT_SGRBG10P,
	};

	KUNIT_EXPECT_EQ(test, mt6797_camsv_format_validate(&format), 0);
	format.width = 1296;
	KUNIT_EXPECT_EQ(test, mt6797_camsv_format_validate(&format), -EINVAL);
	format.width = MT6797_CAMSV_WIDTH;
	format.pixelformat = V4L2_PIX_FMT_SGRBG10;
	KUNIT_EXPECT_EQ(test, mt6797_camsv_format_validate(&format), -EINVAL);
}

static void mt6797_camsv_plan_test(struct kunit *test)
{
	struct mt6797_camsv_reg plan[24];
	const struct mt6797_camsv_reg *reg;
	unsigned int count;

	count = mt6797_camsv_build_config_plan(plan, ARRAY_SIZE(plan));
	KUNIT_ASSERT_LE(test, count, (unsigned int)ARRAY_SIZE(plan));

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_FMT_SEL);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_EQ(test, reg->value, 1U);

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_IMGO_XSIZE);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_EQ(test, reg->value, MT6797_CAMSV_BYTESPERLINE - 1);

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_IMGO_STRIDE);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_EQ(test, reg->value,
			MT6797_CAMSV_BYTESPERLINE | 0x01010000U);

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_FBC_CTL1);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_EQ(test, reg->value, BIT(22) | BIT(16) | BIT(15));

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_DMA_FH_EN);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_EQ(test, reg->value, 0U);

	reg = mt6797_camsv_find_reg(test, plan, count,
				    MT6797_CAMSV_REG_INT_EN);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	KUNIT_EXPECT_TRUE(test, reg->value & MT6797_CAMSV_INT_SW_DONE);
}

static void mt6797_camsv_short_plan_test(struct kunit *test)
{
	struct mt6797_camsv_reg plan[1] = {};
	unsigned int count;

	count = mt6797_camsv_build_config_plan(plan, ARRAY_SIZE(plan));
	KUNIT_EXPECT_GT(test, count, (unsigned int)ARRAY_SIZE(plan));
	KUNIT_EXPECT_EQ(test, plan[0].offset, (u16)0);
}

static struct kunit_case mt6797_camsv_test_cases[] = {
	KUNIT_CASE(mt6797_camsv_format_test),
	KUNIT_CASE(mt6797_camsv_plan_test),
	KUNIT_CASE(mt6797_camsv_short_plan_test),
	{}
};

static struct kunit_suite mt6797_camsv_test_suite = {
	.name = "mt6797-camsv",
	.test_cases = mt6797_camsv_test_cases,
};

kunit_test_suite(mt6797_camsv_test_suite);

MODULE_LICENSE("GPL");
