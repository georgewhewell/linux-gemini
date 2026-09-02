// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "venc_h264_mt6797.h"

static void mt6797_venc_h264_rcpu_size_test(struct kunit *test)
{
	u8 *data;
	int ret;

	data = kunit_kzalloc(test, MT6797_VENC_H264_RCPU_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);

	ret = mt6797_venc_h264_validate_rcpu(data, MT6797_VENC_H264_RCPU_SIZE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_venc_h264_validate_rcpu(data, MT6797_VENC_H264_RCPU_SIZE - 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_h264_validate_rcpu(data, MT6797_VENC_H264_RCPU_SIZE + 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_h264_validate_rcpu(NULL, MT6797_VENC_H264_RCPU_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_venc_h264_cases[] = {
	KUNIT_CASE(mt6797_venc_h264_rcpu_size_test),
	{}
};

static struct kunit_suite mt6797_venc_h264_suite = {
	.name = "mt6797-h264-encoder",
	.test_cases = mt6797_venc_h264_cases,
};

kunit_test_suite(mt6797_venc_h264_suite);

MODULE_LICENSE("GPL");
