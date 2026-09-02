// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "vdec_hevc_req_mt6797.h"

struct mt6797_hevc_test_bits {
	u8 data[32];
	u32 bit;
};

static void mt6797_hevc_test_put_bits(struct mt6797_hevc_test_bits *bits,
				      u32 value, u8 count)
{
	unsigned int i;

	for (i = 0; i < count; i++, bits->bit++) {
		u32 shift = count - i - 1;

		if (value & BIT(shift))
			bits->data[bits->bit >> 3] |= BIT(7 - (bits->bit & 7));
	}
}

static void mt6797_hevc_test_put_ue(struct mt6797_hevc_test_bits *bits,
				    u32 value)
{
	u32 code = value + 1;
	u8 width = fls(code);

	mt6797_hevc_test_put_bits(bits, 0, width - 1);
	mt6797_hevc_test_put_bits(bits, code, width);
}

static void mt6797_hevc_sps_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_pps pps = {
		.flags = V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT,
	};
	struct v4l2_ctrl_hevc_sps sps = {
		.pic_width_in_luma_samples = 1920,
		.pic_height_in_luma_samples = 1080,
		.log2_max_pic_order_cnt_lsb_minus4 = 4,
		.log2_diff_max_min_luma_coding_block_size = 3,
		.log2_diff_max_min_luma_transform_block_size = 3,
		.max_transform_hierarchy_depth_inter = 4,
		.max_transform_hierarchy_depth_intra = 3,
		.pcm_sample_bit_depth_luma_minus1 = 7,
		.pcm_sample_bit_depth_chroma_minus1 = 7,
		.log2_diff_max_min_pcm_luma_coding_block_size = 3,
		.num_short_term_ref_pic_sets = 5,
		.num_long_term_ref_pics_sps = 3,
		.chroma_format_idc = 1,
		.flags = V4L2_HEVC_SPS_FLAG_PCM_ENABLED |
			 V4L2_HEVC_SPS_FLAG_AMP_ENABLED |
			 V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET |
			 V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED |
			 V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT |
			 V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED |
			 V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED,
	};
	struct mt6797_hevc_sps_regs regs;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_sps_regs(&sps, &pps, &regs), 0);
	KUNIT_EXPECT_EQ(test, regs.top_068, 0x0011001eU);
	KUNIT_EXPECT_EQ(test, regs.top_070, 0x04380780U);
	KUNIT_EXPECT_EQ(test, regs.top_094, 0x0011001eU);
	KUNIT_EXPECT_EQ(test, regs.top_100, 0U);
	KUNIT_EXPECT_EQ(test, regs.vld_0a0, 0x054377fdU);
	KUNIT_EXPECT_EQ(test, regs.vld_0a4, 0x00012308U);
	KUNIT_EXPECT_EQ(test, regs.vld_0a8, 0x34635263U);
	KUNIT_EXPECT_EQ(test, regs.ctb_width, (u16)30);
	KUNIT_EXPECT_EQ(test, regs.ctb_height, (u16)17);
}

static void mt6797_hevc_pps_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps = {
		.log2_diff_max_min_luma_coding_block_size = 3,
	};
	struct v4l2_ctrl_hevc_pps pps = {
		.num_extra_slice_header_bits = 5,
		.num_ref_idx_l0_default_active_minus1 = 2,
		.num_ref_idx_l1_default_active_minus1 = 1,
		.init_qp_minus26 = -2,
		.diff_cu_qp_delta_depth = 2,
		.pps_cb_qp_offset = -3,
		.pps_cr_qp_offset = 4,
		.num_tile_columns_minus1 = 2,
		.num_tile_rows_minus1 = 1,
		.pps_beta_offset_div2 = -2,
		.pps_tc_offset_div2 = 3,
		.log2_parallel_merge_level_minus2 = 1,
		.flags = GENMASK_ULL(19, 0),
	};
	struct mt6797_hevc_pps_regs regs;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_pps_regs(&sps, &pps, &regs), 0);
	KUNIT_EXPECT_EQ(test, regs.vld_0ac, 0x53120bffU);
	KUNIT_EXPECT_EQ(test, regs.vld_0b0, 0x041d1843U);
	KUNIT_EXPECT_EQ(test, regs.vld_0b4, 0x01010201U);
	KUNIT_EXPECT_EQ(test, regs.vld_0b8, 0x000007efU);
}

