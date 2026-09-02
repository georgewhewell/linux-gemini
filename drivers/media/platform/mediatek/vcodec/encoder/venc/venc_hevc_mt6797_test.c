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

static void mt6797_venc_hevc_header_regs_test(struct kunit *test)
{
	struct mt6797_venc_hevc_header_regs regs = {};
	int ret;

	ret = mt6797_venc_hevc_header_regs(MT6797_VENC_HEVC_HEADER_SPS, true,
					   &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.mode, (u32)0x02083ff5);
	KUNIT_EXPECT_EQ(test, regs.start, (u32)0x1);
	KUNIT_EXPECT_EQ(test, regs.expected_irq, (u32)0x1);

	ret = mt6797_venc_hevc_header_regs(MT6797_VENC_HEVC_HEADER_VPS, false,
					   &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.mode, (u32)0x02003ff5);
	KUNIT_EXPECT_EQ(test, regs.start, (u32)0x20);
	KUNIT_EXPECT_EQ(test, regs.expected_irq, (u32)0x80);

	ret = mt6797_venc_hevc_header_regs(MT6797_VENC_HEVC_HEADER_PPS, false,
					   &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.mode, (u32)0x02003ff5);
	KUNIT_EXPECT_EQ(test, regs.start, (u32)0x2);
	KUNIT_EXPECT_EQ(test, regs.expected_irq, (u32)0x2);

	ret = mt6797_venc_hevc_header_regs(MT6797_VENC_HEVC_HEADER_VPS, true,
					   &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_header_regs(3, false, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_venc_hevc_header_regs(MT6797_VENC_HEVC_HEADER_SPS, true,
					   NULL);
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

static void mt6797_venc_hevc_seq_regs_1080p_test(struct kunit *test)
{
	struct mt6797_venc_hevc_seq_params params = {
		.width = 1920,
		.height = 1080,
		.buf_width = 1920,
		.buf_height = 1088,
		.intra_period = 30,
		.bitrate = 4000000,
		.framerate = 30,
		.level_idc = 40,
	};
	struct mt6797_venc_hevc_seq_regs regs = {};
	int ret;

	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_004, (u32)0x7007801e);
	KUNIT_EXPECT_EQ(test, regs.reg_008, (u32)0x000073a0);
	KUNIT_EXPECT_EQ(test, regs.reg_00c, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_024, (u32)0x00440078);
	KUNIT_EXPECT_EQ(test, regs.reg_028, (u32)0x0783c008);
	KUNIT_EXPECT_EQ(test, regs.reg_02c, (u32)0x000007f8);
	KUNIT_EXPECT_EQ(test, regs.reg_030, (u32)0x2a000000);
	KUNIT_EXPECT_EQ(test, regs.reg_038, (u32)0x00002123);
	KUNIT_EXPECT_EQ(test, regs.reg_048_header, (u32)0x00060fa0);
	KUNIT_EXPECT_EQ(test, regs.reg_048_frame, (u32)0x00040fa0);
	KUNIT_EXPECT_EQ(test, regs.reg_04c, (u32)0x001e0306);
	KUNIT_EXPECT_EQ(test, regs.reg_050, (u32)0x00003308);
	KUNIT_EXPECT_EQ(test, regs.reg_054, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_128, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_12c, (u32)0x00000600);
	KUNIT_EXPECT_EQ(test, regs.reg_1c0, (u32)0x00000010);
	KUNIT_EXPECT_EQ(test, regs.reg_1f0, (u32)0x00000002);
}

static void mt6797_venc_hevc_seq_regs_modes_test(struct kunit *test)
{
	struct mt6797_venc_hevc_seq_params params = {
		.width = 1920,
		.height = 1080,
		.buf_width = 1920,
		.buf_height = 1088,
		.intra_period = 60,
		.bitrate = 8000000,
		.framerate = 60,
		.level_idc = 41,
	};
	struct mt6797_venc_hevc_seq_regs regs = {};
	int ret;

	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_038, (u32)0x00002923);

	params.width = 3840;
	params.height = 2176;
	params.buf_width = 3840;
	params.buf_height = 2176;
	params.framerate = 30;
	params.level_idc = 50;
	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_024, (u32)0x008800f0);
	KUNIT_EXPECT_EQ(test, regs.reg_028, (u32)0x0f078000);
	KUNIT_EXPECT_EQ(test, regs.reg_02c, (u32)0x00001fe0);
	KUNIT_EXPECT_EQ(test, regs.reg_030, (u32)0x2c800000);
	KUNIT_EXPECT_EQ(test, regs.reg_038, (u32)0x00003d23);
}

