/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDEC_HEVC_REQ_MT6797_H_
#define _VDEC_HEVC_REQ_MT6797_H_

#include <linux/align.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>

#define MT6797_HEVC_MIN_WIDTH		64U
#define MT6797_HEVC_MIN_HEIGHT		64U
#define MT6797_HEVC_MAX_WIDTH		1920U
#define MT6797_HEVC_MAX_HEIGHT		1088U
#define MT6797_HEVC_FRAME_ALIGN		64U
#define MT6797_HEVC_BS_ALIGN		64U
#define MT6797_HEVC_BS_GUARD		0x200U
#define MT6797_HEVC_MV_BUFFER_COUNT	17U
#define MT6797_HEVC_MV_ALIGN		0x1000U
#define MT6797_HEVC_TILE_WORDS		17U

struct mt6797_hevc_sps_regs {
	u32 top_068;
	u32 top_070;
	u32 top_094;
	u32 top_100;
	u32 vld_0a0;
	u32 vld_0a4;
	u32 vld_0a8;
	u16 ctb_width;
	u16 ctb_height;
};

struct mt6797_hevc_pps_regs {
	u32 vld_0ac;
	u32 vld_0b0;
	u32 vld_0b4;
	u32 vld_0b8;
};

struct mt6797_hevc_slice_regs {
	u32 vld_0bc;
	u32 vld_0c0;
	u32 vld_0c4;
	u32 vld_0f4;
};

struct mt6797_hevc_tile_regs {
	u32 column[MT6797_HEVC_TILE_WORDS];
	u32 row[MT6797_HEVC_TILE_WORDS];
};

struct mt6797_hevc_buffer_layout {
	u32 width;
	u32 height;
	u32 luma_size;
	u32 chroma_size;
	u32 frame_size;
	u32 mv_size;
};

static inline u32 mt6797_hevc_ceil_log2_nonzero(u32 value)
{
	return value <= 2 ? 1 : fls(value - 1);
}

static inline int
mt6797_hevc_sps_regs(const struct v4l2_ctrl_hevc_sps *sps,
		     const struct v4l2_ctrl_hevc_pps *pps,
		     struct mt6797_hevc_sps_regs *regs)
{
	u32 min_cb_log2, ctb_log2, ctb_size;
	u32 min_tb_log2, max_tb_log2;
	u32 min_pcm_log2, max_pcm_log2;
	u32 short_rps_bits, long_rps_bits;
	u32 ctb_width, ctb_height;
	u64 flags;

	if (!sps || !pps || !regs || sps->chroma_format_idc != 1 ||
	    sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8 ||
	    sps->pic_width_in_luma_samples < MT6797_HEVC_MIN_WIDTH ||
	    sps->pic_height_in_luma_samples < MT6797_HEVC_MIN_HEIGHT ||
	    sps->pic_width_in_luma_samples > MT6797_HEVC_MAX_WIDTH ||
	    sps->pic_height_in_luma_samples > MT6797_HEVC_MAX_HEIGHT ||
	    sps->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
	    sps->sps_max_sub_layers_minus1 > 6 ||
	    sps->num_short_term_ref_pic_sets > 64 ||
	    sps->num_long_term_ref_pics_sps > 32 ||
	    (sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE))
		return -EOPNOTSUPP;

	min_cb_log2 = sps->log2_min_luma_coding_block_size_minus3 + 3;
	ctb_log2 = min_cb_log2 +
		    sps->log2_diff_max_min_luma_coding_block_size;
	min_tb_log2 = sps->log2_min_luma_transform_block_size_minus2 + 2;
	max_tb_log2 = min_tb_log2 +
		    sps->log2_diff_max_min_luma_transform_block_size;
	min_pcm_log2 = sps->log2_min_pcm_luma_coding_block_size_minus3 + 3;
	max_pcm_log2 = min_pcm_log2 +
		     sps->log2_diff_max_min_pcm_luma_coding_block_size;
	if (min_cb_log2 > 6 || ctb_log2 > 6 || min_tb_log2 > 3 ||
	    max_tb_log2 > 5 || min_pcm_log2 > 7 || max_pcm_log2 > 7 ||
	    sps->max_transform_hierarchy_depth_inter > 7 ||
	    sps->max_transform_hierarchy_depth_intra > 7)
		return -EOPNOTSUPP;

