// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "vdec_h264_req_mt6797.h"

struct mt6797_h264_test_bits {
	u8 data[64];
	u32 bits;
};

static void
mt6797_h264_test_bit(struct mt6797_h264_test_bits *stream, bool value)
{
	u32 bit = stream->bits++;

	if (value)
		stream->data[4 + bit / 8] |= BIT(7 - bit % 8);
}

static void
mt6797_h264_test_bits(struct mt6797_h264_test_bits *stream, u32 value, u8 count)
{
	while (count--)
		mt6797_h264_test_bit(stream, value & BIT(count));
}

static void mt6797_h264_test_ue(struct mt6797_h264_test_bits *stream, u32 value)
{
	u32 code = value + 1;
	u8 bits = fls(code);
	u8 i;

	for (i = 1; i < bits; i++)
		mt6797_h264_test_bit(stream, false);
	mt6797_h264_test_bits(stream, code, bits);
}

static void mt6797_h264_test_se(struct mt6797_h264_test_bits *stream, s32 value)
{
	u32 code = value <= 0 ? -2 * value : 2 * value - 1;

	mt6797_h264_test_ue(stream, code);
}

static size_t mt6797_h264_test_size(const struct mt6797_h264_test_bits *stream)
{
	return 4 + DIV_ROUND_UP(stream->bits, 8);
}

static void mt6797_test_idr(struct mt6797_h264_test_bits *stream, u32 first_mb,
			    s32 qp_delta)
{
	static const u8 prefix[] = { 0, 0, 1, 0x65 };

	memset(stream, 0, sizeof(*stream));
	memcpy(stream->data, prefix, sizeof(prefix));
	mt6797_h264_test_ue(stream, first_mb);
	mt6797_h264_test_ue(stream, V4L2_H264_SLICE_TYPE_I);
	mt6797_h264_test_ue(stream, 0);	/* pic_parameter_set_id */
	mt6797_h264_test_bits(stream, 0, 4);	/* frame_num */
	mt6797_h264_test_ue(stream, 0);	/* idr_pic_id */
	mt6797_h264_test_bits(stream, 0, 2);	/* reference marking */
	mt6797_h264_test_se(stream, qp_delta);
}

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

static void mt6797_h264_pps_id_pack_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_pps pps = {
		.pic_parameter_set_id = 17,
		.seq_parameter_set_id = 9,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_pps_id_reg(&pps), 0x131U);
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

static void mt6797_h264_slice_pack_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_slice_params slice = {
		.first_mb_in_slice = 0x123,
		.slice_type = V4L2_H264_SLICE_TYPE_B,
		.num_ref_idx_l0_active_minus1 = 2,
		.num_ref_idx_l1_active_minus1 = 1,
		.flags = V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED,
	};

	KUNIT_EXPECT_EQ(test, mt6797_h264_slice_reg(&slice, false, false),
			0x2282123U);
}

static void mt6797_h264_validate_inter_test(struct kunit *test)
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

	KUNIT_EXPECT_EQ(test, mt6797_h264_validate_request(&sps, &pps, &slice, &dec), 0);
	dec.flags = V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
	KUNIT_EXPECT_EQ(test, mt6797_h264_validate_request(&sps, &pps, &slice, &dec),
			-EINVAL);
	slice.first_mb_in_slice = 1;
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

static void mt6797_h264_staging_size_test(struct kunit *test)
{
	size_t allocation_size;

	KUNIT_ASSERT_EQ(test, mt6797_h264_staging_size(1000, &allocation_size), 0);
	KUNIT_EXPECT_EQ(test, allocation_size, (size_t)1280);
	KUNIT_EXPECT_EQ(test, mt6797_h264_staging_size(0, &allocation_size),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_h264_staging_size(SIZE_MAX - MT6797_H264_BS_GUARD + 1,
						 &allocation_size),
			-EOVERFLOW);
}

