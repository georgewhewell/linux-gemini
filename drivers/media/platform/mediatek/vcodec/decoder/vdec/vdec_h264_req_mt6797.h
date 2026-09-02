/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDEC_H264_REQ_MT6797_H_
#define _VDEC_H264_REQ_MT6797_H_

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
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

#define MT6797_H264_MAX_WIDTH		1920U
#define MT6797_H264_MAX_HEIGHT		1088U

struct mt6797_h264_buffer_layout {
	u32 width;
	u32 height;
	u32 luma_size;
	u32 chroma_size;
	u32 mv_size;
	u32 crc_span;
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
	if (dec->flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC |
			  V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD |
			  V4L2_H264_DECODE_PARAM_FLAG_PFRAME |
			  V4L2_H264_DECODE_PARAM_FLAG_BFRAME))
		return -EOPNOTSUPP;

	return 0;
}

#endif
