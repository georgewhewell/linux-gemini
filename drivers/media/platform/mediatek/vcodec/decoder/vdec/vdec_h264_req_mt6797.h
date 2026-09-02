/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDEC_H264_REQ_MT6797_H_
#define _VDEC_H264_REQ_MT6797_H_

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <media/v4l2-ctrls.h>

#define MT6797_H264_SPS_CHROMA		GENMASK(1, 0)
#define MT6797_H264_SPS_LOG2_FRAME_NUM	GENMASK(5, 2)
#define MT6797_H264_SPS_POC_TYPE	GENMASK(7, 6)
#define MT6797_H264_SPS_LOG2_POC	GENMASK(11, 8)
#define MT6797_H264_SPS_DELTA_POC_ZERO	BIT(12)
#define MT6797_H264_SPS_MAX_REFS	GENMASK(17, 13)
#define MT6797_H264_SPS_FRAME_ONLY	BIT(18)
#define MT6797_H264_SPS_MBAFF		BIT(19)
#define MT6797_H264_SPS_DIRECT_8X8	BIT(20)
#define MT6797_H264_SPS_ENABLE		BIT(21)

#define MT6797_H264_PPS_ENTROPY		BIT(0)
#define MT6797_H264_PPS_BOTTOM_POC	BIT(1)
#define MT6797_H264_PPS_WEIGHTED_PRED	BIT(2)
#define MT6797_H264_PPS_WEIGHTED_BIPRED	GENMASK(4, 3)
#define MT6797_H264_PPS_INIT_QP		GENMASK(10, 5)
#define MT6797_H264_PPS_CHROMA_QP	GENMASK(15, 11)
#define MT6797_H264_PPS_DEBLOCK		BIT(16)
#define MT6797_H264_PPS_CONSTRAINED_INTRA BIT(17)
#define MT6797_H264_PPS_TRANSFORM_8X8	BIT(18)
#define MT6797_H264_PPS_SECOND_CHROMA_QP GENMASK(23, 19)

#define MT6797_H264_SH_FIRST_MB		GENMASK(12, 0)
#define MT6797_H264_SH_TYPE		GENMASK(16, 13)
#define MT6797_H264_SH_FIELD		BIT(17)
#define MT6797_H264_SH_BOTTOM_FIELD	BIT(18)
#define MT6797_H264_SH_DIRECT_SPATIAL	BIT(19)
#define MT6797_H264_SH_REFS_L0		GENMASK(24, 20)
#define MT6797_H264_SH_REFS_L1		GENMASK(29, 25)

#define MT6797_H264_SH2_QP_DELTA	GENMASK(6, 0)
#define MT6797_H264_SH2_DEBLOCK		GENMASK(8, 7)
#define MT6797_H264_SH2_ALPHA		GENMASK(12, 9)
#define MT6797_H264_SH2_BETA		GENMASK(16, 13)
#define MT6797_H264_SH2_NAL_REF_IDC	GENMASK(18, 17)
#define MT6797_H264_SH2_IDR		BIT(19)
#define MT6797_H264_SH2_CABAC_INIT	GENMASK(21, 20)

#define MT6797_H264_POC_VALUE		GENMASK(17, 0)

#define MT6797_H264_REF_PIC_NUM		GENMASK(18, 0)
#define MT6797_H264_REF_PIC_LONG_TERM	BIT(19)
#define MT6797_H264_REF_MV_LONG_TERM	BIT(20)
#define MT6797_H264_REF_BOTTOM		BIT(22)
#define MT6797_H264_REF_DPB_SLOT	GENMASK(27, 23)
#define MT6797_H264_REF_POC_VALID	BIT(21)

#define MT6797_H264_MAX_WIDTH		1920U
#define MT6797_H264_MAX_HEIGHT		1088U
#define MT6797_H264_DMA_ALIGN		512U
#define MT6797_H264_BS_ALIGN		64U
#define MT6797_H264_BS_GUARD		0x100U
#define MT6797_H264_NAL_HEADER_BITS	8U
#define MT6797_H264_BARREL_MAX_SHIFT	32U
#define MT6797_H264_MV_BUFFER_COUNT	17U
#define MT6797_H264_PREDICTION_SIZE	0x8000U
#define MT6797_H264_PP_SIZE		0x1e000U
#define MT6797_H264_LD_SIZE		0xf000U
#define MT6797_H264_PREDICTION_ALIGN	0x400U
#define MT6797_H264_AUX_ALIGN		0x800U
#define MT6797_H264_MV_ALIGN		0x1000U