static void mt6797_h264_frame_header_test(struct kunit *test)
{
	struct mt6797_h264_test_bits stream = {
		.data = { 0, 0, 1, 0x41 },
	};
	struct v4l2_ctrl_h264_sps sps = {
		.log2_max_frame_num_minus4 = 0,
	};
	struct v4l2_ctrl_h264_pps pps = {
		.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
			 V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT,
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.nal_ref_idc = 2,
		.frame_num = 3,
		.pic_order_cnt_bit_size = 4,
		.dec_ref_pic_marking_bit_size = 3,
		.flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
	};
	struct v4l2_ctrl_h264_slice_params slice;
	size_t size;
	int ret;

	mt6797_h264_test_ue(&stream, 0);	/* first_mb_in_slice */
	mt6797_h264_test_ue(&stream, V4L2_H264_SLICE_TYPE_P);
	mt6797_h264_test_ue(&stream, 0);	/* pic_parameter_set_id */
	mt6797_h264_test_bits(&stream, 3, 4);	/* frame_num */
	mt6797_h264_test_bits(&stream, 0xa, 4);	/* POC syntax */
	mt6797_h264_test_bit(&stream, true);	/* ref count override */
	mt6797_h264_test_ue(&stream, 1);	/* two L0 references */
	mt6797_h264_test_bit(&stream, false);	/* default L0 ordering */
	mt6797_h264_test_bits(&stream, 5, 3);	/* marking syntax */
	mt6797_h264_test_ue(&stream, 2);	/* cabac_init_idc */
	mt6797_h264_test_se(&stream, -3);	/* slice_qp_delta */
	mt6797_h264_test_ue(&stream, 0);	/* deblocking enabled */
	mt6797_h264_test_se(&stream, -2);	/* alpha offset */
	mt6797_h264_test_se(&stream, 2);	/* beta offset */

	size = mt6797_h264_test_size(&stream);
	ret = mt6797_h264_parse_frame_header(stream.data, size, &sps, &pps, &dec, &slice);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, slice.header_bit_size, stream.bits + 8);
	KUNIT_EXPECT_EQ(test, slice.slice_type, (u8)V4L2_H264_SLICE_TYPE_P);
	KUNIT_EXPECT_EQ(test, slice.num_ref_idx_l0_active_minus1, (u8)1);
	KUNIT_EXPECT_EQ(test, slice.cabac_init_idc, (u8)2);
	KUNIT_EXPECT_EQ(test, slice.slice_qp_delta, (s8)-3);
	KUNIT_EXPECT_EQ(test, slice.disable_deblocking_filter_idc, (u8)0);
	KUNIT_EXPECT_EQ(test, slice.slice_alpha_c0_offset_div2, (s8)-2);
	KUNIT_EXPECT_EQ(test, slice.slice_beta_offset_div2, (s8)2);
}

