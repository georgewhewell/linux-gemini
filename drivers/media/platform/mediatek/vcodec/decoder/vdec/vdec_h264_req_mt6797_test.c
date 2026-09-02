// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "vdec_h264_req_mt6797.h"

static void mt6797_h264_sps_pack_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.log2_max_frame_num_minus4 = 4,
		.pic_order_cnt_type = 0,
		.log2_max_pic_order_cnt_lsb_minus4 = 6,
		.max_num_ref_frames = 4,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
			 V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_sps_reg(&sps), 0x348611U);
}

static void mt6797_h264_pps_pack_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_pps pps = {
		.pic_init_qp_minus26 = -3,
		.chroma_qp_index_offset = -2,
		.second_chroma_qp_index_offset = 3,
		.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
			 V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_pps_reg(&pps), 0x19f7a1U);
}

static void mt6797_h264_slice2_pack_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_slice_params slice = {
		.cabac_init_idc = 2,
		.slice_qp_delta = -3,
		.disable_deblocking_filter_idc = 2,
		.slice_alpha_c0_offset_div2 = -2,
		.slice_beta_offset_div2 = 3,
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.nal_ref_idc = 3,
		.flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_slice2_reg(&slice, &dec), 0x2e7d7dU);
}

static void mt6797_h264_reject_inter_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	struct v4l2_ctrl_h264_pps pps = {};
	struct v4l2_ctrl_h264_slice_params slice = {
		.slice_type = V4L2_H264_SLICE_TYPE_P,
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_validate_request(&sps, &pps, &slice, &dec),
			-EOPNOTSUPP);
}

static void mt6797_h264_1080p_layout_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(1920, 1080, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.width, 1920U);
	KUNIT_EXPECT_EQ(test, layout.height, 1088U);
	KUNIT_EXPECT_EQ(test, layout.luma_size, 2088960U);
	KUNIT_EXPECT_EQ(test, layout.chroma_size, 1044480U);
	KUNIT_EXPECT_EQ(test, layout.mv_size, 522752U);
	KUNIT_EXPECT_EQ(test, layout.crc_span, 1566720U);
}

static void mt6797_h264_min_layout_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(16, 16, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.width, 16U);
	KUNIT_EXPECT_EQ(test, layout.height, 32U);
	KUNIT_EXPECT_EQ(test, layout.luma_size, 512U);
	KUNIT_EXPECT_EQ(test, layout.chroma_size, 256U);
	KUNIT_EXPECT_EQ(test, layout.mv_size, 640U);
	KUNIT_EXPECT_EQ(test, layout.crc_span, 512U);
}

static void mt6797_h264_reject_layout_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;

	KUNIT_EXPECT_EQ(test, mt6797_h264_buffer_layout(0, 1080, &layout), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_h264_buffer_layout(1921, 1080, &layout), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_h264_buffer_layout(1920, 1089, &layout), -ERANGE);
}

static void mt6797_h264_barrel_plan_test(struct kunit *test)
{
	struct mt6797_h264_barrel_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_barrel_plan(0x12340000, 0x20000, 65,
						      &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.start_addr, 0x12340000U);
	KUNIT_EXPECT_EQ(test, plan.end_addr, 0x12360000U);
	KUNIT_EXPECT_EQ(test, plan.fetch_start, 0x48d000U);
	KUNIT_EXPECT_EQ(test, plan.fetch_end, 0x48d804U);
	KUNIT_EXPECT_EQ(test, plan.shift_reads, 2U);
	KUNIT_EXPECT_EQ(test, plan.final_shift, 25U);
	KUNIT_EXPECT_EQ(test, mt6797_h264_barrel_shift_chunk(65), 32U);
	KUNIT_EXPECT_EQ(test, mt6797_h264_barrel_shift_chunk(1), 1U);
}