	ctb_size = BIT(ctb_log2);
	ctb_width = DIV_ROUND_UP(sps->pic_width_in_luma_samples, ctb_size);
	ctb_height = DIV_ROUND_UP(sps->pic_height_in_luma_samples, ctb_size);
	short_rps_bits = mt6797_hevc_ceil_log2_nonzero(sps->num_short_term_ref_pic_sets);
	long_rps_bits = mt6797_hevc_ceil_log2_nonzero(sps->num_long_term_ref_pics_sps);
	flags = sps->flags;
	regs->top_068 = (ctb_width & 0xfff) | ((ctb_height & 0xfff) << 16);
	regs->top_070 = sps->pic_width_in_luma_samples |
			(sps->pic_height_in_luma_samples << 16);
	regs->top_094 = regs->top_068;
	regs->top_100 = sps->bit_depth_luma_minus8 |
			(sps->bit_depth_chroma_minus8 << 4);
	regs->vld_0a0 = (u32)sps->chroma_format_idc |
			!!(flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) << 2 |
			!!(pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT) << 3 |
			!!(flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED) << 4 |
			!!(flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET) << 5 |
			!!(flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED) << 6 |
			!!(flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) << 7 |
			(sps->pcm_sample_bit_depth_luma_minus1 & 0xf) << 8 |
			(sps->pcm_sample_bit_depth_chroma_minus1 & 0xf) << 12 |
			(sps->num_long_term_ref_pics_sps & 0x3f) << 16 |
			!!(flags & V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT) << 22 |
			(sps->num_short_term_ref_pic_sets & 0x7f) << 24;
	regs->vld_0a4 = (sps->log2_max_pic_order_cnt_lsb_minus4 + 4) & 0x1f;
	regs->vld_0a4 |= (short_rps_bits & 7) << 8;
	regs->vld_0a4 |= (long_rps_bits & 7) << 12;
	regs->vld_0a4 |= !!(flags &
			      V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED) << 16;
	regs->vld_0a8 = (min_cb_log2 & 7) | (ctb_log2 & 7) << 4 |
			(min_tb_log2 & 7) << 8 | (max_tb_log2 & 7) << 12 |
			(min_pcm_log2 & 7) << 16 | (max_pcm_log2 & 7) << 20 |
			(sps->max_transform_hierarchy_depth_inter & 7) << 24 |
			(sps->max_transform_hierarchy_depth_intra & 7) << 28;
	regs->ctb_width = ctb_width;
	regs->ctb_height = ctb_height;

	return 0;
}

static inline int
mt6797_hevc_pps_regs(const struct v4l2_ctrl_hevc_sps *sps,
		     const struct v4l2_ctrl_hevc_pps *pps,
		     struct mt6797_hevc_pps_regs *regs)
{
	u32 ctb_log2;
	u64 flags;

	if (!sps || !pps || !regs)
		return -EINVAL;
	ctb_log2 = sps->log2_min_luma_coding_block_size_minus3 + 3 +
		     sps->log2_diff_max_min_luma_coding_block_size;
	if (ctb_log2 > 6 ||
	    pps->diff_cu_qp_delta_depth >
		sps->log2_diff_max_min_luma_coding_block_size ||
	    pps->num_ref_idx_l0_default_active_minus1 > 15 ||
	    pps->num_ref_idx_l1_default_active_minus1 > 15 ||
	    pps->num_tile_columns_minus1 >= 20 ||
	    pps->num_tile_rows_minus1 >= 22 ||
	    pps->num_extra_slice_header_bits > 7 ||
	    pps->init_qp_minus26 < -26 || pps->init_qp_minus26 > 25 ||
	    pps->pps_cb_qp_offset < -12 || pps->pps_cb_qp_offset > 12 ||
	    pps->pps_cr_qp_offset < -12 || pps->pps_cr_qp_offset > 12 ||
	    pps->pps_beta_offset_div2 < -6 ||
	    pps->pps_beta_offset_div2 > 6 ||
	    pps->pps_tc_offset_div2 < -6 || pps->pps_tc_offset_div2 > 6 ||
	    pps->log2_parallel_merge_level_minus2 > 5)
		return -EOPNOTSUPP;