static void mt6797_hevc_slice_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_pps pps = {
		.init_qp_minus26 = -2,
		.pps_cb_qp_offset = -3,
		.pps_cr_qp_offset = 4,
	};
	struct v4l2_ctrl_hevc_slice_params slice = {
		.nal_unit_type = 19,
		.nuh_temporal_id_plus1 = 1,
		.slice_type = V4L2_HEVC_SLICE_TYPE_P,
		.num_ref_idx_l0_active_minus1 = 2,
		.num_ref_idx_l1_active_minus1 = 1,
		.collocated_ref_idx = 3,
		.five_minus_max_num_merge_cand = 2,
		.slice_qp_delta = -4,
		.slice_cb_qp_offset = -4,
		.slice_cr_qp_offset = 2,
		.slice_beta_offset_div2 = -3,
		.slice_tc_offset_div2 = 2,
		.slice_segment_addr = 7,
		.flags = GENMASK_ULL(5, 0) | GENMASK_ULL(9, 7),
	};
	struct mt6797_hevc_slice_regs regs;

	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_slice_regs(&pps, &slice, 510, &regs), 0);
	KUNIT_EXPECT_EQ(test, regs.vld_0bc, 0xd0900000U);
	KUNIT_EXPECT_EQ(test, regs.vld_0c0, 0x061914e0U);
	KUNIT_EXPECT_EQ(test, regs.vld_0c4, 0x7c5d3312U);
	KUNIT_EXPECT_EQ(test, regs.vld_0f4, 0xffff0715U);
}

static void mt6797_hevc_tile_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_pps pps = {
		.num_tile_columns_minus1 = 2,
		.num_tile_rows_minus1 = 1,
		.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			 V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING,
	};
	struct mt6797_hevc_tile_regs regs;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_tile_regs(&pps, 10, 6, &regs), 0);
	KUNIT_EXPECT_EQ(test, regs.column[0], 0x224U);
	KUNIT_EXPECT_EQ(test, regs.row[0], 0x24U);

	pps.flags &= ~V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
	pps.column_width_minus1[0] = 1;
	pps.column_width_minus1[1] = 2;
	pps.row_height_minus1[0] = 1;
	KUNIT_ASSERT_EQ(test, mt6797_hevc_tile_regs(&pps, 10, 6, &regs), 0);
	KUNIT_EXPECT_EQ(test, regs.column[0], 0x212U);
	KUNIT_EXPECT_EQ(test, regs.row[0], 0x22U);
}

