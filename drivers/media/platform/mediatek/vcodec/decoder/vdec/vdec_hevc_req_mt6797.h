/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDEC_HEVC_REQ_MT6797_H_
#define _VDEC_HEVC_REQ_MT6797_H_

#include <linux/align.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/string.h>
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
#define MT6797_HEVC_Y_ALIGN		0x200U
#define MT6797_HEVC_C_ALIGN		0x100U
#define MT6797_HEVC_MAIN_FRAME_ALIGN	0x200U
#define MT6797_HEVC_MAIN10_FRAME_ALIGN	0x400U
#define MT6797_HEVC_REF_LIST_LEN	16U
#define MT6797_HEVC_TILE_WORDS		17U
#define MT6797_HEVC_MAX_SPS_RPS		65U
#define MT6797_HEVC_MAX_RPS_PICS	16U

struct mt6797_hevc_sps_regs {
	u32 top_068;
	u32 top_070;
	u32 top_094;
	u32 top_100;
	u32 mc_0cec;
	u32 pp_0b8c;
	u32 pp_0b90;
	u32 vld_0a0;
	u32 vld_0a4;
	u32 vld_0a8;
	u16 ctb_width;
	u16 ctb_height;
	u8 bit_depth;
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
	u32 fourcc;
	u8 num_planes;
};

struct mt6797_hevc_barrel_plan {
	u32 fetch_start_64;
	u32 fetch_end_64;
	u32 read_start;
	u32 read_end;
};

struct mt6797_hevc_output_plan {
	u32 y_addr;
	u32 c_addr;
	u32 y_addr_512;
	u32 c_addr_256;
	u32 c_offset;
	u32 mb_width;
	u32 mb_height;
};

struct mt6797_hevc_ref_buffer {
	u64 reference_ts;
	u64 y_addr;
	u64 c_addr;
	u64 mv_addr;
	u8 mv_slot;
	bool valid;
};

struct mt6797_hevc_ref {
	u32 y_addr;
	u32 c_addr;
	u32 mv_addr_16;
	u32 poc_slot;
	bool long_term;
};

struct mt6797_hevc_ref_plan {
	u8 num_l0;
	u8 num_l1;
	u32 long_term_flags;
	u32 current_after_all;
	struct mt6797_hevc_ref l0[MT6797_HEVC_REF_LIST_LEN];
	struct mt6797_hevc_ref l1[MT6797_HEVC_REF_LIST_LEN];
};

struct mt6797_hevc_mv_slot {
	u64 reference_ts;
	bool valid;
};

struct mt6797_hevc_mv_slot_plan {
	u8 dpb_slots[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	u8 current_slot;
};

struct mt6797_hevc_rps_set {
	s16 delta_poc_s0[MT6797_HEVC_MAX_RPS_PICS];
	s16 delta_poc_s1[MT6797_HEVC_MAX_RPS_PICS];
	u8 num_negative_pics;
	u8 num_positive_pics;
	u32 used_by_curr_pic;
};

struct mt6797_hevc_rps_regs {
	u32 vld_0c8;
};

#define MT6797_HEVC_HEADER_CMD_NONE	U32_MAX

struct mt6797_hevc_header_plan {
	u32 payload_offset;
	u32 rpl_start_bit;
	u32 rpl_end_bit;
	u32 weight_start_bit;
	u32 weight_end_bit;
	u32 header_end_bit;
	u8 num_pic_total_curr;
};

struct mt6797_hevc_scaling_dc_regs {
	u32 pp_634;
	u32 pp_638;
};

struct mt6797_hevc_bitreader {
	const u8 *data;
	size_t size;
	size_t pos;
	u32 raw_bits;
	u32 rbsp_bits;
	u8 byte;
	u8 bits_left;
	u8 zero_bytes;
};

static inline u32 mt6797_hevc_rpl_config(u8 num_pic_total_curr)
{
	return fls(num_pic_total_curr - 1) << 4 |
	       (num_pic_total_curr & 0xf);
}

static inline u32 mt6797_hevc_pack_scaling_column(const u8 *matrix,
						  unsigned int width,
						  unsigned int word)
{
	unsigned int column = width == 4 ? word : word / 2;
	unsigned int row = width == 4 ? 0 : (word & 1) * 4;

	return (u32)matrix[(row + 0) * width + column] |
	       (u32)matrix[(row + 1) * width + column] << 8 |
	       (u32)matrix[(row + 2) * width + column] << 16 |
	       (u32)matrix[(row + 3) * width + column] << 24;
}

static inline void
mt6797_hevc_scaling_dc_regs(const struct v4l2_ctrl_hevc_scaling_matrix *matrix,
			    struct mt6797_hevc_scaling_dc_regs *regs)
{
	regs->pp_634 = (u32)matrix->scaling_list_dc_coef_16x16[0] |
		(u32)matrix->scaling_list_dc_coef_16x16[1] << 8 |
		(u32)matrix->scaling_list_dc_coef_16x16[2] << 16 |
		(u32)matrix->scaling_list_dc_coef_16x16[3] << 24;
	regs->pp_638 = (u32)matrix->scaling_list_dc_coef_16x16[4] |
		(u32)matrix->scaling_list_dc_coef_16x16[5] << 8 |
		(u32)matrix->scaling_list_dc_coef_32x32[0] << 16 |
		(u32)matrix->scaling_list_dc_coef_32x32[1] << 24;
}

static inline int
mt6797_hevc_validate_frame_slices(const struct v4l2_ctrl_hevc_slice_params *slices,
				  unsigned int count, u32 ctb_count)
{
	const struct v4l2_ctrl_hevc_slice_params *first;
	unsigned int i;

	if (!slices || !count || !ctb_count)
		return -EINVAL;
	first = &slices[0];
	for (i = 0; i < count; i++) {
		const struct v4l2_ctrl_hevc_slice_params *slice = &slices[i];

		if (!slice->bit_size ||
		    (u64)slice->data_byte_offset * 8 >= slice->bit_size ||
		    slice->slice_segment_addr >= ctb_count)
			return -EINVAL;
		if (slice->num_entry_point_offsets)
			return -EOPNOTSUPP;
		if (slice->nal_unit_type != first->nal_unit_type ||
		    slice->nuh_temporal_id_plus1 != first->nuh_temporal_id_plus1 ||
		    slice->slice_pic_order_cnt != first->slice_pic_order_cnt)
			return -EINVAL;
		if (!i) {
			if (slice->slice_segment_addr ||
			    (slice->flags &
			     V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT))
				return -EINVAL;
		} else if (slice->slice_segment_addr <=
			   slices[i - 1].slice_segment_addr) {
			return -EINVAL;
		}
	}

	return 0;
}

static inline u32 mt6797_hevc_ceil_log2_nonzero(u32 value)
{
	return value <= 2 ? 1 : fls(value - 1);
}

static inline int mt6797_hevc_rps_add(s16 *pocs, u8 *count, s32 delta)
{
	if (*count >= MT6797_HEVC_MAX_RPS_PICS || delta < S16_MIN ||
	    delta > S16_MAX)
		return -ERANGE;
	pocs[(*count)++] = delta;

	return 0;
}

static inline int
mt6797_hevc_rps_add_s0(struct mt6797_hevc_rps_set *set, s32 delta)
{
	return mt6797_hevc_rps_add(set->delta_poc_s0,
				     &set->num_negative_pics, delta);
}

static inline int
mt6797_hevc_rps_add_s1(struct mt6797_hevc_rps_set *set, s32 delta)
{
	return mt6797_hevc_rps_add(set->delta_poc_s1,
				     &set->num_positive_pics, delta);
}

static inline int
mt6797_hevc_predict_rps(const struct mt6797_hevc_rps_set *ref,
			s32 delta_rps, u32 use_delta_flag,
			u32 used_by_curr_pic,
			struct mt6797_hevc_rps_set *set)
{
	u32 num_delta;
	unsigned int j;
	int j_signed;
	int ret;