	flags = pps->flags;
	regs->vld_0ac =
		(!!(flags & V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED)) |
			!!(flags & V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT) << 1 |
			!!(flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED) << 2 |
			!!(flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED) << 3 |
			!!(flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) << 4 |
			!!(flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED) << 5 |
			!!(flags & V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT) << 6 |
			!!(flags & V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED) << 7 |
			!!(flags & V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED) << 8 |
			!!(flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) << 9 |
			!!(flags &
			   V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT) << 11 |
			(pps->num_ref_idx_l0_default_active_minus1 & 0xf) << 16 |
			(pps->num_ref_idx_l1_default_active_minus1 & 0xf) << 20 |
			((pps->log2_parallel_merge_level_minus2 + 2) & 7) << 24 |
			(pps->num_extra_slice_header_bits & 7) << 28;
	regs->vld_0b0 =
		(!!(flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED)) |
			!!(flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT) << 1 |
			((ctb_log2 - pps->diff_cu_qp_delta_depth) & 7) << 4 |
			((u32)(pps->init_qp_minus26 + 26) & 0x7f) << 8 |
			((u32)pps->pps_cb_qp_offset & 0x1f) << 16 |
			((u32)pps->pps_cr_qp_offset & 0x1f) << 24;
	regs->vld_0b4 =
		(!!(flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED)) |
			(pps->num_tile_columns_minus1 & 0x1f) << 8 |
			(pps->num_tile_rows_minus1 & 0x1f) << 16 |
			!!(flags & V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED) << 24;
	regs->vld_0b8 =
		(!!(flags &
		    V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED)) |
		!!(flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) << 1 |
		!!(flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED) << 2 |
		!!(flags & V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER) << 3 |
		((u32)pps->pps_beta_offset_div2 & 0x1f) << 4 |
		((u32)pps->pps_tc_offset_div2 & 0x1f) << 9;

	return 0;
}

static inline int
mt6797_hevc_slice_regs(const struct v4l2_ctrl_hevc_pps *pps,
		       const struct v4l2_ctrl_hevc_slice_params *slice,
		       u32 ctb_count,
		       struct mt6797_hevc_slice_regs *regs)
{
	s32 qp, cb_qp, cr_qp;
	u32 addr_bits;
	u64 flags;

	if (!pps || !slice || !regs || !ctb_count ||
	    slice->slice_type > V4L2_HEVC_SLICE_TYPE_I ||
	    slice->num_ref_idx_l0_active_minus1 > 15 ||
	    slice->num_ref_idx_l1_active_minus1 > 15 ||
	    slice->collocated_ref_idx > 15 ||
	    slice->five_minus_max_num_merge_cand > 4 ||
	    slice->slice_segment_addr >= ctb_count ||
	    slice->nal_unit_type > 63 || !slice->nuh_temporal_id_plus1 ||
	    slice->nuh_temporal_id_plus1 > 7 || slice->colour_plane_id ||
	    slice->slice_act_y_qp_offset || slice->slice_act_cb_qp_offset ||
	    slice->slice_act_cr_qp_offset ||
	    (slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_USE_INTEGER_MV))
		return -EOPNOTSUPP;

	qp = pps->init_qp_minus26 + 26 + slice->slice_qp_delta;
	cb_qp = pps->pps_cb_qp_offset + slice->slice_cb_qp_offset;
	cr_qp = pps->pps_cr_qp_offset + slice->slice_cr_qp_offset;
	if (qp < 0 || qp > 51 || cb_qp < -16 || cb_qp > 15 ||
	    cr_qp < -16 || cr_qp > 15)
		return -ERANGE;

	flags = slice->flags;
	addr_bits = fls(ctb_count - 1);
	regs->vld_0bc = (slice->slice_type & 3) << 28 |
			(addr_bits & 0x1f) << 20 |
			(slice->nal_unit_type == 19 || slice->nal_unit_type == 20) << 31 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) << 30;
	regs->vld_0c0 = !!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) << 5 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) << 6 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) << 7 |
			(qp & 0x7f) << 8 | (cb_qp & 0x1f) << 16 |
			(cr_qp & 0x1f) << 24;
	regs->vld_0c4 = (slice->num_ref_idx_l0_active_minus1 & 0xf) |
			(slice->num_ref_idx_l1_active_minus1 & 0xf) << 4 |
			(slice->collocated_ref_idx & 0xf) << 8 |
			((5 - slice->five_minus_max_num_merge_cand) & 7) << 12 |
			((u32)slice->slice_beta_offset_div2 & 0x1f) << 16 |
			((u32)slice->slice_tc_offset_div2 & 0x1f) << 21 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) << 26 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0) << 27 |
			!!(flags & V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO) << 28;
	if (flags &
	    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED)
		regs->vld_0c4 |= BIT(29);
	if (flags &
	    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED)
		regs->vld_0c4 |= BIT(30);
	regs->vld_0f4 = slice->slice_segment_addr << 8 | 0xffff0015;

	return 0;
}