static void mt6797_hevc_scaling_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_scaling_matrix matrix = {};
	struct mt6797_hevc_scaling_dc_regs dc;
	unsigned int i;
	u32 word;

	for (i = 0; i < ARRAY_SIZE(matrix.scaling_list_4x4[0]); i++)
		matrix.scaling_list_4x4[0][i] = i;
	for (i = 0; i < ARRAY_SIZE(matrix.scaling_list_8x8[0]); i++)
		matrix.scaling_list_8x8[0][i] = i;
	for (i = 0; i < ARRAY_SIZE(matrix.scaling_list_dc_coef_16x16); i++)
		matrix.scaling_list_dc_coef_16x16[i] = i + 1;
	matrix.scaling_list_dc_coef_32x32[0] = 7;
	matrix.scaling_list_dc_coef_32x32[1] = 8;

	word = mt6797_hevc_pack_scaling_column(matrix.scaling_list_4x4[0], 4, 0);
	KUNIT_EXPECT_EQ(test, word, 0x0c080400U);
	word = mt6797_hevc_pack_scaling_column(matrix.scaling_list_4x4[0], 4, 3);
	KUNIT_EXPECT_EQ(test, word, 0x0f0b0703U);
	word = mt6797_hevc_pack_scaling_column(matrix.scaling_list_8x8[0], 8, 4);
	KUNIT_EXPECT_EQ(test, word, 0x1a120a02U);
	word = mt6797_hevc_pack_scaling_column(matrix.scaling_list_8x8[0], 8, 5);
	KUNIT_EXPECT_EQ(test, word, 0x3a322a22U);

	mt6797_hevc_scaling_dc_regs(&matrix, &dc);
	KUNIT_EXPECT_EQ(test, dc.pp_634, 0x04030201U);
	KUNIT_EXPECT_EQ(test, dc.pp_638, 0x08070605U);
}

static void mt6797_hevc_reject_registers_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps = {
		.pic_width_in_luma_samples = 1920,
		.pic_height_in_luma_samples = 1080,
		.chroma_format_idc = 1,
	};
	struct v4l2_ctrl_hevc_pps pps = {};
	struct v4l2_ctrl_hevc_slice_params slice = {
		.nuh_temporal_id_plus1 = 1,
		.slice_type = V4L2_HEVC_SLICE_TYPE_I,
	};
	struct mt6797_hevc_sps_regs sps_regs;
	struct mt6797_hevc_pps_regs pps_regs;
	struct mt6797_hevc_slice_regs slice_regs;
	struct mt6797_hevc_tile_regs tile_regs;

	sps.bit_depth_luma_minus8 = 2;
	KUNIT_EXPECT_EQ(test, mt6797_hevc_sps_regs(&sps, &pps, &sps_regs),
			-EOPNOTSUPP);
	sps.bit_depth_luma_minus8 = 0;
	pps.pps_cb_qp_offset = -13;
	KUNIT_EXPECT_EQ(test, mt6797_hevc_pps_regs(&sps, &pps, &pps_regs),
			-EOPNOTSUPP);
	pps.pps_cb_qp_offset = 0;
	slice.slice_qp_delta = -27;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_slice_regs(&pps, &slice, 1, &slice_regs),
			-ERANGE);
	pps.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	pps.num_tile_columns_minus1 = 1;
	pps.column_width_minus1[0] = 9;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_tile_regs(&pps, 10, 6, &tile_regs),
			-ERANGE);
}

static void mt6797_hevc_barrel_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_slice_params slice = {
		.data_byte_offset = 37,
	};
	struct mt6797_hevc_barrel_plan plan;
	int ret;

	ret = mt6797_hevc_barrel_plan(0x20000000, 1000, &slice, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.fetch_start_64, 0x00800000U);
	KUNIT_EXPECT_EQ(test, plan.fetch_end_64, 0x00800017U);
	KUNIT_EXPECT_EQ(test, plan.read_start, 0x20000025U);
	KUNIT_EXPECT_EQ(test, plan.read_end, 0x200005c0U);
}

static void mt6797_hevc_output_plan_test(struct kunit *test)
{
	struct mt6797_hevc_buffer_layout layout;
	struct mt6797_hevc_output_plan plan;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_buffer_layout(1920, 1080, &layout), 0);
	ret = mt6797_hevc_output_plan(0x10000000, 0x101fe000, &layout, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.y_addr, 0x10000000U);
	KUNIT_EXPECT_EQ(test, plan.c_addr, 0x101fe000U);
	KUNIT_EXPECT_EQ(test, plan.y_addr_512, 0x00080000U);
	KUNIT_EXPECT_EQ(test, plan.c_addr_256, 0x00101fe0U);
	KUNIT_EXPECT_EQ(test, plan.c_offset, 2088960U);
	KUNIT_EXPECT_EQ(test, plan.mb_width, 120U);
	KUNIT_EXPECT_EQ(test, plan.mb_height, 68U);
}