static void mt6797_h264_reject_modified_list_test(struct kunit *test)
{
	struct mt6797_h264_test_bits stream = {
		.data = { 0, 0, 1, 0x01 },
	};
	static const struct v4l2_ctrl_h264_sps sps;
	static const struct v4l2_ctrl_h264_pps pps;
	static const struct v4l2_ctrl_h264_decode_params dec = {
		.flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
	};
	struct v4l2_ctrl_h264_slice_params slice;
	size_t size;
	int ret;

	mt6797_h264_test_ue(&stream, 0);
	mt6797_h264_test_ue(&stream, V4L2_H264_SLICE_TYPE_P);
	mt6797_h264_test_ue(&stream, 0);
	mt6797_h264_test_bits(&stream, 0, 4);
	mt6797_h264_test_bit(&stream, false);	/* default ref counts */
	mt6797_h264_test_bit(&stream, true);	/* modified L0 ordering */

	size = mt6797_h264_test_size(&stream);
	ret = mt6797_h264_parse_frame_header(stream.data, size, &sps, &pps, &dec, &slice);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	stream.data[0] = 1;
	ret = mt6797_h264_parse_frame_header(stream.data, size, &sps, &pps, &dec, &slice);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_h264_multi_slice_frame_test(struct kunit *test)
{
	struct mt6797_h264_test_bits first_stream, second_stream;
	static const struct v4l2_ctrl_h264_sps sps;
	static const struct v4l2_ctrl_h264_pps pps;
	static const struct v4l2_ctrl_h264_decode_params dec = {
		.nal_ref_idc = 3,
		.dec_ref_pic_marking_bit_size = 2,
		.flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
	};
	struct v4l2_ctrl_h264_slice_params slice;
	u8 frame[128];
	size_t first_size, second_size, frame_size;
	int ret;

	mt6797_test_idr(&first_stream, 0, 0);
	mt6797_test_idr(&second_stream, 10, 0);
	first_size = mt6797_h264_test_size(&first_stream);
	second_size = mt6797_h264_test_size(&second_stream);
	memcpy(frame, first_stream.data, first_size);
	memcpy(frame + first_size, second_stream.data, second_size);

	frame_size = first_size + second_size;
	ret = mt6797_h264_parse_frame_headers(frame, frame_size, &sps, &pps, &dec, &slice);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, slice.first_mb_in_slice, 0U);
	KUNIT_EXPECT_EQ(test, slice.slice_type, (u8)V4L2_H264_SLICE_TYPE_I);

	mt6797_test_idr(&second_stream, 10, 1);
	second_size = mt6797_h264_test_size(&second_stream);
	memcpy(frame + first_size, second_stream.data, second_size);
	frame_size = first_size + second_size;
	ret = mt6797_h264_parse_frame_headers(frame, frame_size, &sps, &pps, &dec, &slice);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
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

static void mt6797_h264_p_ref_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_slice_params slice = {
		.slice_type = V4L2_H264_SLICE_TYPE_P,
		.num_ref_idx_l0_active_minus1 = 1,
		.ref_pic_list0 = {
			{ .fields = V4L2_H264_FRAME_REF, .index = 4 },
			{ .fields = V4L2_H264_FRAME_REF, .index = 9 },
		},
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.dpb = {
			[4] = {
				.pic_num = 0xfffffff9,
				.fields = V4L2_H264_FRAME_REF,
				.top_field_order_cnt = -8,
				.bottom_field_order_cnt = -6,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
			[9] = {
				.pic_num = 0x12345,
				.frame_num = 3,
				.fields = V4L2_H264_FRAME_REF,
				.top_field_order_cnt = 20,
				.bottom_field_order_cnt = 18,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
					 V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM,
			},
		},
		.flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
	};
	struct mt6797_h264_ref_buffer buffers[V4L2_H264_NUM_DPB_ENTRIES] = {
		[4] = { .y_addr = 0x12000000, .dpb_slot = 2 },
		[9] = { .y_addr = 0x14000000, .dpb_slot = 15 },
	};
	struct mt6797_h264_p_ref_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_p_ref_plan(&slice, &dec, buffers,
						     &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.num_refs, 2);
	KUNIT_EXPECT_EQ(test, plan.refs[0].y_addr, 0x12000000U);
	KUNIT_EXPECT_EQ(test, plan.refs[0].pic_num, 0x7fff9U);
	KUNIT_EXPECT_EQ(test, plan.refs[0].top, 0x1200000U);
	KUNIT_EXPECT_EQ(test, plan.refs[0].bottom, 0x1600000U);
	KUNIT_EXPECT_EQ(test, plan.refs[1].y_addr, 0x14000000U);
	KUNIT_EXPECT_EQ(test, plan.refs[1].pic_num, 0x80003U);
	KUNIT_EXPECT_EQ(test, plan.refs[1].top, 0x7900000U);
	KUNIT_EXPECT_EQ(test, plan.refs[1].bottom, 0x7d00000U);
}

static void mt6797_h264_reject_bad_p_ref_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_slice_params slice = {
		.slice_type = V4L2_H264_SLICE_TYPE_P,
		.ref_pic_list0 = {
			{ .fields = V4L2_H264_TOP_FIELD_REF, .index = 0 },
		},
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.dpb = {
			[0] = {
				.fields = V4L2_H264_FRAME_REF,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
		},
		.flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
	};
	struct mt6797_h264_ref_buffer buffers[V4L2_H264_NUM_DPB_ENTRIES] = {
		[0] = { .y_addr = 0x12000000, .dpb_slot = 0 },
	};
	struct mt6797_h264_p_ref_plan plan;

	KUNIT_EXPECT_EQ(test, mt6797_h264_p_ref_plan(&slice, &dec, buffers,
						     &plan), -EINVAL);
	slice.ref_pic_list0[0].fields = V4L2_H264_FRAME_REF;
	dec.dpb[0].flags = V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
	KUNIT_EXPECT_EQ(test, mt6797_h264_p_ref_plan(&slice, &dec, buffers,
						     &plan), -EINVAL);
	dec.dpb[0].flags |= V4L2_H264_DPB_ENTRY_FLAG_VALID;
	buffers[0].y_addr++;
	KUNIT_EXPECT_EQ(test, mt6797_h264_p_ref_plan(&slice, &dec, buffers,
						     &plan), -EINVAL);
}

static void mt6797_h264_b_ref_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_slice_params slice = {
		.slice_type = V4L2_H264_SLICE_TYPE_B,
		.ref_pic_list0 = {
			{ .fields = V4L2_H264_FRAME_REF, .index = 4 },
		},
		.ref_pic_list1 = {
			{ .fields = V4L2_H264_FRAME_REF, .index = 9 },
		},
	};
	struct v4l2_ctrl_h264_decode_params dec = {
		.dpb = {
			[4] = {
				.pic_num = 0xfffffff9,
				.fields = V4L2_H264_FRAME_REF,
				.top_field_order_cnt = -8,
				.bottom_field_order_cnt = -6,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
			[9] = {
				.frame_num = 3,
				.fields = V4L2_H264_FRAME_REF,
				.top_field_order_cnt = 20,
				.bottom_field_order_cnt = 18,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
					 V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM,
			},
		},
		.flags = V4L2_H264_DECODE_PARAM_FLAG_BFRAME,
	};
	struct mt6797_h264_ref_buffer buffers[V4L2_H264_NUM_DPB_ENTRIES] = {
		[4] = {
			.y_addr = 0x12000000,
			.mv_addr = 0x18000000,
			.dpb_slot = 2,
		},
		[9] = {
			.y_addr = 0x14000000,
			.mv_addr = 0x20000000,
			.dpb_slot = 15,
		},
	};
	struct mt6797_h264_b_ref_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_b_ref_plan(&slice, &dec, buffers,
						     &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.num_l0, 1);
	KUNIT_EXPECT_EQ(test, plan.num_l1, 1);
	KUNIT_EXPECT_EQ(test, plan.l0[0].y_addr, 0x12000000U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].pic_num, 0x7fff9U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].top, 0x123fff8U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].bottom, 0x163fffaU);
	KUNIT_EXPECT_EQ(test, plan.l0[0].mv_top_addr_16, 0U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].y_addr, 0x14000000U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].pic_num, 0x80003U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].top, 0x7900014U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].bottom, 0x7d00012U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].mv_top_addr_16, 0x2000000U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].mv_bottom_addr_16, 0x2000004U);
}