struct mt6797_h264_buffer_layout {
	u32 width;
	u32 height;
	u32 luma_size;
	u32 chroma_size;
	u32 mv_size;
	u32 crc_span;
};

struct mt6797_h264_barrel_plan {
	u32 start_addr;
	u32 end_addr;
	u32 fetch_start;
	u32 fetch_end;
	u32 shift_reads;
	u32 final_shift;
};

struct mt6797_h264_output_plan {
	u32 y_addr_512;
	u32 c_addr_256;
	u32 end_addr_16;
	u32 y_addr_16;
	u32 y_addr;
	u32 c_offset;
	u32 mv_addr_16;
	u32 crc_y_addr;
	u32 crc_end_addr;
};

struct mt6797_h264_working_plan {
	u32 prediction_addr;
	u32 pp_addr;
	u32 ld_addr;
	u32 mv_addr_16;
};

struct mt6797_h264_idr_plan {
	u32 max_frame_num;
	u32 frame_num;
	u32 poc;
	u32 top_poc;
	u32 bottom_poc;
	u32 ref_y_addr;
	u32 ref_mv_addr_16;
};

struct mt6797_h264_ref_buffer {
	u64 y_addr;
	u8 dpb_slot;
};

struct mt6797_h264_p_ref {
	u32 y_addr;
	u32 pic_num;
	u32 top;
	u32 bottom;
};

struct mt6797_h264_p_ref_plan {
	u8 num_refs;
	struct mt6797_h264_p_ref refs[V4L2_H264_REF_LIST_LEN];
};

static inline int
mt6797_h264_buffer_layout(u32 width, u32 height,
			  struct mt6797_h264_buffer_layout *layout)
{
	u32 tile_count;

	if (!width || !height || width > MT6797_H264_MAX_WIDTH ||
	    height > MT6797_H264_MAX_HEIGHT)
		return -ERANGE;

	layout->width = ALIGN(width, 16);
	layout->height = ALIGN(height, 32);
	tile_count = layout->width / 16 * (layout->height / 32);
	layout->luma_size = tile_count * 512;
	layout->chroma_size = layout->luma_size / 2;
	layout->mv_size = layout->width / 16 * (layout->height / 16) * 64 + 512;
	layout->crc_span = ALIGN(tile_count * 384, 512);

	return 0;
}

static inline int
mt6797_h264_barrel_plan(u64 dma_addr, u64 size, u32 header_bits,
			struct mt6797_h264_barrel_plan *plan)
{
	u64 end_addr, fetch_end;
	u64 bit_size;
	u32 shift_bits;

	if (!size || header_bits <= MT6797_H264_NAL_HEADER_BITS ||
	    (dma_addr & (MT6797_H264_BS_ALIGN - 1)))
		return -EINVAL;
	if (check_mul_overflow(size, 8ULL, &bit_size) || header_bits >= bit_size)
		return -ERANGE;
	if (check_add_overflow(dma_addr, size, &end_addr) ||
	    check_add_overflow(end_addr, (u64)MT6797_H264_BS_GUARD, &fetch_end) ||
	    fetch_end > U32_MAX)
		return -ERANGE;

	plan->start_addr = dma_addr;
	plan->end_addr = end_addr;
	plan->fetch_start = dma_addr >> 6;
	plan->fetch_end = fetch_end >> 6;
	shift_bits = header_bits - MT6797_H264_NAL_HEADER_BITS;
	plan->shift_reads = DIV_ROUND_UP(shift_bits,
					 MT6797_H264_BARREL_MAX_SHIFT);
	plan->final_shift = shift_bits % MT6797_H264_BARREL_MAX_SHIFT;
	if (!plan->final_shift)
		plan->final_shift = MT6797_H264_BARREL_MAX_SHIFT;

	return 0;
}

static inline u32 mt6797_h264_barrel_shift_chunk(u32 remaining_bits)
{
	if (remaining_bits > MT6797_H264_BARREL_MAX_SHIFT)
		return MT6797_H264_BARREL_MAX_SHIFT;

	return remaining_bits;
}

