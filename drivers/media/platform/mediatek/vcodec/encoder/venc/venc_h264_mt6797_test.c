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

static void mt6797_venc_h264_dma_reg_test(struct kunit *test)
{
	u32 reg = 0;
	int ret;

	ret = mt6797_venc_h264_dma_reg(0x12340080, 0x1000, 0x80, &reg);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, (u32)0x01234008);

	ret = mt6797_venc_h264_dma_reg(0x12340040, 0x1000, 0x80, &reg);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	ret = mt6797_venc_h264_dma_reg(0xfffff000, 0x1001, 0x80, &reg);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	ret = mt6797_venc_h264_dma_reg(0x12340080, 0, 0x80, &reg);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

static void mt6797_venc_h264_io_regs_test(struct kunit *test)
{
	struct mt6797_venc_h264_io_bufs bufs = {
		.src_addr = 0x10000000,
		.src_size = 0x2fd000,
		.buf_width = 1920,
		.buf_height = 1088,
		.bs_addr = 0x20000000,
		.bs_size = 0x100000,
	};
	struct mt6797_venc_h264_io_regs regs = { };
	int ret;

	ret = mt6797_venc_h264_calc_io_regs(&bufs, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.src_y, (u32)0x01000000);
	KUNIT_EXPECT_EQ(test, regs.src_c, (u32)0x0101fe00);
	KUNIT_EXPECT_EQ(test, regs.bs_addr, (u32)0x02000000);
	KUNIT_EXPECT_EQ(test, regs.bs_limit, (u32)0x2000);
}

static void mt6797_venc_h264_io_regs_reject_test(struct kunit *test)
{
	struct mt6797_venc_h264_io_bufs bufs = {
		.src_addr = 0x10000000,
		.src_size = 0x2fd000,
		.buf_width = 1920,
		.buf_height = 1088,
		.bs_addr = 0x20000000,
		.bs_size = 0x100000,
	};
	struct mt6797_venc_h264_io_regs regs = { };
	int ret;

	bufs.src_size--;
	ret = mt6797_venc_h264_calc_io_regs(&bufs, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	bufs.src_size++;
	bufs.bs_addr += 0x40;
	ret = mt6797_venc_h264_calc_io_regs(&bufs, &regs);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	bufs.bs_addr -= 0x40;
	bufs.bs_size--;
	ret = mt6797_venc_h264_calc_io_regs(&bufs, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_venc_h264_cases[] = {
	KUNIT_CASE(mt6797_venc_h264_rcpu_size_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_1080p_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_alignment_test),
	KUNIT_CASE(mt6797_venc_h264_work_buf_reject_test),
	KUNIT_CASE(mt6797_venc_h264_dma_reg_test),
	KUNIT_CASE(mt6797_venc_h264_io_regs_test),
	KUNIT_CASE(mt6797_venc_h264_io_regs_reject_test),
	{}
};

static struct kunit_suite mt6797_venc_h264_suite = {
	.name = "mt6797-h264-encoder",
	.test_cases = mt6797_venc_h264_cases,
};

kunit_test_suite(mt6797_venc_h264_suite);

MODULE_LICENSE("GPL");