	if (!ref || !set)
		return -EINVAL;
	num_delta = ref->num_negative_pics + ref->num_positive_pics;
	if (num_delta >= MT6797_HEVC_MAX_RPS_PICS)
		return -ERANGE;
	memset(set, 0, sizeof(*set));

	for (j_signed = ref->num_positive_pics - 1; j_signed >= 0;
	     j_signed--) {
		s32 delta = ref->delta_poc_s1[j_signed] + delta_rps;
		u32 bit = ref->num_negative_pics + j_signed;

		if (delta < 0 && (use_delta_flag & BIT(bit))) {
			ret = mt6797_hevc_rps_add_s0(set, delta);
			if (ret)
				return ret;
			if (used_by_curr_pic & BIT(bit))
				set->used_by_curr_pic |=
					BIT(set->num_negative_pics - 1);
		}
	}
	if (delta_rps < 0 && (use_delta_flag & BIT(num_delta))) {
		ret = mt6797_hevc_rps_add_s0(set, delta_rps);
		if (ret)
			return ret;
		if (used_by_curr_pic & BIT(num_delta))
			set->used_by_curr_pic |= BIT(set->num_negative_pics - 1);
	}
	for (j = 0; j < ref->num_negative_pics; j++) {
		s32 delta = ref->delta_poc_s0[j] + delta_rps;

		if (delta < 0 && (use_delta_flag & BIT(j))) {
			ret = mt6797_hevc_rps_add_s0(set, delta);
			if (ret)
				return ret;
			if (used_by_curr_pic & BIT(j))
				set->used_by_curr_pic |=
					BIT(set->num_negative_pics - 1);
		}
	}

	for (j_signed = ref->num_negative_pics - 1; j_signed >= 0;
	     j_signed--) {
		s32 delta = ref->delta_poc_s0[j_signed] + delta_rps;

		if (delta > 0 && (use_delta_flag & BIT(j_signed))) {
			ret = mt6797_hevc_rps_add_s1(set, delta);
			if (ret)
				return ret;
			if (used_by_curr_pic & BIT(j_signed))
				set->used_by_curr_pic |=
					BIT(set->num_negative_pics +
					    set->num_positive_pics - 1);
		}
	}
	if (delta_rps > 0 && (use_delta_flag & BIT(num_delta))) {
		ret = mt6797_hevc_rps_add_s1(set, delta_rps);
		if (ret)
			return ret;
		if (used_by_curr_pic & BIT(num_delta))
			set->used_by_curr_pic |=
				BIT(set->num_negative_pics +
				    set->num_positive_pics - 1);
	}
	for (j = 0; j < ref->num_positive_pics; j++) {
		s32 delta = ref->delta_poc_s1[j] + delta_rps;
		u32 bit = ref->num_negative_pics + j;

		if (delta > 0 && (use_delta_flag & BIT(bit))) {
			ret = mt6797_hevc_rps_add_s1(set, delta);
			if (ret)
				return ret;
			if (used_by_curr_pic & BIT(bit))
				set->used_by_curr_pic |=
					BIT(set->num_negative_pics +
					    set->num_positive_pics - 1);
		}
	}