static inline int
mt6797_h264_output_plan(u64 y_addr, u64 c_addr, u64 mv_addr,
			const struct mt6797_h264_buffer_layout *layout,
			struct mt6797_h264_output_plan *plan)
{
	u64 expected_c, end_addr, crc_end_addr;

	if ((y_addr & (MT6797_H264_DMA_ALIGN - 1)) ||
	    (c_addr & (MT6797_H264_DMA_ALIGN - 1)) || (mv_addr & 0xf))
		return -EINVAL;
	if (check_add_overflow(y_addr, (u64)layout->luma_size, &expected_c) ||
	    check_add_overflow(c_addr, (u64)layout->chroma_size, &end_addr) ||
	    check_add_overflow(y_addr, (u64)layout->crc_span, &crc_end_addr) ||
	    end_addr > U32_MAX || crc_end_addr > U32_MAX || mv_addr > U32_MAX)
		return -ERANGE;

	/* The recovered MT6797 path only establishes this contiguous layout. */
	if (c_addr != expected_c)
		return -EOPNOTSUPP;

	plan->y_addr_512 = y_addr >> 9;
	plan->c_addr_256 = c_addr >> 8;
	plan->end_addr_16 = end_addr >> 4;
	plan->y_addr_16 = y_addr >> 4;
	plan->y_addr = y_addr;
	plan->c_offset = layout->luma_size;
	plan->mv_addr_16 = mv_addr >> 4;
	plan->crc_y_addr = y_addr;
	plan->crc_end_addr = crc_end_addr;

	return 0;
}

static inline int
mt6797_h264_working_plan(u64 prediction_addr, u64 pp_addr, u64 ld_addr,
			 u64 mv_addr, struct mt6797_h264_working_plan *plan)
{
	if ((prediction_addr & (MT6797_H264_PREDICTION_ALIGN - 1)) ||
	    (pp_addr & (MT6797_H264_AUX_ALIGN - 1)) ||
	    (ld_addr & (MT6797_H264_AUX_ALIGN - 1)) ||
	    (mv_addr & (MT6797_H264_MV_ALIGN - 1)))
		return -EINVAL;
	if (prediction_addr > U32_MAX || pp_addr > U32_MAX ||
	    ld_addr > U32_MAX || mv_addr > U32_MAX)
		return -ERANGE;

	plan->prediction_addr = prediction_addr;
	plan->pp_addr = pp_addr;
	plan->ld_addr = ld_addr;
	plan->mv_addr_16 = mv_addr >> 4;

	return 0;
}

static inline u32 mt6797_h264_sps_reg(const struct v4l2_ctrl_h264_sps *sps)
{
	u32 flags = sps->flags;
	u32 reg = MT6797_H264_SPS_ENABLE;

	reg |= FIELD_PREP(MT6797_H264_SPS_CHROMA, sps->chroma_format_idc);
	reg |= FIELD_PREP(MT6797_H264_SPS_LOG2_FRAME_NUM,
			  sps->log2_max_frame_num_minus4);
	reg |= FIELD_PREP(MT6797_H264_SPS_POC_TYPE, sps->pic_order_cnt_type);
	reg |= FIELD_PREP(MT6797_H264_SPS_LOG2_POC,
			  sps->log2_max_pic_order_cnt_lsb_minus4);
	reg |= FIELD_PREP(MT6797_H264_SPS_MAX_REFS, sps->max_num_ref_frames);
	if (flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)
		reg |= MT6797_H264_SPS_DELTA_POC_ZERO;
	if (flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY)
		reg |= MT6797_H264_SPS_FRAME_ONLY;
	if (flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)
		reg |= MT6797_H264_SPS_MBAFF;
	if (flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE)
		reg |= MT6797_H264_SPS_DIRECT_8X8;

	return reg;
}