static void mt6797_hevc_ref_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_decode_params dec = {
		.pic_order_cnt_val = 100,
		.num_active_dpb_entries = 2,
		.dpb = {
			{ .timestamp = 11, .pic_order_cnt_val = 90 },
			{
				.timestamp = 22,
				.pic_order_cnt_val = 300,
				.flags = V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE,
			},
		},
	};
	struct v4l2_ctrl_hevc_slice_params slice = {
		.slice_type = V4L2_HEVC_SLICE_TYPE_B,
		.num_ref_idx_l0_active_minus1 = 1,
		.ref_idx_l0 = { 1, 0 },
		.ref_idx_l1 = { 1 },
	};
	struct mt6797_hevc_ref_buffer buffers[2] = {
		{
			.reference_ts = 11,
			.y_addr = 0x10000000,
			.c_addr = 0x101fe000,
			.mv_addr = 0x30000000,
			.mv_slot = 3,
			.valid = true,
		},
		{
			.reference_ts = 22,
			.y_addr = 0x11000000,
			.c_addr = 0x111fe000,
			.mv_addr = 0x30100000,
			.mv_slot = 7,
			.valid = true,
		},
	};
	struct mt6797_hevc_ref_plan plan;

	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_ref_plan(&slice, &dec, buffers, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.num_l0, (u8)2);
	KUNIT_EXPECT_EQ(test, plan.num_l1, (u8)1);
	KUNIT_EXPECT_EQ(test, plan.current_after_all, 0U);
	KUNIT_EXPECT_EQ(test, plan.long_term_flags, 0xffff5555U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].y_addr, 0x11000000U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].c_addr, 0x111fe000U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].mv_addr_16, 0x03010000U);
	KUNIT_EXPECT_EQ(test, plan.l0[0].poc_slot, 0x00000880U);
	KUNIT_EXPECT_EQ(test, plan.l0[1].poc_slot, 0x0000040aU);
	KUNIT_EXPECT_EQ(test, plan.l0[2].poc_slot, 0x00000880U);
	KUNIT_EXPECT_EQ(test, plan.l1[0].poc_slot, 0x00000880U);

	memset(&dec, 0, sizeof(dec));
	memset(&slice, 0, sizeof(slice));
	slice.slice_type = V4L2_HEVC_SLICE_TYPE_I;
	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_ref_plan(&slice, &dec, buffers, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.current_after_all, 0U);
}

static void mt6797_hevc_mv_slot_plan_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_decode_params dec = {
		.num_active_dpb_entries = 2,
		.dpb = {
			{ .timestamp = 11 },
			{ .timestamp = 22 },
		},
	};
	struct mt6797_hevc_mv_slot slots[MT6797_HEVC_MV_BUFFER_COUNT] = {
		[3] = { .reference_ts = 11, .valid = true },
		[7] = { .reference_ts = 22, .valid = true },
	};
	struct mt6797_hevc_mv_slot_plan plan;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_mv_slot_plan(&dec, slots, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.dpb_slots[0], (u8)3);
	KUNIT_EXPECT_EQ(test, plan.dpb_slots[1], (u8)7);
	KUNIT_EXPECT_EQ(test, plan.current_slot, (u8)0);
	KUNIT_ASSERT_EQ(test, mt6797_hevc_mv_slot_commit(slots, 0, 33), 0);
	KUNIT_EXPECT_TRUE(test, slots[0].valid);
	KUNIT_EXPECT_EQ(test, slots[0].reference_ts, 33ULL);
}