static inline int
mt6797_hevc_tile_regs(const struct v4l2_ctrl_hevc_pps *pps,
		      u32 ctb_width, u32 ctb_height,
		      struct mt6797_hevc_tile_regs *regs)
{
	u32 columns, rows, end, i;
	bool uniform;

	if (!pps || !regs || !ctb_width || !ctb_height ||
	    ctb_width > MT6797_HEVC_TILE_WORDS * 32 ||
	    ctb_height > MT6797_HEVC_TILE_WORDS * 32)
		return -EINVAL;

	*regs = (struct mt6797_hevc_tile_regs){};
	if (!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED))
		return 0;

	columns = pps->num_tile_columns_minus1 + 1;
	rows = pps->num_tile_rows_minus1 + 1;
	if (columns > ARRAY_SIZE(pps->column_width_minus1) ||
	    rows > ARRAY_SIZE(pps->row_height_minus1) ||
	    columns > ctb_width || rows > ctb_height)
		return -ERANGE;

	uniform = pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
	end = 0;
	for (i = 0; i < columns; i++) {
		if (uniform)
			end = ((i + 1) * ctb_width) / columns;
		else if (i + 1 == columns)
			end = ctb_width;
		else
			end += pps->column_width_minus1[i] + 1;
		if (!end || end > ctb_width ||
		    (!uniform && i + 1 < columns && end == ctb_width))
			return -ERANGE;
		regs->column[(end - 1) >> 5] |= BIT((end - 1) & 31);
	}
	if (end != ctb_width)
		return -ERANGE;

	end = 0;
	for (i = 0; i < rows; i++) {
		if (uniform)
			end = ((i + 1) * ctb_height) / rows;
		else if (i + 1 == rows)
			end = ctb_height;
		else
			end += pps->row_height_minus1[i] + 1;
		if (!end || end > ctb_height ||
		    (!uniform && i + 1 < rows && end == ctb_height))
			return -ERANGE;
		regs->row[(end - 1) >> 5] |= BIT((end - 1) & 31);
	}

	return end == ctb_height ? 0 : -ERANGE;
}

static inline int
mt6797_hevc_buffer_layout(u32 width, u32 height,
			  struct mt6797_hevc_buffer_layout *layout)
{
	u32 mb_count, pixels;

	if (!layout || width < MT6797_HEVC_MIN_WIDTH ||
	    height < MT6797_HEVC_MIN_HEIGHT ||
	    width > MT6797_HEVC_MAX_WIDTH ||
	    height > MT6797_HEVC_MAX_HEIGHT)
		return -ERANGE;

	layout->width = ALIGN(width, MT6797_HEVC_FRAME_ALIGN);
	layout->height = ALIGN(height, MT6797_HEVC_FRAME_ALIGN);
	if (check_mul_overflow(layout->width, layout->height, &pixels))
		return -EOVERFLOW;
	layout->luma_size = pixels;
	layout->chroma_size = pixels / 2;
	if (check_add_overflow(layout->luma_size, layout->chroma_size,
			       &layout->frame_size))
		return -EOVERFLOW;

	if (check_mul_overflow(layout->width / 16, layout->height / 16,
			       &mb_count) ||
	    check_mul_overflow(mb_count, 0x40U, &layout->mv_size) ||
	    check_add_overflow(layout->mv_size, 0x200U, &layout->mv_size))
		return -EOVERFLOW;

	return 0;
}

static inline int
mt6797_hevc_staging_size(size_t payload_size, size_t *allocation_size)
{
	size_t guarded;

	if (!payload_size || !allocation_size)
		return -EINVAL;
	if (check_add_overflow(payload_size, (size_t)MT6797_HEVC_BS_GUARD,
			       &guarded))
		return -EOVERFLOW;
	if (guarded > SIZE_MAX - (MT6797_HEVC_BS_ALIGN - 1))
		return -EOVERFLOW;
	*allocation_size = ALIGN(guarded, MT6797_HEVC_BS_ALIGN);

	return 0;
}

#endif /* _VDEC_HEVC_REQ_MT6797_H_ */