static inline u32 mt6797_h264_pps_reg(const struct v4l2_ctrl_h264_pps *pps)
{
	u32 flags = pps->flags;
	u32 reg = 0;

	if (flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)
		reg |= MT6797_H264_PPS_ENTROPY;
	if (flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
		reg |= MT6797_H264_PPS_BOTTOM_POC;
	if (flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED)
		reg |= MT6797_H264_PPS_WEIGHTED_PRED;
	reg |= FIELD_PREP(MT6797_H264_PPS_WEIGHTED_BIPRED,
			  pps->weighted_bipred_idc);
	reg |= FIELD_PREP(MT6797_H264_PPS_INIT_QP,
			  (u32)pps->pic_init_qp_minus26 & 0x3f);
	reg |= FIELD_PREP(MT6797_H264_PPS_CHROMA_QP,
			  (u32)pps->chroma_qp_index_offset & 0x1f);
	if (flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT)
		reg |= MT6797_H264_PPS_DEBLOCK;
	if (flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		reg |= MT6797_H264_PPS_CONSTRAINED_INTRA;
	if (flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)
		reg |= MT6797_H264_PPS_TRANSFORM_8X8;
	reg |= FIELD_PREP(MT6797_H264_PPS_SECOND_CHROMA_QP,
			  (u32)pps->second_chroma_qp_index_offset & 0x1f);

	return reg;
}

static inline u32 mt6797_h264_slice_reg(const struct v4l2_ctrl_h264_slice_params *slice,
					bool field, bool bottom_field)
{
	u32 reg;

	reg = FIELD_PREP(MT6797_H264_SH_TYPE, slice->slice_type);
	reg |= FIELD_PREP(MT6797_H264_SH_REFS_L0,
			  slice->num_ref_idx_l0_active_minus1);
	reg |= FIELD_PREP(MT6797_H264_SH_REFS_L1,
			  slice->num_ref_idx_l1_active_minus1);
	if (field)
		reg |= MT6797_H264_SH_FIELD;
	if (bottom_field)
		reg |= MT6797_H264_SH_BOTTOM_FIELD;
	if (slice->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)
		reg |= MT6797_H264_SH_DIRECT_SPATIAL;

	return reg;
}

static inline u32
mt6797_h264_slice2_reg(const struct v4l2_ctrl_h264_slice_params *slice,
		       const struct v4l2_ctrl_h264_decode_params *dec)
{
	u32 reg;

	reg = FIELD_PREP(MT6797_H264_SH2_QP_DELTA,
			 (u32)slice->slice_qp_delta & 0x7f);
	reg |= FIELD_PREP(MT6797_H264_SH2_DEBLOCK,
			  slice->disable_deblocking_filter_idc);
	reg |= FIELD_PREP(MT6797_H264_SH2_ALPHA,
			  (u32)slice->slice_alpha_c0_offset_div2 & 0xf);
	reg |= FIELD_PREP(MT6797_H264_SH2_BETA,
			  (u32)slice->slice_beta_offset_div2 & 0xf);
	reg |= FIELD_PREP(MT6797_H264_SH2_NAL_REF_IDC, dec->nal_ref_idc);
	reg |= FIELD_PREP(MT6797_H264_SH2_CABAC_INIT, slice->cabac_init_idc);
	if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)
		reg |= MT6797_H264_SH2_IDR;

	return reg;
}

static inline int
mt6797_h264_idr_plan(const struct v4l2_ctrl_h264_sps *sps,
		     const struct v4l2_ctrl_h264_decode_params *dec,
		     u64 y_addr, u64 mv_addr,
		     struct mt6797_h264_idr_plan *plan)
{
	s32 poc;

	if (!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))
		return -EINVAL;
	if (sps->log2_max_frame_num_minus4 > 12 ||
	    (y_addr & (MT6797_H264_DMA_ALIGN - 1)) ||
	    (mv_addr & (MT6797_H264_MV_ALIGN - 1)))
		return -EINVAL;
	if (y_addr > U32_MAX || mv_addr > U32_MAX)
		return -ERANGE;

	poc = min(dec->top_field_order_cnt, dec->bottom_field_order_cnt);
	plan->max_frame_num = 1U << (sps->log2_max_frame_num_minus4 + 4);
	plan->frame_num = dec->frame_num;
	plan->poc = (u32)poc & MT6797_H264_POC_VALUE;
	plan->top_poc = (u32)dec->top_field_order_cnt & MT6797_H264_POC_VALUE;
	plan->bottom_poc = (u32)dec->bottom_field_order_cnt & MT6797_H264_POC_VALUE;
	plan->ref_y_addr = y_addr;
	plan->ref_mv_addr_16 = mv_addr >> 4;

	return 0;
}