	return 0;
}

static inline int
mt6797_hevc_sps_rps_sets(const struct v4l2_ctrl_hevc_ext_sps_st_rps *rps,
			 unsigned int count, struct mt6797_hevc_rps_set *sets)
{
	unsigned int i;

	if ((!rps && count) || !sets || count > MT6797_HEVC_MAX_SPS_RPS)
		return -EINVAL;
	memset(sets, 0, sizeof(*sets) * count);
	for (i = 0; i < count; i++) {
		const struct v4l2_ctrl_hevc_ext_sps_st_rps *src = &rps[i];
		struct mt6797_hevc_rps_set *set = &sets[i];
		const struct mt6797_hevc_rps_set *ref;
		s32 delta_rps;
		u32 ref_count;
		unsigned int j;
		int ret = 0;

		if (!(src->flags &
		      V4L2_HEVC_EXT_SPS_ST_RPS_FLAG_INTER_REF_PIC_SET_PRED)) {
			if (src->num_negative_pics + src->num_positive_pics >
			    MT6797_HEVC_MAX_RPS_PICS)
				return -ERANGE;
			if (!(src->num_negative_pics + src->num_positive_pics) ?
			    src->used_by_curr_pic :
			    src->used_by_curr_pic &
				~GENMASK(src->num_negative_pics +
					 src->num_positive_pics - 1, 0))
				return -EINVAL;
			set->used_by_curr_pic = src->used_by_curr_pic;
			for (j = 0; j < src->num_negative_pics; j++) {
				s32 delta = (j ? set->delta_poc_s0[j - 1] : 0) -
					    (src->delta_poc_s0_minus1[j] + 1);

				ret = mt6797_hevc_rps_add_s0(set, delta);
				if (ret)
					return ret;
			}
			for (j = 0; j < src->num_positive_pics; j++) {
				s32 delta = (j ? set->delta_poc_s1[j - 1] : 0) +
					    src->delta_poc_s1_minus1[j] + 1;

				ret = mt6797_hevc_rps_add_s1(set, delta);
				if (ret)
					return ret;
			}
			continue;
		}

		if (!i || src->delta_idx_minus1 >= i)
			return -EINVAL;
		ref = &sets[i - src->delta_idx_minus1 - 1];
		ref_count = ref->num_negative_pics + ref->num_positive_pics;
		if ((src->used_by_curr_pic | src->use_delta_flag) &
		    ~GENMASK(ref_count, 0))
			return -EINVAL;
		if (src->used_by_curr_pic & ~src->use_delta_flag)
			return -EINVAL;
		delta_rps = src->abs_delta_rps_minus1 + 1;
		if (src->delta_rps_sign)
			delta_rps = -delta_rps;
		ret = mt6797_hevc_predict_rps(ref, delta_rps,
					      src->use_delta_flag,
					      src->used_by_curr_pic, set);
		if (ret)
			return ret;
	}

	return 0;
}

static inline int mt6797_hevc_next_byte(struct mt6797_hevc_bitreader *br)
{
	u8 byte;

	while (br->pos < br->size) {
		byte = br->data[br->pos++];
		if (br->zero_bytes >= 2 && byte == 0x03) {
			br->zero_bytes = 0;
			br->raw_bits += 8;
			continue;
		}
		if (byte)
			br->zero_bytes = 0;
		else if (br->zero_bytes < 2)
			br->zero_bytes++;
		br->byte = byte;
		br->bits_left = 8;
		return 0;
	}

	return -ENODATA;
}

static inline int
mt6797_hevc_get_bits(struct mt6797_hevc_bitreader *br, u8 count, u32 *value)
{
	u32 result = 0;
	unsigned int i;
	int ret;

	if (count > 32)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!br->bits_left) {
			ret = mt6797_hevc_next_byte(br);
			if (ret)
				return ret;
		}
		result = result << 1 | ((br->byte >> (br->bits_left - 1)) & 1);
		br->bits_left--;
		br->raw_bits++;
		br->rbsp_bits++;
	}
	*value = result;

	return 0;
}

static inline int mt6797_hevc_get_ue(struct mt6797_hevc_bitreader *br,
				     u32 *value)
{
	u32 bit, suffix;
	u8 leading = 0;
	int ret;

	do {
		ret = mt6797_hevc_get_bits(br, 1, &bit);
		if (ret)
			return ret;
		if (!bit && ++leading == 32)
			return -ERANGE;
	} while (!bit);
	if (!leading) {
		*value = 0;
		return 0;
	}
	ret = mt6797_hevc_get_bits(br, leading, &suffix);
	if (ret)
		return ret;
	*value = (BIT(leading) - 1) + suffix;

	return 0;
}

static inline int mt6797_hevc_get_se(struct mt6797_hevc_bitreader *br,
				     s32 *value)
{
	u32 code;
	int ret;

	ret = mt6797_hevc_get_ue(br, &code);
	if (ret)
		return ret;
	if (code == U32_MAX)
		return -ERANGE;
	*value = code & 1 ? (code + 1) / 2 : -(s32)(code / 2);

	return 0;
}

static inline int mt6797_hevc_skip_bits(struct mt6797_hevc_bitreader *br,
					u32 count)
{
	u32 unused;
	u8 chunk;
	int ret;

	while (count) {
		chunk = min_t(u32, count, 32);
		ret = mt6797_hevc_get_bits(br, chunk, &unused);
		if (ret)
			return ret;
		count -= chunk;
	}

	return 0;
}

static inline int mt6797_hevc_expect_bit(struct mt6797_hevc_bitreader *br,
					 bool expected)
{
	u32 value;
	int ret;

	ret = mt6797_hevc_get_bits(br, 1, &value);
	if (ret)
		return ret;

	return value == expected ? 0 : -EINVAL;
}

static inline int mt6797_hevc_expect_ue(struct mt6797_hevc_bitreader *br,
				       u32 expected)
{
	u32 value;
	int ret;

	ret = mt6797_hevc_get_ue(br, &value);
	if (ret)
		return ret;

	return value == expected ? 0 : -EINVAL;
}

static inline int mt6797_hevc_expect_se(struct mt6797_hevc_bitreader *br,
				       s32 expected)
{
	s32 value;
	int ret;

	ret = mt6797_hevc_get_se(br, &value);
	if (ret)
		return ret;

	return value == expected ? 0 : -EINVAL;
}

