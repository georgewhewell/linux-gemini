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

static struct kunit_case mt6797_h264_cases[] = {
	KUNIT_CASE(mt6797_h264_sps_pack_test),
	KUNIT_CASE(mt6797_h264_pps_pack_test),
	KUNIT_CASE(mt6797_h264_reject_inter_test),
	{}
};

static struct kunit_suite mt6797_h264_suite = {
	.name = "mt6797-h264-registers",
	.test_cases = mt6797_h264_cases,
};

kunit_test_suite(mt6797_h264_suite);

MODULE_LICENSE("GPL");