static inline int
mt6797_h264_p_ref_plan(const struct v4l2_ctrl_h264_slice_params *slice,
		       const struct v4l2_ctrl_h264_decode_params *dec,
		       const struct mt6797_h264_ref_buffer buffers[V4L2_H264_NUM_DPB_ENTRIES],
		       struct mt6797_h264_p_ref_plan *plan)
{
	u32 num_refs = slice->num_ref_idx_l0_active_minus1 + 1;
	unsigned int i;

	if (slice->slice_type != V4L2_H264_SLICE_TYPE_P ||
	    !(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_PFRAME) ||
	    dec->flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC |
			  V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD |
			  V4L2_H264_DECODE_PARAM_FLAG_BFRAME) ||
	    num_refs > V4L2_H264_REF_LIST_LEN)
		return -EINVAL;

	plan->num_refs = num_refs;
	for (i = 0; i < num_refs; i++) {
		const struct v4l2_h264_reference *ref = &slice->ref_pic_list0[i];
		const struct v4l2_h264_dpb_entry *dpb;
		const struct mt6797_h264_ref_buffer *buffer;
		struct mt6797_h264_p_ref *entry = &plan->refs[i];
		u32 common;
		bool long_term;

		if (ref->index >= V4L2_H264_NUM_DPB_ENTRIES ||
		    ref->fields != V4L2_H264_FRAME_REF)
			return -EINVAL;

		dpb = &dec->dpb[ref->index];
		buffer = &buffers[ref->index];
		if ((dpb->flags & (V4L2_H264_DPB_ENTRY_FLAG_VALID |
				   V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)) !=
		    (V4L2_H264_DPB_ENTRY_FLAG_VALID |
		     V4L2_H264_DPB_ENTRY_FLAG_ACTIVE) ||
		    dpb->fields != V4L2_H264_FRAME_REF ||
		    buffer->dpb_slot >= MT6797_H264_MV_BUFFER_COUNT ||
		    (buffer->y_addr & (MT6797_H264_DMA_ALIGN - 1)))
			return -EINVAL;
		if (buffer->y_addr > U32_MAX)
			return -ERANGE;

		long_term = dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
		entry->y_addr = buffer->y_addr;
		entry->pic_num = (long_term ? dpb->frame_num : dpb->pic_num) &
				 MT6797_H264_REF_PIC_NUM;
		common = FIELD_PREP(MT6797_H264_REF_DPB_SLOT,
				    buffer->dpb_slot);
		if (long_term) {
			entry->pic_num |= MT6797_H264_REF_PIC_LONG_TERM;
			common |= MT6797_H264_REF_MV_LONG_TERM;
		}
		if (dpb->top_field_order_cnt <= dpb->bottom_field_order_cnt &&
		    dpb->top_field_order_cnt != INT_MAX)
			common |= MT6797_H264_REF_POC_VALID;
		entry->top = common;
		entry->bottom = common | MT6797_H264_REF_BOTTOM;
	}

	return 0;
}

static inline int
mt6797_h264_validate_request(const struct v4l2_ctrl_h264_sps *sps,
			     const struct v4l2_ctrl_h264_pps *pps,
			     const struct v4l2_ctrl_h264_slice_params *slice,
			     const struct v4l2_ctrl_h264_decode_params *dec)
{
	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8)
		return -EINVAL;
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    sps->flags & (V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE |
			  V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD))
		return -EINVAL;
	if (sps->pic_width_in_mbs_minus1 > 119 ||
	    sps->pic_height_in_map_units_minus1 > 67)
		return -ERANGE;
	if (pps->num_slice_groups_minus1 ||
	    pps->flags & (V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
			  V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT |
			  V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) ||
	    pps->weighted_bipred_idc)
		return -EINVAL;
	if (slice->first_mb_in_slice || slice->slice_type != V4L2_H264_SLICE_TYPE_I)
		return -EOPNOTSUPP;
	if (!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))
		return -EOPNOTSUPP;
	if (dec->flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC |
			  V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD |
			  V4L2_H264_DECODE_PARAM_FLAG_PFRAME |
			  V4L2_H264_DECODE_PARAM_FLAG_BFRAME))
		return -EOPNOTSUPP;

	return 0;
}

#endif