static void mt6797_h264_mv_slot_lifetime_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_decode_params dec = {
		.dpb = {
			[4] = {
				.reference_ts = 100,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
			[9] = {
				.reference_ts = 200,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
		},
	};
	struct mt6797_h264_mv_slot slots[MT6797_H264_MV_BUFFER_COUNT] = {
		[0] = { .reference_ts = 50, .valid = true },
		[3] = { .reference_ts = 100, .valid = true },
		[15] = { .reference_ts = 200, .valid = true },
	};
	struct mt6797_h264_mv_slot_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_mv_slot_plan(&dec, slots, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.dpb_slots[4], 3);
	KUNIT_EXPECT_EQ(test, plan.dpb_slots[9], 15);
	KUNIT_EXPECT_EQ(test, plan.dpb_slots[0], U8_MAX);
	KUNIT_EXPECT_EQ(test, plan.current_slot, 0);

	KUNIT_ASSERT_EQ(test, mt6797_h264_mv_slot_begin(slots, plan.current_slot), 0);
	KUNIT_EXPECT_FALSE(test, slots[0].valid);
	KUNIT_ASSERT_EQ(test, mt6797_h264_mv_slot_commit(slots, plan.current_slot, 300), 0);
	KUNIT_EXPECT_TRUE(test, slots[0].valid);
	KUNIT_EXPECT_EQ(test, slots[0].reference_ts, 300ULL);
}

static void mt6797_h264_mv_slot_missing_ref_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_decode_params dec = {
		.dpb = {
			[0] = {
				.reference_ts = 100,
				.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
			},
		},
	};
	struct mt6797_h264_mv_slot slots[MT6797_H264_MV_BUFFER_COUNT] = {};
	struct mt6797_h264_mv_slot_plan plan;

	KUNIT_EXPECT_EQ(test, mt6797_h264_mv_slot_plan(&dec, slots, &plan),
			-ENOENT);
	dec.dpb[0].flags = V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
	KUNIT_EXPECT_EQ(test, mt6797_h264_mv_slot_plan(&dec, slots, &plan),
			-EINVAL);
}

static void mt6797_h264_mv_range_test(struct kunit *test)
{
	u64 addrs[MT6797_H264_MV_BUFFER_COUNT];
	struct mt6797_h264_mv_range range;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(addrs); i++)
		addrs[i] = 0x10000000 + (MT6797_H264_MV_BUFFER_COUNT - i) * 0x20000;

	KUNIT_ASSERT_EQ(test, mt6797_h264_mv_range_plan(addrs, 0x12000, &range), 0);
	KUNIT_EXPECT_EQ(test, range.min_addr, 0x10020000U);
	KUNIT_EXPECT_EQ(test, range.max_end, 0x10232000U);

	addrs[4]++;
	KUNIT_EXPECT_EQ(test, mt6797_h264_mv_range_plan(addrs, 0x12000, &range),
			-EINVAL);
	addrs[4] = BIT_ULL(32) - 0x1000;
	KUNIT_EXPECT_EQ(test, mt6797_h264_mv_range_plan(addrs, 0x12000, &range),
			-ERANGE);
}