static inline int
mt6797_hevc_parse_weight_list(struct mt6797_hevc_bitreader *br,
			      const struct v4l2_hevc_pred_weight_table *table,
			      u32 active, bool chroma, bool list1)
{
	u32 chroma_flags = 0, luma_flags = 0;
	unsigned int i, j;
	int ret;

	if (!active || active > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -ERANGE;
	for (i = 0; i < active; i++) {
		u32 flag;

		ret = mt6797_hevc_get_bits(br, 1, &flag);
		if (ret)
			return ret;
		if (flag)
			luma_flags |= BIT(i);
	}
	if (chroma) {
		for (i = 0; i < active; i++) {
			u32 flag;

			ret = mt6797_hevc_get_bits(br, 1, &flag);
			if (ret)
				return ret;
			if (flag)
				chroma_flags |= BIT(i);
		}
	}

	for (i = 0; i < active; i++) {
		if (luma_flags & BIT(i)) {
			ret = mt6797_hevc_expect_se(br,
				list1 ? table->delta_luma_weight_l1[i] :
					table->delta_luma_weight_l0[i]);
			if (ret)
				return ret;
			ret = mt6797_hevc_expect_se(br,
				list1 ? table->luma_offset_l1[i] :
					table->luma_offset_l0[i]);
			if (ret)
				return ret;
		}
		if (!(chroma_flags & BIT(i)))
			continue;
		for (j = 0; j < 2; j++) {
			ret = mt6797_hevc_expect_se(br,
				list1 ? table->delta_chroma_weight_l1[i][j] :
					table->delta_chroma_weight_l0[i][j]);
			if (ret)
				return ret;
			ret = mt6797_hevc_expect_se(br,
				list1 ? table->chroma_offset_l1[i][j] :
					table->chroma_offset_l0[i][j]);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static inline int
mt6797_hevc_parse_weight_table(struct mt6797_hevc_bitreader *br,
			       const struct v4l2_ctrl_hevc_slice_params *slice,
			       bool chroma)
{
	const struct v4l2_hevc_pred_weight_table *table =
		&slice->pred_weight_table;
	s32 chroma_denom;
	u32 denom;
	int ret;

	ret = mt6797_hevc_get_ue(br, &denom);
	if (ret)
		return ret;
	if (denom != table->luma_log2_weight_denom)
		return -EINVAL;
	if (chroma) {
		ret = mt6797_hevc_get_se(br, &chroma_denom);
		if (ret)
			return ret;
		if (chroma_denom != table->delta_chroma_log2_weight_denom)
			return -EINVAL;
	}

	ret = mt6797_hevc_parse_weight_list(br, table,
				slice->num_ref_idx_l0_active_minus1 + 1,
				chroma, false);
	if (ret || slice->slice_type != V4L2_HEVC_SLICE_TYPE_B)
		return ret;

	return mt6797_hevc_parse_weight_list(br, table,
				slice->num_ref_idx_l1_active_minus1 + 1,
				chroma, true);
}

static inline int
mt6797_hevc_parse_rpl_entries(struct mt6797_hevc_bitreader *br, u32 entries,
			      u32 bits, u32 num_pic_total_curr)
{
	u32 value;
	unsigned int i;
	int ret;

	for (i = 0; i < entries; i++) {
		ret = mt6797_hevc_get_bits(br, bits, &value);
		if (ret || value >= num_pic_total_curr)
			return ret ?: -EINVAL;
	}

	return 0;
}

static inline int
mt6797_hevc_slice_header_parse(const u8 *data, size_t size,
			       const struct v4l2_ctrl_hevc_sps *sps,
			       const struct v4l2_ctrl_hevc_pps *pps,
			       const struct v4l2_ctrl_hevc_slice_params *slice,
			       const struct v4l2_ctrl_hevc_decode_params *dec,
			       const struct mt6797_hevc_rps_set *sps_sets,
			       const struct v4l2_ctrl_hevc_ext_sps_lt_rps *lt_sets,
			       struct mt6797_hevc_rps_regs *regs,
			       struct mt6797_hevc_header_plan *plan)
{
	struct mt6797_hevc_bitreader br = {};
	struct mt6797_hevc_rps_set predicted_set;
	const struct mt6797_hevc_rps_set *set;
	u32 prefix, nal_type, temporal_id, value;
	u32 num_negative, num_positive;
	u32 num_delta_ref = 0;
	u32 num_long_sps = 0, num_long_pics = 0;
	u32 used_curr = 0;
	u32 poc_bits, poc_mask;
	unsigned int i;
	int ret;

	if (!data || !sps || !pps || !slice || !dec || !regs || size < 6)
		return -EINVAL;
	if (data[0] || data[1])
		return -EINVAL;
	if (data[2] == 1)
		prefix = 3;
	else if (size >= 7 && !data[2] && data[3] == 1)
		prefix = 4;
	else
		return -EINVAL;
	if (prefix + 2 >= size || data[prefix] & BIT(7) ||
	    (data[prefix] & 1) || (data[prefix + 1] & 0xf8))
		return -EINVAL;
	nal_type = data[prefix] >> 1;
	temporal_id = data[prefix + 1] & 7;
	if (!temporal_id || nal_type != slice->nal_unit_type ||
	    temporal_id != slice->nuh_temporal_id_plus1)
		return -EINVAL;

	br.data = data;
	br.size = min_t(size_t, size, slice->data_byte_offset);
	br.pos = prefix + 2;
	br.raw_bits = br.pos * 8;
	if (plan) {
		*plan = (struct mt6797_hevc_header_plan) {
			.payload_offset = br.pos,
			.rpl_start_bit = MT6797_HEVC_HEADER_CMD_NONE,
			.rpl_end_bit = MT6797_HEVC_HEADER_CMD_NONE,
			.weight_start_bit = MT6797_HEVC_HEADER_CMD_NONE,
			.weight_end_bit = MT6797_HEVC_HEADER_CMD_NONE,
		};
	}
	ret = mt6797_hevc_get_bits(&br, 1, &value);
	if (ret || !value)
		return ret ?: -EOPNOTSUPP;
	if (nal_type >= 16 && nal_type <= 23) {
		ret = mt6797_hevc_skip_bits(&br, 1);
		if (ret)
			return ret;
	}
	ret = mt6797_hevc_get_ue(&br, &value);
	if (ret || value != pps->pic_parameter_set_id)
		return ret ?: -EINVAL;
	ret = mt6797_hevc_skip_bits(&br, pps->num_extra_slice_header_bits);
	if (ret)
		return ret;
	ret = mt6797_hevc_get_ue(&br, &value);
	if (ret || value != slice->slice_type)
		return ret ?: -EINVAL;
	if (pps->flags & V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT) {
		ret = mt6797_hevc_skip_bits(&br, 1);
		if (ret)
			return ret;
	}
	if (nal_type == 19 || nal_type == 20) {
		if (dec->pic_order_cnt_val || slice->slice_pic_order_cnt)
			return -EINVAL;
		regs->vld_0c8 = 0;
		if (!plan)
			return 0;
		goto parsed_rps;
	}

	poc_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
	if (poc_bits > 16)
		return -ERANGE;
	ret = mt6797_hevc_get_bits(&br, poc_bits, &value);
	if (ret)
		return ret;
	poc_mask = BIT(poc_bits) - 1;
	if (value != ((u32)dec->pic_order_cnt_val & poc_mask) ||
	    dec->pic_order_cnt_val != slice->slice_pic_order_cnt)
		return -EINVAL;

	ret = mt6797_hevc_get_bits(&br, 1, &value);
	if (ret)
		return ret;
	if (value) {
		if (!sps->num_short_term_ref_pic_sets || !sps_sets)
			return -EINVAL;
		if (sps->num_short_term_ref_pic_sets == 1) {
			value = 0;
		} else {
			poc_bits = fls(sps->num_short_term_ref_pic_sets - 1);
			ret = mt6797_hevc_get_bits(&br, poc_bits, &value);
			if (ret)
				return ret;
			if (value >= sps->num_short_term_ref_pic_sets)
				return -EINVAL;
		}
		set = &sps_sets[value];
		num_negative = set->num_negative_pics;
		num_positive = set->num_positive_pics;
		used_curr = hweight32(set->used_by_curr_pic);
	} else {
		if (sps->num_short_term_ref_pic_sets) {
			u32 delta_idx_minus1, use_delta_flag = 0;
			u32 used_by_curr_pic = 0;
			u32 delta_rps_sign;
			s32 delta_rps;

			ret = mt6797_hevc_get_bits(&br, 1, &value);
			if (ret)
				return ret;
			if (value) {
				if (!sps_sets)
					return -EINVAL;
				ret = mt6797_hevc_get_ue(&br, &delta_idx_minus1);
				if (ret)
					return ret;
				if (delta_idx_minus1 >=
				    sps->num_short_term_ref_pic_sets)
					return -EINVAL;
				set = &sps_sets[sps->num_short_term_ref_pic_sets -
						 delta_idx_minus1 - 1];
				num_delta_ref = set->num_negative_pics +
						set->num_positive_pics;
				if (num_delta_ref >= MT6797_HEVC_MAX_RPS_PICS ||
				    dec->num_delta_pocs_of_ref_rps_idx !=
				    num_delta_ref)
					return -EINVAL;
				ret = mt6797_hevc_get_bits(&br, 1,
							   &delta_rps_sign);
				if (ret)
					return ret;
				ret = mt6797_hevc_get_ue(&br, &value);
				if (ret)
					return ret;
				if (value >= S16_MAX)
					return -ERANGE;
				delta_rps = value + 1;
				if (delta_rps_sign)
					delta_rps = -delta_rps;
				for (i = 0; i <= num_delta_ref; i++) {
					ret = mt6797_hevc_get_bits(&br, 1, &value);
					if (ret)
						return ret;
					if (value) {
						used_by_curr_pic |= BIT(i);
						use_delta_flag |= BIT(i);
						continue;
					}
					ret = mt6797_hevc_get_bits(&br, 1, &value);
					if (ret)
						return ret;
					if (value)
						use_delta_flag |= BIT(i);
				}
				ret = mt6797_hevc_predict_rps(set, delta_rps,
							      use_delta_flag,
							      used_by_curr_pic,
							      &predicted_set);
				if (ret)
					return ret;
				num_negative = predicted_set.num_negative_pics;
				num_positive = predicted_set.num_positive_pics;
				used_curr = hweight32(predicted_set.used_by_curr_pic);
				goto parsed_short_rps;
			}
		}
		ret = mt6797_hevc_get_ue(&br, &num_negative);
		if (ret)
			return ret;
		ret = mt6797_hevc_get_ue(&br, &num_positive);
		if (ret)
			return ret;
		if (num_negative + num_positive > MT6797_HEVC_MAX_RPS_PICS)
			return -ERANGE;
		for (i = 0; i < num_negative + num_positive; i++) {
			ret = mt6797_hevc_get_ue(&br, &value);
			if (ret)
				return ret;
			ret = mt6797_hevc_get_bits(&br, 1, &value);
			if (ret)
				return ret;
			used_curr += value;
		}
	}

parsed_short_rps:
	if (sps->flags & V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT) {
		if (sps->num_long_term_ref_pics_sps) {
			ret = mt6797_hevc_get_ue(&br, &num_long_sps);
			if (ret)
				return ret;
		}
		ret = mt6797_hevc_get_ue(&br, &num_long_pics);
		if (ret)
			return ret;
	}
	if (num_long_sps > sps->num_long_term_ref_pics_sps ||
	    num_negative + num_positive > 0x3f ||
	    num_long_sps + num_long_pics > 0x1f || num_long_sps > 0x1f)
		return -ERANGE;
	regs->vld_0c8 = num_delta_ref |
			(num_negative + num_positive) << 8 |
				(num_long_sps + num_long_pics) << 16 |
				num_long_sps << 24;
	if (!plan)
		return 0;

	/* The autonomous parser must cross the complete long-term RPS syntax. */
	for (i = 0; i < num_long_sps + num_long_pics; i++) {
		if (i < num_long_sps) {
			u32 index = 0;

			if (sps->num_long_term_ref_pics_sps > 1) {
				u32 bits = fls(sps->num_long_term_ref_pics_sps - 1);

				ret = mt6797_hevc_get_bits(&br, bits, &index);
				if (ret || index >= sps->num_long_term_ref_pics_sps)
					return ret ?: -EINVAL;
			}
			if (!lt_sets)
				return -EINVAL;
			used_curr += !!(lt_sets[index].flags &
					V4L2_HEVC_EXT_SPS_LT_RPS_FLAG_USED_LT);
		} else {
			ret = mt6797_hevc_skip_bits(&br, poc_bits);
			if (ret)
				return ret;
			ret = mt6797_hevc_get_bits(&br, 1, &value);
			if (ret)
				return ret;
			used_curr += value;
		}
		ret = mt6797_hevc_get_bits(&br, 1, &value);
		if (ret)
			return ret;
		if (value) {
			ret = mt6797_hevc_get_ue(&br, &value);
			if (ret)
				return ret;
		}
	}

parsed_rps:
	if (nal_type != 19 && nal_type != 20 &&
	    sps->flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) {
		ret = mt6797_hevc_expect_bit(&br,
			slice->flags &
			V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED);
		if (ret)
			return ret;
	}
	if (sps->flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET) {
		ret = mt6797_hevc_expect_bit(&br,
			slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA);
		if (ret)
			return ret;
		ret = mt6797_hevc_expect_bit(&br,
			slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA);
		if (ret)
			return ret;
	}

	if (slice->slice_type != V4L2_HEVC_SLICE_TYPE_I) {
		u32 bits, num_l0, num_l1 = 0;
		u32 total_curr = dec->num_poc_st_curr_before +
			dec->num_poc_st_curr_after + dec->num_poc_lt_curr;

		if (!total_curr || total_curr > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			return -EINVAL;
		if (used_curr != total_curr)
			return -EINVAL;
		ret = mt6797_hevc_get_bits(&br, 1, &value);
		if (ret)
			return ret;
		if (value) {
			ret = mt6797_hevc_get_ue(&br, &num_l0);
			if (ret)
				return ret;
			num_l0++;
			if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B) {
				ret = mt6797_hevc_get_ue(&br, &num_l1);
				if (ret)
					return ret;
				num_l1++;
			}
		} else {
			num_l0 = pps->num_ref_idx_l0_default_active_minus1 + 1;
			if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B)
				num_l1 = pps->num_ref_idx_l1_default_active_minus1 + 1;
		}
		if (num_l0 != slice->num_ref_idx_l0_active_minus1 + 1 ||
		    (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B &&
		     num_l1 != slice->num_ref_idx_l1_active_minus1 + 1))
			return -EINVAL;

		plan->num_pic_total_curr = total_curr;
		if (total_curr > 1 &&
		    pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT) {
			plan->rpl_start_bit = br.rbsp_bits;
			bits = fls(total_curr - 1);
			ret = mt6797_hevc_get_bits(&br, 1, &value);
			if (ret)
				return ret;
			if (value) {
				ret = mt6797_hevc_parse_rpl_entries(&br, num_l0,
							     bits, total_curr);
				if (ret)
					return ret;
			}
			if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B) {
				ret = mt6797_hevc_get_bits(&br, 1, &value);
				if (ret)
					return ret;
				if (value) {
					ret = mt6797_hevc_parse_rpl_entries(&br,
								     num_l1, bits,
								     total_curr);
					if (ret)
						return ret;
				}
			}
			plan->rpl_end_bit = br.rbsp_bits;
		}

		if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B) {
			ret = mt6797_hevc_expect_bit(&br,
				slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO);
			if (ret)
				return ret;
		}
		if (pps->flags & V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT) {
			ret = mt6797_hevc_expect_bit(&br,
				slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT);
			if (ret)
				return ret;
		}
		if (slice->flags &
		    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) {
			bool from_l0 = true;

			if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B) {
				from_l0 = slice->flags &
					V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0;
				ret = mt6797_hevc_expect_bit(&br, from_l0);
				if (ret)
					return ret;
			}
			if ((from_l0 && num_l0 > 1) || (!from_l0 && num_l1 > 1)) {
				ret = mt6797_hevc_expect_ue(&br, slice->collocated_ref_idx);
				if (ret)
					return ret;
			}
		}

		if ((slice->slice_type == V4L2_HEVC_SLICE_TYPE_P &&
		     pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) ||
		    (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B &&
		     pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED)) {
			plan->weight_start_bit = br.rbsp_bits;
			ret = mt6797_hevc_parse_weight_table(&br, slice,
						      sps->chroma_format_idc != 0);
			if (ret)
				return ret;
			plan->weight_end_bit = br.rbsp_bits;
		}
		ret = mt6797_hevc_expect_ue(&br,
					 slice->five_minus_max_num_merge_cand);
		if (ret)
			return ret;
	}

	ret = mt6797_hevc_expect_se(&br, slice->slice_qp_delta);
	if (ret)
		return ret;
	if (pps->flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT) {
		ret = mt6797_hevc_expect_se(&br, slice->slice_cb_qp_offset);
		if (ret)
			return ret;
		ret = mt6797_hevc_expect_se(&br, slice->slice_cr_qp_offset);
		if (ret)
			return ret;
	}
	if (pps->flags & V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		bool disabled = pps->flags &
			V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
		bool overridden = false;
		u64 disabled_flag =
			V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED;

		if (pps->flags &
		    V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED) {
			ret = mt6797_hevc_get_bits(&br, 1, &value);
			if (ret)
				return ret;
			if (value) {
				overridden = true;
				disabled = slice->flags & disabled_flag;
				ret = mt6797_hevc_expect_bit(&br, disabled);
				if (ret)
					return ret;
				if (!disabled) {
					ret = mt6797_hevc_expect_se(&br,
							     slice->slice_beta_offset_div2);
					if (ret)
						return ret;
					ret = mt6797_hevc_expect_se(&br,
							     slice->slice_tc_offset_div2);
					if (ret)
						return ret;
				}
			}
		}
		if (disabled != !!(slice->flags & disabled_flag))
			return -EINVAL;
		if (!overridden &&
		    (slice->slice_beta_offset_div2 != pps->pps_beta_offset_div2 ||
		     slice->slice_tc_offset_div2 != pps->pps_tc_offset_div2))
			return -EINVAL;
	}
	if (pps->flags & V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED &&
	    ((slice->flags & (V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA |
			      V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA)) ||
	     !(slice->flags &
	       V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED))) {
		ret = mt6797_hevc_expect_bit(&br,
			slice->flags &
			V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED);
		if (ret)
			return ret;
	}
	if (pps->flags & (V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			  V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED)) {
		u32 entry_points, offset_len = 0;

		ret = mt6797_hevc_get_ue(&br, &entry_points);
		if (ret || entry_points != slice->num_entry_point_offsets)
			return ret ?: -EINVAL;
		if (entry_points) {
			ret = mt6797_hevc_get_ue(&br, &offset_len);
			if (ret || offset_len >= 32)
				return ret ?: -ERANGE;
			ret = mt6797_hevc_skip_bits(&br,
						entry_points * (offset_len + 1));
			if (ret)
				return ret;
		}
	}
	if (pps->flags &
	    V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT) {
		u32 extension_length;

		ret = mt6797_hevc_get_ue(&br, &extension_length);
		if (ret || extension_length > (U32_MAX / 8))
			return ret ?: -ERANGE;
		ret = mt6797_hevc_skip_bits(&br, extension_length * 8);
		if (ret)
			return ret;
	}

	ret = mt6797_hevc_expect_bit(&br, true);
	if (ret)
		return ret;
	while (br.rbsp_bits & 7) {
		ret = mt6797_hevc_expect_bit(&br, false);
		if (ret)
			return ret;
	}
	if (br.pos != slice->data_byte_offset || br.bits_left)
		return -EINVAL;
	plan->header_end_bit = br.rbsp_bits;

	return 0;
}