static void mt6797_hevc_reject_plans_test(struct kunit *test)
{
	struct mt6797_hevc_buffer_layout layout;
	struct mt6797_hevc_output_plan output;
	struct mt6797_hevc_barrel_plan barrel;
	struct v4l2_ctrl_hevc_slice_params slice = {};
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_buffer_layout(1920, 1080, &layout), 0);
	ret = mt6797_hevc_output_plan(0x10000000, 0x10200000, &layout,
				      &output);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_hevc_barrel_plan(0x20000001, 1000, &slice, &barrel);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	slice.data_byte_offset = 1000;
	ret = mt6797_hevc_barrel_plan(0x20000000, 1000, &slice, &barrel);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_hevc_1080p_layout_test(struct kunit *test)
{
	struct mt6797_hevc_buffer_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_buffer_layout(1920, 1080, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.width, 1920U);
	KUNIT_EXPECT_EQ(test, layout.height, 1088U);
	KUNIT_EXPECT_EQ(test, layout.luma_size, 2088960U);
	KUNIT_EXPECT_EQ(test, layout.chroma_size, 1044480U);
	KUNIT_EXPECT_EQ(test, layout.frame_size, 3133440U);
	KUNIT_EXPECT_EQ(test, layout.mv_size, 522752U);
}

static void mt6797_hevc_min_layout_test(struct kunit *test)
{
	struct mt6797_hevc_buffer_layout layout;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_buffer_layout(64, 64, &layout), 0);
	KUNIT_EXPECT_EQ(test, layout.width, 64U);
	KUNIT_EXPECT_EQ(test, layout.height, 64U);
	KUNIT_EXPECT_EQ(test, layout.luma_size, 4096U);
	KUNIT_EXPECT_EQ(test, layout.chroma_size, 2048U);
	KUNIT_EXPECT_EQ(test, layout.frame_size, 6144U);
	KUNIT_EXPECT_EQ(test, layout.mv_size, 1536U);
}

static void mt6797_hevc_reject_layout_test(struct kunit *test)
{
	struct mt6797_hevc_buffer_layout layout;

	KUNIT_EXPECT_EQ(test, mt6797_hevc_buffer_layout(0, 1080, &layout),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_hevc_buffer_layout(1921, 1080, &layout),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_hevc_buffer_layout(1920, 1089, &layout),
			-ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_hevc_buffer_layout(1920, 1080, NULL),
			-ERANGE);
}

static void mt6797_hevc_staging_size_test(struct kunit *test)
{
	size_t allocation_size;

	KUNIT_ASSERT_EQ(test, mt6797_hevc_staging_size(1000, &allocation_size), 0);
	KUNIT_EXPECT_EQ(test, allocation_size, (size_t)1536);
	KUNIT_EXPECT_EQ(test, mt6797_hevc_staging_size(0, &allocation_size),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_hevc_staging_size(1000, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_staging_size(SIZE_MAX -
						  MT6797_HEVC_BS_GUARD + 1,
						  &allocation_size),
			-EOVERFLOW);
}

static void mt6797_hevc_frame_slices_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_slice_params slices[2] = {
		{
			.bit_size = 800,
			.data_byte_offset = 10,
			.nal_unit_type = 1,
			.nuh_temporal_id_plus1 = 1,
			.slice_pic_order_cnt = 9,
		},
		{
			.bit_size = 640,
			.data_byte_offset = 8,
			.nal_unit_type = 1,
			.nuh_temporal_id_plus1 = 1,
			.slice_pic_order_cnt = 9,
			.slice_segment_addr = 12,
		},
	};

	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 2, 100), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 0, 100),
			-EINVAL);

	slices[1].slice_segment_addr = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 2, 100),
			-EINVAL);
	slices[1].slice_segment_addr = 12;
	slices[1].slice_pic_order_cnt++;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 2, 100),
			-EINVAL);
	slices[1].slice_pic_order_cnt--;
	slices[1].num_entry_point_offsets = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 2, 100),
			-EOPNOTSUPP);
	slices[1].num_entry_point_offsets = 0;
	slices[0].flags =
		V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_validate_frame_slices(slices, 2, 100),
			-EINVAL);
}