static void mt6797_h264_start_plan_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;
	struct v4l2_ctrl_h264_pps pps = {
		.flags = V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED,
	};
	struct mt6797_h264_start_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(1920, 1080, &layout), 0);
	KUNIT_ASSERT_EQ(test, mt6797_h264_start_plan(1920, 1088, &layout, &pps,
						     &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.width, 1920U);
	KUNIT_EXPECT_EQ(test, plan.height, 1088U);
	KUNIT_EXPECT_EQ(test, plan.packed_size, 0x04400780U);
	KUNIT_EXPECT_EQ(test, plan.mb_width_minus1, 119U);
	KUNIT_EXPECT_EQ(test, plan.mb_height_minus1, 67U);
	KUNIT_EXPECT_EQ(test, plan.mb_width, 120U);
	KUNIT_EXPECT_EQ(test, plan.mb_count, 8160U);
	KUNIT_EXPECT_TRUE(test, plan.constrained_intra_pred);
}

static void mt6797_h264_padded_start_plan_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout;
	struct v4l2_ctrl_h264_pps pps = {};
	struct mt6797_h264_start_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_h264_buffer_layout(1280, 720, &layout), 0);
	KUNIT_ASSERT_EQ(test, mt6797_h264_start_plan(1280, 720, &layout, &pps,
						     &plan), 0);
	KUNIT_EXPECT_EQ(test, layout.height, 736U);
	KUNIT_EXPECT_EQ(test, plan.height, 720U);
	KUNIT_EXPECT_EQ(test, plan.mb_height_minus1, 44U);
	KUNIT_EXPECT_EQ(test, plan.mb_count, 3600U);
}

static void mt6797_h264_reject_start_plan_test(struct kunit *test)
{
	struct mt6797_h264_buffer_layout layout = {
		.width = 1921,
		.height = 1088,
	};
	struct v4l2_ctrl_h264_pps pps = {};
	struct mt6797_h264_start_plan plan;

	KUNIT_EXPECT_EQ(test, mt6797_h264_start_plan(1921, 1088, &layout, &pps,
						     &plan),
			-EINVAL);
	layout.width = 1920;
	layout.height = 0;
	KUNIT_EXPECT_EQ(test, mt6797_h264_start_plan(1920, 0, &layout, &pps,
						     &plan),
			-EINVAL);
}

static struct kunit_case mt6797_h264_cases[] = {
	KUNIT_CASE(mt6797_h264_sps_pack_test),
	KUNIT_CASE(mt6797_h264_pps_pack_test),
	KUNIT_CASE(mt6797_h264_pps_id_pack_test),
	KUNIT_CASE(mt6797_h264_slice_pack_test),
	KUNIT_CASE(mt6797_h264_slice2_pack_test),
	KUNIT_CASE(mt6797_h264_validate_inter_test),
	KUNIT_CASE(mt6797_h264_1080p_layout_test),
	KUNIT_CASE(mt6797_h264_min_layout_test),
	KUNIT_CASE(mt6797_h264_reject_layout_test),
	KUNIT_CASE(mt6797_h264_staging_size_test),
	KUNIT_CASE(mt6797_h264_frame_header_test),
	KUNIT_CASE(mt6797_h264_reject_modified_list_test),
	KUNIT_CASE(mt6797_h264_multi_slice_frame_test),
	KUNIT_CASE(mt6797_h264_barrel_plan_test),
	KUNIT_CASE(mt6797_h264_reject_barrel_plan_test),
	KUNIT_CASE(mt6797_h264_output_plan_test),
	KUNIT_CASE(mt6797_h264_reject_split_output_test),
	KUNIT_CASE(mt6797_h264_working_plan_test),
	KUNIT_CASE(mt6797_h264_idr_plan_test),
	KUNIT_CASE(mt6797_h264_p_ref_plan_test),
	KUNIT_CASE(mt6797_h264_reject_bad_p_ref_test),
	KUNIT_CASE(mt6797_h264_b_ref_plan_test),
	KUNIT_CASE(mt6797_h264_mv_slot_lifetime_test),
	KUNIT_CASE(mt6797_h264_mv_slot_missing_ref_test),
	KUNIT_CASE(mt6797_h264_mv_range_test),
	KUNIT_CASE(mt6797_h264_start_plan_test),
	KUNIT_CASE(mt6797_h264_padded_start_plan_test),
	KUNIT_CASE(mt6797_h264_reject_start_plan_test),
	{}
};

static struct kunit_suite mt6797_h264_suite = {
	.name = "mt6797-h264-registers",
	.test_cases = mt6797_h264_cases,
};

kunit_test_suite(mt6797_h264_suite);

MODULE_LICENSE("GPL");