static inline int
mt6797_hevc_slice_rps_regs(const u8 *data, size_t size,
			   const struct v4l2_ctrl_hevc_sps *sps,
			   const struct v4l2_ctrl_hevc_pps *pps,
			   const struct v4l2_ctrl_hevc_slice_params *slice,
			   const struct v4l2_ctrl_hevc_decode_params *dec,
			   const struct mt6797_hevc_rps_set *sps_sets,
			   struct mt6797_hevc_rps_regs *regs)
{
	return mt6797_hevc_slice_header_parse(data, size, sps, pps, slice, dec,
					      sps_sets, NULL, regs, NULL);
}

static inline int mt6797_hevc_sps_regs(const struct v4l2_ctrl_hevc_sps *sps,
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
	    sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8 ||
	    (sps->bit_depth_luma_minus8 &&
	     sps->bit_depth_luma_minus8 != 2) ||
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
	regs->mc_0cec = !!sps->bit_depth_luma_minus8;
	regs->pp_0b8c = !!sps->bit_depth_luma_minus8;
	regs->pp_0b90 = !!sps->bit_depth_luma_minus8;
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
	regs->bit_depth = sps->bit_depth_luma_minus8 + 8;

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
mt6797_hevc_barrel_plan(u64 dma_addr, size_t payload_size,
			const struct v4l2_ctrl_hevc_slice_params *slice,
			struct mt6797_hevc_barrel_plan *plan)
{
	u64 fetch_end, read_start;

	if (!slice || !plan || !payload_size ||
	    dma_addr & (MT6797_HEVC_BS_ALIGN - 1) ||
	    slice->data_byte_offset >= payload_size)
		return -EINVAL;
	if (check_add_overflow(dma_addr, (u64)payload_size, &fetch_end) ||
	    check_add_overflow(fetch_end, (u64)MT6797_HEVC_BS_GUARD,
			       &fetch_end) ||
	    check_add_overflow(dma_addr, (u64)slice->data_byte_offset,
			       &read_start) ||
	    fetch_end > U32_MAX)
		return -ERANGE;

	plan->fetch_start_64 = dma_addr >> 6;
	plan->fetch_end_64 = fetch_end >> 6;
	plan->read_start = read_start;
	plan->read_end = fetch_end & ~(MT6797_HEVC_BS_ALIGN - 1);

	return 0;
}

static inline int
mt6797_hevc_output_plan(u64 y_addr, u64 c_addr,
			const struct mt6797_hevc_buffer_layout *layout,
			struct mt6797_hevc_output_plan *plan)
{
	u64 y_end, c_end;

	if (!layout || !plan || !layout->luma_size || !layout->chroma_size ||
	    (layout->num_planes != 1 && layout->num_planes != 2) ||
	    y_addr & (MT6797_HEVC_Y_ALIGN - 1) ||
	    c_addr & (MT6797_HEVC_C_ALIGN - 1) ||
	    check_add_overflow(y_addr, (u64)layout->luma_size, &y_end) ||
	    check_add_overflow(c_addr, (u64)layout->chroma_size, &c_end))
		return -EINVAL;
	if (layout->num_planes == 1 && y_end != c_addr)
		return -EINVAL;
	if (y_end > U32_MAX || c_end > U32_MAX)
		return -ERANGE;

	plan->y_addr = y_addr;
	plan->c_addr = c_addr;
	plan->y_addr_512 = y_addr >> 9;
	plan->c_addr_256 = c_addr >> 8;
	plan->c_offset = layout->luma_size;
	plan->mb_width = layout->width >> 4;
	plan->mb_height = layout->height >> 4;

	return 0;
}

static inline int
mt6797_hevc_ref_entry(u8 dpb_index,
		      const struct v4l2_ctrl_hevc_decode_params *dec,
		      const struct mt6797_hevc_ref_buffer *buffers,
		      struct mt6797_hevc_ref *ref)
{
	const struct v4l2_hevc_dpb_entry *dpb;
	const struct mt6797_hevc_ref_buffer *buffer;
	s64 poc_delta;

	if (!dec || !buffers || !ref ||
	    dpb_index >= dec->num_active_dpb_entries ||
	    dpb_index >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -EINVAL;

	dpb = &dec->dpb[dpb_index];
	buffer = &buffers[dpb_index];
	if (!buffer->valid || buffer->reference_ts != dpb->timestamp ||
	    dpb->field_pic || buffer->mv_slot >= MT6797_HEVC_MV_BUFFER_COUNT ||
	    buffer->y_addr & (MT6797_HEVC_Y_ALIGN - 1) ||
	    buffer->c_addr & (MT6797_HEVC_C_ALIGN - 1) ||
	    buffer->mv_addr & (MT6797_HEVC_MV_ALIGN - 1))
		return -EINVAL;
	if (buffer->y_addr > U32_MAX || buffer->c_addr > U32_MAX ||
	    buffer->mv_addr > U32_MAX)
		return -ERANGE;

	poc_delta = (s64)dec->pic_order_cnt_val - dpb->pic_order_cnt_val;
	poc_delta = clamp_t(s64, poc_delta, -128, 127);
	ref->y_addr = buffer->y_addr;
	ref->c_addr = buffer->c_addr;
	ref->mv_addr_16 = buffer->mv_addr >> 4;
	ref->poc_slot = ((buffer->mv_slot + 1) & 0x1f) << 8 |
			(u8)poc_delta;
	ref->long_term = dpb->flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;

	return 0;
}

static inline int
mt6797_hevc_ref_plan(const struct v4l2_ctrl_hevc_slice_params *slice,
		     const struct v4l2_ctrl_hevc_decode_params *dec,
		     const struct mt6797_hevc_ref_buffer *buffers,
		     struct mt6797_hevc_ref_plan *plan)
{
	u32 num_l0 = 0, num_l1 = 0;
	unsigned int i;
	u8 ref_index;
	int ret;

	if (!slice || !dec || !buffers || !plan ||
	    dec->num_active_dpb_entries > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -EINVAL;

	if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_P) {
		num_l0 = slice->num_ref_idx_l0_active_minus1 + 1;
	} else if (slice->slice_type == V4L2_HEVC_SLICE_TYPE_B) {
		num_l0 = slice->num_ref_idx_l0_active_minus1 + 1;
		num_l1 = slice->num_ref_idx_l1_active_minus1 + 1;
	} else if (slice->slice_type != V4L2_HEVC_SLICE_TYPE_I) {
		return -EINVAL;
	}
	if (num_l0 > MT6797_HEVC_REF_LIST_LEN ||
	    num_l1 > MT6797_HEVC_REF_LIST_LEN ||
	    ((num_l0 || num_l1) && !dec->num_active_dpb_entries))
		return -EINVAL;

	*plan = (struct mt6797_hevc_ref_plan){};
	plan->num_l0 = num_l0;
	plan->num_l1 = num_l1;
	plan->current_after_all = !!dec->num_active_dpb_entries;
	for (i = 0; i < dec->num_active_dpb_entries; i++) {
		if (dec->dpb[i].field_pic)
			return -EOPNOTSUPP;
		if (dec->pic_order_cnt_val < dec->dpb[i].pic_order_cnt_val)
			plan->current_after_all = 0;
	}

	for (i = 0; i < MT6797_HEVC_REF_LIST_LEN && num_l0; i++) {
		ref_index = slice->ref_idx_l0[i % num_l0];
		ret = mt6797_hevc_ref_entry(ref_index, dec, buffers,
					    &plan->l0[i]);
		if (ret)
			return ret;
		if (plan->l0[i].long_term)
			plan->long_term_flags |= BIT(i);
	}
	for (i = 0; i < MT6797_HEVC_REF_LIST_LEN && num_l1; i++) {
		ref_index = slice->ref_idx_l1[i % num_l1];
		ret = mt6797_hevc_ref_entry(ref_index, dec, buffers,
					    &plan->l1[i]);
		if (ret)
			return ret;
		if (plan->l1[i].long_term)
			plan->long_term_flags |= BIT(i + 16);
	}

	return 0;
}

static inline int
mt6797_hevc_mv_slot_plan(const struct v4l2_ctrl_hevc_decode_params *dec,
			 const struct mt6797_hevc_mv_slot *slots,
			 struct mt6797_hevc_mv_slot_plan *plan)
{
	u32 used = 0;
	unsigned int i, j;

	if (!dec || !slots || !plan ||
	    dec->num_active_dpb_entries > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -EINVAL;

	memset(plan->dpb_slots, U8_MAX, sizeof(plan->dpb_slots));
	for (i = 0; i < dec->num_active_dpb_entries; i++) {
		for (j = 0; j < MT6797_HEVC_MV_BUFFER_COUNT; j++) {
			if (slots[j].valid &&
			    slots[j].reference_ts == dec->dpb[i].timestamp)
				break;
		}
		if (j == MT6797_HEVC_MV_BUFFER_COUNT)
			return -ENOENT;
		plan->dpb_slots[i] = j;
		used |= BIT(j);
	}

	for (i = 0; i < MT6797_HEVC_MV_BUFFER_COUNT; i++) {
		if (!(used & BIT(i))) {
			plan->current_slot = i;
			return 0;
		}
	}

	return -ENOSPC;
}

static inline int
mt6797_hevc_mv_slot_commit(struct mt6797_hevc_mv_slot *slots, u8 slot,
			   u64 reference_ts)
{
	if (!slots || slot >= MT6797_HEVC_MV_BUFFER_COUNT)
		return -EINVAL;

	slots[slot].reference_ts = reference_ts;
	slots[slot].valid = true;

	return 0;
}

static inline int
mt6797_hevc_buffer_layout(u32 width, u32 height, u8 bit_depth,
			  struct mt6797_hevc_buffer_layout *layout)
{
	u32 mb_count, packed, pixels;

	if (!layout || width < MT6797_HEVC_MIN_WIDTH ||
	    height < MT6797_HEVC_MIN_HEIGHT ||
	    width > MT6797_HEVC_MAX_WIDTH ||
	    height > MT6797_HEVC_MAX_HEIGHT)
		return -ERANGE;
	if (bit_depth != 8 && bit_depth != 10)
		return -EOPNOTSUPP;

	layout->width = ALIGN(width, MT6797_HEVC_FRAME_ALIGN);
	layout->height = ALIGN(height, MT6797_HEVC_FRAME_ALIGN);
	if (check_mul_overflow(layout->width, layout->height, &pixels))
		return -EOVERFLOW;
	if (bit_depth == 10) {
		if (check_mul_overflow(pixels, 5U, &packed))
			return -EOVERFLOW;
		layout->luma_size = packed / 4;
		layout->chroma_size = packed / 8;
		layout->fourcc = V4L2_PIX_FMT_MT2110T;
		layout->num_planes = 2;
	} else {
		layout->luma_size = pixels;
		layout->chroma_size = pixels / 2;
		layout->fourcc = V4L2_PIX_FMT_MS21;
		layout->num_planes = 1;
	}
	if (check_add_overflow(layout->luma_size, layout->chroma_size,
			       &layout->frame_size))
		return -EOVERFLOW;
	layout->frame_size = ALIGN(layout->frame_size,
				   bit_depth == 10 ?
				   MT6797_HEVC_MAIN10_FRAME_ALIGN :
				   MT6797_HEVC_MAIN_FRAME_ALIGN);

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