static void mt6797_hevc_sps_rps_sets_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_ext_sps_st_rps rps[2] = {
		{
			.num_negative_pics = 1,
			.num_positive_pics = 1,
			.delta_poc_s1_minus1 = { 1 },
		},
		{
			.abs_delta_rps_minus1 = 0,
			.use_delta_flag = GENMASK(2, 0),
			.flags = V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED,
		},
	};
	struct mt6797_hevc_rps_set sets[2];

	KUNIT_ASSERT_EQ(test, mt6797_hevc_sps_rps_sets(rps, 2, sets), 0);
	KUNIT_EXPECT_EQ(test, sets[0].num_negative_pics, (u8)1);
	KUNIT_EXPECT_EQ(test, sets[0].num_positive_pics, (u8)1);
	KUNIT_EXPECT_EQ(test, sets[0].delta_poc_s0[0], (s16)-1);
	KUNIT_EXPECT_EQ(test, sets[0].delta_poc_s1[0], (s16)2);
	KUNIT_EXPECT_EQ(test, sets[1].num_negative_pics, (u8)0);
	KUNIT_EXPECT_EQ(test, sets[1].num_positive_pics, (u8)2);
	KUNIT_EXPECT_EQ(test, sets[1].delta_poc_s1[0], (s16)1);
	KUNIT_EXPECT_EQ(test, sets[1].delta_poc_s1[1], (s16)3);

	rps[0].flags = V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED;
	KUNIT_EXPECT_EQ(test, mt6797_hevc_sps_rps_sets(rps, 1, sets), -EINVAL);
}

static void mt6797_hevc_slice_rps_regs_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_slice_params slice = {
		.nal_unit_type = 19,
		.nuh_temporal_id_plus1 = 1,
		.slice_type = V4L2_HEVC_SLICE_TYPE_I,
	};
	struct v4l2_ctrl_hevc_decode_params dec = {
		.flags = V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC,
	};
	struct v4l2_ctrl_hevc_sps sps = {};
	struct v4l2_ctrl_hevc_pps pps = {};
	struct mt6797_hevc_rps_set set = {
		.delta_poc_s0 = { -1 },
		.delta_poc_s1 = { 2 },
		.num_negative_pics = 1,
		.num_positive_pics = 1,
	};
	struct mt6797_hevc_rps_regs regs;
	struct mt6797_hevc_test_bits bits = {
		.data = { 0, 0, 1, 0x26, 1 },
		.bit = 40,
	};
	size_t size;

	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_bits(&bits, 0, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_ue(&bits, V4L2_HEVC_SLICE_TYPE_I);
	size = DIV_ROUND_UP(bits.bit, 8);
	slice.data_byte_offset = size;
	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_slice_rps_regs(bits.data, size, &sps, &pps,
						   &slice, &dec, NULL, &regs),
			0);
	KUNIT_EXPECT_EQ(test, regs.vld_0c8, 0U);

	memset(&bits, 0, sizeof(bits));
	bits.data[2] = 1;
	bits.data[3] = 0x02;
	bits.data[4] = 1;
	bits.bit = 40;
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_ue(&bits, V4L2_HEVC_SLICE_TYPE_P);
	mt6797_hevc_test_put_bits(&bits, 1, 4);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	size = DIV_ROUND_UP(bits.bit, 8);
	sps.num_short_term_ref_pic_sets = 1;
	slice.nal_unit_type = 1;
	slice.slice_type = V4L2_HEVC_SLICE_TYPE_P;
	slice.slice_pic_order_cnt = 1;
	slice.data_byte_offset = size;
	dec.pic_order_cnt_val = 1;
	dec.flags = 0;
	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_slice_rps_regs(bits.data, size, &sps, &pps,
						   &slice, &dec, &set, &regs),
			0);
	KUNIT_EXPECT_EQ(test, regs.vld_0c8, 0x00000200U);

	memset(&bits, 0, sizeof(bits));
	bits.data[2] = 1;
	bits.data[3] = 0x02;
	bits.data[4] = 1;
	bits.bit = 40;
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_ue(&bits, V4L2_HEVC_SLICE_TYPE_P);
	mt6797_hevc_test_put_bits(&bits, 1, 4);
	mt6797_hevc_test_put_ue(&bits, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	size = DIV_ROUND_UP(bits.bit, 8);
	sps.num_short_term_ref_pic_sets = 0;
	slice.data_byte_offset = size;
	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_slice_rps_regs(bits.data, size, &sps, &pps,
						   &slice, &dec, NULL, &regs),
			0);
	KUNIT_EXPECT_EQ(test, regs.vld_0c8, 0x00000100U);

	memset(&bits, 0, sizeof(bits));
	bits.data[2] = 1;
	bits.data[3] = 0x02;
	bits.data[4] = 1;
	bits.bit = 40;
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_ue(&bits, V4L2_HEVC_SLICE_TYPE_P);
	mt6797_hevc_test_put_bits(&bits, 1, 4);
	mt6797_hevc_test_put_bits(&bits, 0, 1);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_bits(&bits, 0, 1);
	mt6797_hevc_test_put_ue(&bits, 0);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	mt6797_hevc_test_put_bits(&bits, 1, 1);
	size = DIV_ROUND_UP(bits.bit, 8);
	sps.num_short_term_ref_pic_sets = 1;
	slice.data_byte_offset = size;
	dec.num_delta_pocs_of_ref_rps_idx = 2;
	KUNIT_ASSERT_EQ(test,
			mt6797_hevc_slice_rps_regs(bits.data, size, &sps, &pps,
						   &slice, &dec, &set, &regs),
			0);
	KUNIT_EXPECT_EQ(test, regs.vld_0c8, 0x00000202U);

	dec.num_delta_pocs_of_ref_rps_idx = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_hevc_slice_rps_regs(bits.data, size, &sps, &pps,
						   &slice, &dec, &set, &regs),
			-EINVAL);
}