static void mt6797_venc_hevc_seq_regs_reject_test(struct kunit *test)
{
	struct mt6797_venc_hevc_seq_params params = {
		.width = 1920,
		.height = 1080,
		.buf_width = 1920,
		.buf_height = 1088,
		.intra_period = 30,
		.bitrate = 4000000,
		.framerate = 30,
		.level_idc = 40,
	};
	struct mt6797_venc_hevc_seq_regs regs = {};
	int ret;

	params.buf_height = 1080;
	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	params.buf_height = 1088;
	params.level_idc = 42;
	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	params.level_idc = 40;
	params.bitrate = 60000001;
	ret = mt6797_venc_hevc_calc_seq_regs(&params, &regs);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_venc_hevc_frame_regs_test(struct kunit *test)
{
	struct mt6797_venc_hevc_frame_params params = {
		.intra_period = 30,
		.seq_reg_038 = 0x2123,
		.sao_luma = true,
		.sao_chroma = true,
	};
	struct mt6797_venc_hevc_frame_regs regs = {};
	int ret;

	ret = mt6797_hevc_frame_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_010, (u32)0x800e);
	KUNIT_EXPECT_EQ(test, regs.reg_014, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_018, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_01c, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.reg_038, (u32)0x21bb);
	KUNIT_EXPECT_EQ(test, regs.mode, (u32)0x0f603ff5);
	KUNIT_EXPECT_TRUE(test, regs.is_idr);
	KUNIT_EXPECT_TRUE(test, regs.next_idr_toggle);

	params.frame_num = 1;
	params.idr_toggle = regs.next_idr_toggle;
	params.last_idr_frame = regs.next_last_idr_frame;
	ret = mt6797_hevc_frame_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_010, (u32)0x8008);
	KUNIT_EXPECT_EQ(test, regs.reg_014, (u32)1);
	KUNIT_EXPECT_EQ(test, regs.reg_018, (u32)1);
	KUNIT_EXPECT_EQ(test, regs.reg_01c, (u32)0);
	KUNIT_EXPECT_EQ(test, regs.mode, (u32)0x0fa03ff5);
	KUNIT_EXPECT_FALSE(test, regs.is_idr);

	params.frame_num = 2;
	ret = mt6797_hevc_frame_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_010, (u32)0x8108);

	params.frame_num = 30;
	ret = mt6797_hevc_frame_regs(&params, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.reg_010, (u32)0x8006);
	KUNIT_EXPECT_TRUE(test, regs.is_idr);
	KUNIT_EXPECT_FALSE(test, regs.next_idr_toggle);
	KUNIT_EXPECT_EQ(test, regs.next_last_idr_frame, (u32)30);
}

static void mt6797_venc_hevc_ref_regs_test(struct kunit *test)
{
	u32 work_buf_regs[MT6797_VENC_HEVC_WORK_BUF_MAX] = {
		[MT6797_VENC_HEVC_WORK_BUF_REC_LUMA] = 0x10,
		[MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA] = 0x20,
		[MT6797_VENC_HEVC_WORK_BUF_REF_LUMA] = 0x30,
		[MT6797_VENC_HEVC_WORK_BUF_REF_CHROMA] = 0x40,
		[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1] = 0x50,
		[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_2] = 0x60,
	};
	struct mt6797_venc_hevc_ref_regs regs = {};
	int ret;

	ret = mt6797_hevc_ref_regs(work_buf_regs, 0, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.ref_luma, (u32)0x30);
	KUNIT_EXPECT_EQ(test, regs.ref_chroma, (u32)0x40);
	KUNIT_EXPECT_EQ(test, regs.rec_luma, (u32)0x10);
	KUNIT_EXPECT_EQ(test, regs.rec_chroma, (u32)0x20);
	KUNIT_EXPECT_EQ(test, regs.mv_info_1, (u32)0x50);
	KUNIT_EXPECT_EQ(test, regs.mv_info_2, (u32)0x60);

	ret = mt6797_hevc_ref_regs(work_buf_regs, 1, &regs);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, regs.ref_luma, (u32)0x10);
	KUNIT_EXPECT_EQ(test, regs.ref_chroma, (u32)0x20);
	KUNIT_EXPECT_EQ(test, regs.rec_luma, (u32)0x30);
	KUNIT_EXPECT_EQ(test, regs.rec_chroma, (u32)0x40);
	KUNIT_EXPECT_EQ(test, regs.mv_info_1, (u32)0x60);
	KUNIT_EXPECT_EQ(test, regs.mv_info_2, (u32)0x50);
}

static struct kunit_case mt6797_venc_hevc_cases[] = {
	KUNIT_CASE(mt6797_venc_hevc_rcpu_size_test),
	KUNIT_CASE(mt6797_venc_hevc_header_regs_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_1080p_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_alignment_test),
	KUNIT_CASE(mt6797_venc_hevc_work_buf_reject_test),
	KUNIT_CASE(mt6797_venc_hevc_io_regs_test),
	KUNIT_CASE(mt6797_venc_hevc_io_regs_reject_test),
	KUNIT_CASE(mt6797_venc_hevc_seq_regs_1080p_test),
	KUNIT_CASE(mt6797_venc_hevc_seq_regs_modes_test),
	KUNIT_CASE(mt6797_venc_hevc_seq_regs_reject_test),
	KUNIT_CASE(mt6797_venc_hevc_frame_regs_test),
	KUNIT_CASE(mt6797_venc_hevc_ref_regs_test),
	{}
};

static struct kunit_suite mt6797_venc_hevc_suite = {
	.name = "mt6797-hevc-encoder",
	.test_cases = mt6797_venc_hevc_cases,
};

kunit_test_suite(mt6797_venc_hevc_suite);

MODULE_LICENSE("GPL");
