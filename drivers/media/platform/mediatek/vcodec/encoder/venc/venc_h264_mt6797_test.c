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

static void mt6797_venc_h264_work_buf_1080p_test(struct kunit *test)
{
	struct mt6797_venc_h264_work_buf_sizes sizes = { };
	int ret;

	ret = mt6797_venc_h264_calc_work_buf_sizes(1920, 1080, &sizes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_RC_INFO],
			(size_t)0x400);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_REC_LUMA],
			(size_t)2088960);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_REC_CHROMA],
			(size_t)1044480);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_MV_INFO_1],
			(size_t)65280);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_SKIP_FRAME],
			(size_t)0x80);
}

static void mt6797_venc_h264_work_buf_alignment_test(struct kunit *test)
{
	struct mt6797_venc_h264_work_buf_sizes sizes = { };
	int ret;

	ret = mt6797_venc_h264_calc_work_buf_sizes(1, 1, &sizes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_REC_LUMA],
			(size_t)512);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_REC_CHROMA],
			(size_t)256);
	KUNIT_EXPECT_EQ(test,
			sizes.size[MT6797_VENC_H264_WORK_BUF_MV_INFO_1],
			(size_t)128);
}

static void mt6797_venc_h264_work_buf_reject_test(struct kunit *test)
{
	struct mt6797_venc_h264_work_buf_sizes sizes = { };
	int ret;

	ret = mt6797_venc_h264_calc_work_buf_sizes(0, 1080, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_h264_calc_work_buf_sizes(1920, 0, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_h264_calc_work_buf_sizes(1920, 1080, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_h264_calc_work_buf_sizes(U32_MAX, U32_MAX, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
}

static struct kunit_case mt6797_venc_h264_cases[] = {
	KUNIT_CASE(mt6797_venc_h264_rcpu_size_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_1080p_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_alignment_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_reject_test),
	{}
};

static struct kunit_suite mt6797_venc_h264_suite = {
	.name = "mt6797-h264-encoder",
	.test_cases = mt6797_venc_h264_cases,
};

kunit_test_suite(mt6797_venc_h264_suite);

MODULE_LICENSE("GPL");