static struct kunit_case mt6797_hevc_test_cases[] = {
	KUNIT_CASE(mt6797_hevc_sps_regs_test),
	KUNIT_CASE(mt6797_hevc_pps_regs_test),
	KUNIT_CASE(mt6797_hevc_slice_regs_test),
	KUNIT_CASE(mt6797_hevc_tile_regs_test),
	KUNIT_CASE(mt6797_hevc_scaling_regs_test),
	KUNIT_CASE(mt6797_hevc_reject_registers_test),
	KUNIT_CASE(mt6797_hevc_barrel_plan_test),
	KUNIT_CASE(mt6797_hevc_output_plan_test),
	KUNIT_CASE(mt6797_hevc_ref_plan_test),
	KUNIT_CASE(mt6797_hevc_mv_slot_plan_test),
	KUNIT_CASE(mt6797_hevc_reject_plans_test),
	KUNIT_CASE(mt6797_hevc_1080p_layout_test),
	KUNIT_CASE(mt6797_hevc_min_layout_test),
	KUNIT_CASE(mt6797_hevc_reject_layout_test),
	KUNIT_CASE(mt6797_hevc_staging_size_test),
	KUNIT_CASE(mt6797_hevc_frame_slices_test),
	KUNIT_CASE(mt6797_hevc_sps_rps_sets_test),
	KUNIT_CASE(mt6797_hevc_slice_rps_regs_test),
	{},
};

static struct kunit_suite mt6797_hevc_test_suite = {
	.name = "mt6797-hevc-registers",
	.test_cases = mt6797_hevc_test_cases,
};

kunit_test_suite(mt6797_hevc_test_suite);

MODULE_LICENSE("GPL");