static void mt6797_h264_reject_barrel_plan_test(struct kunit *test)
{
	struct mt6797_h264_barrel_plan plan;

	KUNIT_EXPECT_EQ(test, mt6797_h264_barrel_plan(0x12340001, 0x1000, 8,
						      &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_h264_barrel_plan(0x12340000, 4, 32, &plan),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_h264_barrel_plan(0x12340000, 0x1000, 8,
						      &plan), -EINVAL);
	KUNIT_ASSERT_EQ(test, mt6797_h264_barrel_plan(0x12340000, 0x1000, 40,
						      &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.shift_reads, 1U);
	KUNIT_EXPECT_EQ(test, plan.final_shift, 32U);
}

static void mt6797_h264_output_plan_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;
	struct mt6797_h264_output_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(1920, 1080, &layout), 0);
	KUNIT_ASSERT_EQ(test, mt6797_h264_output_plan(0x10000000, 0x101fe000,
						      0x20000000, &layout,
						      &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.y_addr_512, 0x80000U);
	KUNIT_EXPECT_EQ(test, plan.c_addr_256, 0x101fe0U);
	KUNIT_EXPECT_EQ(test, plan.end_addr_16, 0x102fd00U);
	KUNIT_EXPECT_EQ(test, plan.y_addr_16, 0x1000000U);
	KUNIT_EXPECT_EQ(test, plan.y_addr, 0x10000000U);
	KUNIT_EXPECT_EQ(test, plan.c_offset, 2088960U);
	KUNIT_EXPECT_EQ(test, plan.mv_addr_16, 0x2000000U);
	KUNIT_EXPECT_EQ(test, plan.crc_y_addr, 0x10000000U);
	KUNIT_EXPECT_EQ(test, plan.crc_end_addr, 0x1017e800U);
}

static void mt6797_h264_reject_split_output_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;
	struct mt6797_h264_output_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(1920, 1080, &layout), 0);
	KUNIT_EXPECT_EQ(test, mt6797_h264_output_plan(0x10000000, 0x11000000,
						      0x20000000, &layout,
						      &plan), -EOPNOTSUPP);
}

static void mt6797_h264_working_plan_test(struct kunit *test)
{
	struct mt6797_h264_working_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_working_plan(0x10000000, 0x10008000,
						       0x10026000, 0x20000000,
						       &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.prediction_addr, 0x10000000U);
	KUNIT_EXPECT_EQ(test, plan.pp_addr, 0x10008000U);
	KUNIT_EXPECT_EQ(test, plan.ld_addr, 0x10026000U);
	KUNIT_EXPECT_EQ(test, plan.mv_addr_16, 0x2000000U);
	KUNIT_EXPECT_EQ(test, mt6797_h264_working_plan(0x10000001, 0x10008000,
						       0x10026000, 0x20000000,
						       &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_h264_working_plan(0x10000000, 0x10008000,
						       0x10026000, BIT_ULL(32),
						       &plan), -ERANGE);
}

static void mt6797_h264_idr_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.log2_max_frame_num_minus4 = 4,
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.frame_num = 7,
		.top_field_order_cnt = -2,
		.bottom_field_order_cnt = 3,
		.flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
	};
	struct mt6797_h264_idr_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_idr_plan(&sps, &dec, 0x10000000,
						   0x20000000, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.max_frame_num, 256U);
	KUNIT_EXPECT_EQ(test, plan.frame_num, 7U);
	KUNIT_EXPECT_EQ(test, plan.poc, 0x3fffeU);
	KUNIT_EXPECT_EQ(test, plan.top_poc, 0x3fffeU);
	KUNIT_EXPECT_EQ(test, plan.bottom_poc, 3U);
	KUNIT_EXPECT_EQ(test, plan.ref_y_addr, 0x10000000U);
	KUNIT_EXPECT_EQ(test, plan.ref_mv_addr_16, 0x2000000U);
	dec.flags = 0;
	KUNIT_EXPECT_EQ(test, mt6797_h264_idr_plan(&sps, &dec, 0x10000000,
						   0x20000000, &plan), -EINVAL);
}

static struct kunit_case mt6797_h264_cases[] = {
	KUNIT_CASE(mt6797_h264_sps_pack_test),
	KUNIT_CASE(mt6797_h264_pps_pack_test),
	KUNIT_CASE(mt6797_h264_slice2_pack_test),
	KUNIT_CASE(mt6797_h264_reject_inter_test),
	KUNIT_CASE(mt6797_h264_1080p_layout_test),
	KUNIT_CASE(mt6797_h264_min_layout_test),
	KUNIT_CASE(mt6797_h264_reject_layout_test),
	KUNIT_CASE(mt6797_h264_barrel_plan_test),
	KUNIT_CASE(mt6797_h264_reject_barrel_plan_test),
	KUNIT_CASE(mt6797_h264_output_plan_test),
	KUNIT_CASE(mt6797_h264_reject_split_output_test),
	KUNIT_CASE(mt6797_h264_working_plan_test),
	KUNIT_CASE(mt6797_h264_idr_plan_test),
	{}
};

static struct kunit_suite mt6797_h264_suite = {
	.name = "mt6797-h264-registers",
	.test_cases = mt6797_h264_cases,
};

kunit_test_suite(mt6797_h264_suite);

MODULE_LICENSE("GPL");
