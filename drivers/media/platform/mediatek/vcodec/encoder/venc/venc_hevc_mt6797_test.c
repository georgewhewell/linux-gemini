// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "venc_hevc_mt6797.h"

static void mt6797_venc_hevc_rcpu_size_test(struct kunit *test)
{
	u8 *data;
	int ret;

	data = kunit_kzalloc(test, MT6797_VENC_HEVC_RCPU_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);

	ret = mt6797_venc_hevc_validate_rcpu(data, MT6797_VENC_HEVC_RCPU_SIZE);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_venc_hevc_validate_rcpu(data,
					     MT6797_VENC_HEVC_RCPU_SIZE - 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_validate_rcpu(data,
					     MT6797_VENC_HEVC_RCPU_SIZE + 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_validate_rcpu(NULL, MT6797_VENC_HEVC_RCPU_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_venc_hevc_work_buf_1080p_test(struct kunit *test)
{
	struct mt6797_venc_hevc_work_buf_sizes sizes = {};
	int ret;

	ret = mt6797_venc_hevc_calc_buf_sizes(1920, 1080, &sizes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_RC_INFO],
			(size_t)0x400);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_REC_LUMA],
			(size_t)2088960);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA],
			(size_t)1044480);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1],
			(size_t)32640);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_SPS],
			(size_t)0x100000);
}

static void mt6797_venc_hevc_work_buf_alignment_test(struct kunit *test)
{
	struct mt6797_venc_hevc_work_buf_sizes sizes = {};
	int ret;

	ret = mt6797_venc_hevc_calc_buf_sizes(1, 1, &sizes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_REC_LUMA],
			(size_t)1024);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA],
			(size_t)512);
	KUNIT_EXPECT_EQ(test, sizes.size[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1],
			(size_t)32);
}

static void mt6797_venc_hevc_work_buf_reject_test(struct kunit *test)
{
	struct mt6797_venc_hevc_work_buf_sizes sizes = {};
	int ret;

	ret = mt6797_venc_hevc_calc_buf_sizes(0, 1080, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_calc_buf_sizes(1920, 0, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_calc_buf_sizes(1920, 1080, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_calc_buf_sizes(U32_MAX, U32_MAX, &sizes);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
}

static void mt6797_venc_hevc_io_regs_test(struct kunit *test)
{
	struct mt6797_venc_hevc_io_bufs bufs = {
		.src_addr = 0x10000000,
		.src_size = 0x2fd000,
		.buf_width = 1920,
		.buf_height = 1088,
		.bs_addr = 0x20000000,
		.bs_size = 0x100000,
	};
	struct mt6797_venc_hevc_io_regs regs = {};
	int ret;

	ret = mt6797_venc_hevc_calc_io_regs(&bufs, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.src_y, (u32)0x01000000);
	KUNIT_EXPECT_EQ(test, regs.src_c, (u32)0x0101fe00);
	KUNIT_EXPECT_EQ(test, regs.bs_addr, (u32)0x02000000);
	KUNIT_EXPECT_EQ(test, regs.bs_limit, (u32)0x2000);
}

static void mt6797_venc_hevc_io_regs_reject_test(struct kunit *test)
{
	struct mt6797_venc_hevc_io_bufs bufs = {
		.src_addr = 0x10000000,
		.src_size = 0x2fd000,
		.buf_width = 1920,
		.buf_height = 1088,
		.bs_addr = 0x20000000,
		.bs_size = 0x100000,
	};
	struct mt6797_venc_hevc_io_regs regs = {};

	bufs.src_size--;
	KUNIT_EXPECT_EQ(test, mt6797_venc_hevc_calc_io_regs(&bufs, &regs),
			-EINVAL);
	bufs.src_size++;
	bufs.buf_height = 1080;
	KUNIT_EXPECT_EQ(test, mt6797_venc_hevc_calc_io_regs(&bufs, &regs),
			-EINVAL);
	bufs.buf_height = 1088;
	bufs.bs_addr += 0x40;
	KUNIT_EXPECT_EQ(test, mt6797_venc_hevc_calc_io_regs(&bufs, &regs),
			-ERANGE);
}

static struct kunit_case mt6797_venc_hevc_cases[] = {
	KUNIT_CASE(mt6797_venc_hevc_rcpu_size_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_1080p_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_alignment_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_reject_test),
	KUNIT_CASE(mt6797_venc_hevc_io_regs_test),
	KUNIT_CASE(mt6797_venc_hevc_io_regs_reject_test),
	{}
};

static struct kunit_suite mt6797_venc_hevc_suite = {
	.name = "mt6797-hevc-encoder",
	.test_cases = mt6797_venc_hevc_cases,
};

kunit_test_suite(mt6797_venc_hevc_suite);

MODULE_LICENSE("GPL");
