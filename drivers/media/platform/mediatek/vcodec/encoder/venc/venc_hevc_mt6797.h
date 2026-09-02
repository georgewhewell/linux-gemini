/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENC_HEVC_MT6797_H_
#define _VENC_HEVC_MT6797_H_

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/types.h>

/*
 * MT6797's normal HEVC mode fetches an opaque RCPU image from a
 * 128-byte-aligned DMA buffer.  The image has no discoverable header, so the
 * driver can only validate the exact size observed at the hardware boundary.
 */
#define MT6797_VENC_HEVC_RCPU_ALIGN 0x80U
#define MT6797_VENC_HEVC_RCPU_SIZE 0x4040U
#define MT6797_VENC_HEVC_RCPU_FIRMWARE "mediatek/mt6797-venc-hevc-rcpu.bin"

#define MT6797_VENC_HEVC_WORK_BUF_ALIGN 0x80U
#define MT6797_VENC_HEVC_RC_INFO_SIZE 0x400U
#define MT6797_VENC_HEVC_PARAMETER_SET_SIZE 0x100000U
#define MT6797_VENC_HEVC_DEFAULT_QP 28U
#define MT6797_VENC_HEVC_MAX_WIDTH 3840U
#define MT6797_VENC_HEVC_MAX_HEIGHT 2176U
#define MT6797_VENC_HEVC_HEADER_MODE 0x02003ff5U
#define MT6797_VENC_HEVC_FIRST_HEADER_MODE 0x02083ff5U

#define MT6797_VENC_HEVC_RC_START_WINDOW 15U
#define MT6797_VENC_HEVC_RC_WINDOW_SIZE 1U
#define MT6797_VENC_HEVC_RC_TRACKING_FACTOR_128 128U
#define MT6797_VENC_HEVC_RC_BITRATE_TOLERANCE 55000000U
#define MT6797_VENC_HEVC_RC_MAX_QP 51U
#define MT6797_VENC_HEVC_RC_MAX_QP_HARD_UPPER 51U
#define MT6797_VENC_HEVC_RC_MAX_QP_HARD_LOWER 35U
#define MT6797_VENC_HEVC_RC_MIN_QP 8U

enum mt6797_venc_hevc_header_type {
	MT6797_VENC_HEVC_HEADER_SPS,
	MT6797_VENC_HEVC_HEADER_VPS,
	MT6797_VENC_HEVC_HEADER_PPS,
};

struct mt6797_venc_hevc_header_regs {
	u32 mode;
	u32 start;
	u32 expected_irq;
};

enum mt6797_venc_hevc_work_buf {
	MT6797_VENC_HEVC_WORK_BUF_RC_INFO,
	MT6797_VENC_HEVC_WORK_BUF_REC_LUMA,
	MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA,
	MT6797_VENC_HEVC_WORK_BUF_REF_LUMA,
	MT6797_VENC_HEVC_WORK_BUF_REF_CHROMA,
	MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1,
	MT6797_VENC_HEVC_WORK_BUF_MV_INFO_2,
	MT6797_VENC_HEVC_WORK_BUF_SPS,
	MT6797_VENC_HEVC_WORK_BUF_VPS,
	MT6797_VENC_HEVC_WORK_BUF_PPS,
	MT6797_VENC_HEVC_WORK_BUF_MAX,
};

struct mt6797_venc_hevc_work_buf_sizes {
	size_t size[MT6797_VENC_HEVC_WORK_BUF_MAX];
};

struct mt6797_venc_hevc_io_bufs {
	dma_addr_t src_addr;
	size_t src_size;
	u32 buf_width;
	u32 buf_height;
	dma_addr_t bs_addr;
	size_t bs_size;
};

struct mt6797_venc_hevc_io_regs {
	u32 src_y;
	u32 src_c;
	u32 bs_addr;
	u32 bs_limit;
};

struct mt6797_venc_hevc_seq_params {
	u32 width;
	u32 height;
	u32 buf_width;
	u32 buf_height;
	u32 intra_period;
	u32 bitrate;
	u32 framerate;
	u32 level_idc;
};

struct mt6797_venc_hevc_seq_regs {
	u32 reg_004;
	u32 reg_008;
	u32 reg_00c;
	u32 reg_024;
	u32 reg_028;
	u32 reg_02c;
	u32 reg_030;
	u32 reg_038;
	u32 reg_048_header;
	u32 reg_048_frame;
	u32 reg_04c;
	u32 reg_050;
	u32 reg_054;
	u32 reg_128;
	u32 reg_12c;
	u32 reg_1c0;
	u32 reg_1f0;
};

struct mt6797_venc_hevc_frame_params {
	u32 frame_num;
	u32 intra_period;
	u32 last_idr_frame;
	u32 seq_reg_038;
	bool idr_toggle;
	bool sao_luma;
	bool sao_chroma;
};

struct mt6797_venc_hevc_frame_regs {
	u32 reg_010;
	u32 reg_014;
	u32 reg_018;
	u32 reg_01c;
	u32 reg_038;
	u32 mode;
	u32 next_last_idr_frame;
	bool next_idr_toggle;
	bool is_idr;
};

struct mt6797_venc_hevc_ref_regs {
	u32 ref_luma;
	u32 ref_chroma;
	u32 rec_luma;
	u32 rec_chroma;
	u32 mv_info_1;
	u32 mv_info_2;
};

struct mt6797_venc_hevc_rate_control {
	u32 start_window;
	u32 window_size;
	u32 tracking_factor_128;
	u32 bitrate_tolerance;
	u32 max_qp_hard_upper;
	u32 max_qp_hard_lower;
	u32 max_qp;
	u32 min_qp;
	u64 average_frame_bits;
	u64 window_bitrate;
};

static inline void
mt6797_hevc_rc_init(struct mt6797_venc_hevc_rate_control *rc)
{
	*rc = (struct mt6797_venc_hevc_rate_control) {
		.start_window = MT6797_VENC_HEVC_RC_START_WINDOW,
		.window_size = MT6797_VENC_HEVC_RC_WINDOW_SIZE,
		.tracking_factor_128 =
			MT6797_VENC_HEVC_RC_TRACKING_FACTOR_128,
		.bitrate_tolerance = MT6797_VENC_HEVC_RC_BITRATE_TOLERANCE,
		.max_qp_hard_upper = MT6797_VENC_HEVC_RC_MAX_QP_HARD_UPPER,
		.max_qp_hard_lower = MT6797_VENC_HEVC_RC_MAX_QP_HARD_LOWER,
		.max_qp = MT6797_VENC_HEVC_RC_MAX_QP,
		.min_qp = MT6797_VENC_HEVC_RC_MIN_QP,
	};
}

/*
 * The vendor policy compares the measured/tolerance ratio against six
 * double-precision constants.  Express the observed six-decimal literals as
 * integers and perform a full-width multiply/divide: no floating point enters
 * the kernel and the strict comparison direction is retained.
 */
static inline unsigned int
mt6797_hevc_rc_step(u64 numerator, u64 denominator,
		    u32 tracking_factor_128)
{
	static const u32 threshold_millionths[] = {
		1121658, 1258988, 1413132, 1586148, 1780347, 1998323,
	};
	u64 ratio;
	unsigned int i;

	if (!denominator)
		return ARRAY_SIZE(threshold_millionths) + 1;

	ratio = mul_u64_u64_div_u64(numerator, 128000000ULL, denominator);
	for (i = 0; i < ARRAY_SIZE(threshold_millionths); i++) {
		if (ratio < (u64)tracking_factor_128 *
			    threshold_millionths[i])
			return i + 1;
	}

	return ARRAY_SIZE(threshold_millionths) + 1;
}

static inline void
mt6797_hevc_rc_update(struct mt6797_venc_hevc_rate_control *rc,
		      u32 frame_rate, u32 frame_num, u32 frame_bytes)
{
	u64 frame_bits = (u64)frame_bytes * 8;
	u32 next_qp;
	unsigned int step;

	if (!rc || !frame_rate || !rc->window_size)
		return;

	if (!frame_num)
		rc->average_frame_bits = frame_bits;
	else
		rc->average_frame_bits =
			(frame_bits + rc->average_frame_bits * 7) >> 3;

	if (frame_num % rc->window_size || frame_num <= rc->start_window)
		return;

	rc->window_bitrate = rc->average_frame_bits * frame_rate;
	if (rc->window_bitrate > rc->bitrate_tolerance) {
		step = mt6797_hevc_rc_step(rc->window_bitrate,
					   rc->bitrate_tolerance,
					   rc->tracking_factor_128);
		next_qp = min_t(u64, (u64)rc->max_qp + step,
				rc->max_qp_hard_upper);
	} else if (rc->max_qp > rc->max_qp_hard_lower) {
		step = mt6797_hevc_rc_step(rc->bitrate_tolerance,
					   rc->window_bitrate,
					   rc->tracking_factor_128);
		next_qp = rc->max_qp > step ? rc->max_qp - step : 0;
		next_qp = max(next_qp, rc->max_qp_hard_lower);
	} else {
		return;
	}

	rc->max_qp = clamp(next_qp, rc->max_qp_hard_lower,
			   rc->max_qp_hard_upper);
}

static inline u32
mt6797_hevc_rc_reg(const struct mt6797_venc_hevc_rate_control *rc)
{
	return rc->max_qp << 8 | rc->min_qp;
}

/*
 * Header production is three distinct hardware jobs.  The first SPS after a
 * sequence reset carries the new-session bit; SetHWModeSel consumes it, so
 * the following VPS and PPS always use the ordinary header mode.
 */
static inline int
mt6797_venc_hevc_header_regs(enum mt6797_venc_hevc_header_type type,
			     bool first_sequence_header,
			     struct mt6797_venc_hevc_header_regs *regs)
{
	if (!regs)
		return -EINVAL;
	if (first_sequence_header && type != MT6797_VENC_HEVC_HEADER_SPS)
		return -EINVAL;

	switch (type) {
	case MT6797_VENC_HEVC_HEADER_SPS:
		regs->start = BIT(0);
		regs->expected_irq = BIT(0);
		break;
	case MT6797_VENC_HEVC_HEADER_VPS:
		regs->start = BIT(5);
		regs->expected_irq = BIT(7);
		break;
	case MT6797_VENC_HEVC_HEADER_PPS:
		regs->start = BIT(1);
		regs->expected_irq = BIT(1);
		break;
	default:
		return -EINVAL;
	}

	regs->mode = first_sequence_header ?
		     MT6797_VENC_HEVC_FIRST_HEADER_MODE :
		     MT6797_VENC_HEVC_HEADER_MODE;
	return 0;
}

static inline int mt6797_venc_hevc_validate_rcpu(const u8 *data, size_t size)
{
	if (!data || size != MT6797_VENC_HEVC_RCPU_SIZE)
		return -EINVAL;

	return 0;
}

static inline int mt6797_venc_hevc_dma_reg(dma_addr_t addr, size_t size,
					   unsigned int align, u32 *reg)
{
	dma_addr_t end;

	if (!size || !reg || !is_power_of_2(align) || align < 16 ||
	    !IS_ALIGNED(addr, align) ||
	    check_add_overflow(addr, size - 1, &end) || upper_32_bits(end))
		return -ERANGE;

	*reg = lower_32_bits(addr) >> 4;
	return 0;
}

/*
 * Size the ordinary (non-UFO) work buffers.  The hardware works in 32-line
 * groups.  Luma/chroma rows retain 16-pixel granularity while each MV row
 * covers 64 pixels.  These are allocation sizes, before allocator padding.
 */
static inline int
mt6797_venc_hevc_calc_buf_sizes(u32 width, u32 height,
				struct mt6797_venc_hevc_work_buf_sizes *sizes)
{
	size_t block_rows, luma_cols, mv_cols, luma_size, chroma_size, mv_size;
	u32 buf_width, buf_height;

	if (!width || !height || !sizes)
		return -EINVAL;
	if (check_add_overflow(width, 31U, &buf_width) ||
	    check_add_overflow(height, 31U, &buf_height))
		return -EOVERFLOW;

	buf_width &= ~31U;
	buf_height &= ~31U;
	block_rows = buf_height / 32;
	luma_cols = DIV_ROUND_UP(buf_width, 16);
	mv_cols = DIV_ROUND_UP(buf_width, 64);

	if (check_mul_overflow(block_rows, luma_cols, &luma_size) ||
	    check_mul_overflow(luma_size, (size_t)0x200, &luma_size) ||
	    check_mul_overflow(block_rows, luma_cols, &chroma_size) ||
	    check_mul_overflow(chroma_size, (size_t)0x100, &chroma_size) ||
	    check_mul_overflow(block_rows, mv_cols, &mv_size) ||
	    check_mul_overflow(mv_size, (size_t)0x20, &mv_size))
		return -EOVERFLOW;

	sizes->size[MT6797_VENC_HEVC_WORK_BUF_RC_INFO] =
		MT6797_VENC_HEVC_RC_INFO_SIZE;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_REC_LUMA] = luma_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA] = chroma_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_REF_LUMA] = luma_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_REF_CHROMA] = chroma_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1] = mv_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_MV_INFO_2] = mv_size;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_SPS] =
		MT6797_VENC_HEVC_PARAMETER_SET_SIZE;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_VPS] =
		MT6797_VENC_HEVC_PARAMETER_SET_SIZE;
	sizes->size[MT6797_VENC_HEVC_WORK_BUF_PPS] =
		MT6797_VENC_HEVC_PARAMETER_SET_SIZE;

	return 0;
}

/* MT6797 derives the NV12 chroma address from one contiguous allocation. */
static inline int
mt6797_venc_hevc_calc_io_regs(const struct mt6797_venc_hevc_io_bufs *bufs,
			      struct mt6797_venc_hevc_io_regs *regs)
{
	dma_addr_t chroma_addr;
	u64 luma_size, frame_size;
	size_t chroma_size;
	int ret;

	if (!bufs || !regs || !bufs->buf_width || !bufs->buf_height ||
	    !IS_ALIGNED(bufs->buf_width, 32) ||
	    !IS_ALIGNED(bufs->buf_height, 32) ||
	    !IS_ALIGNED(bufs->bs_size, 128))
		return -EINVAL;

	if (check_mul_overflow((u64)bufs->buf_width, (u64)bufs->buf_height,
			       &luma_size) ||
	    check_add_overflow(luma_size, luma_size / 2, &frame_size) ||
	    frame_size > bufs->src_size)
		return -EINVAL;

	ret = mt6797_venc_hevc_dma_reg(bufs->src_addr, frame_size, 16,
				       &regs->src_y);
	if (ret)
		return ret;

	chroma_addr = bufs->src_addr + luma_size;
	chroma_size = frame_size - luma_size;
	ret = mt6797_venc_hevc_dma_reg(chroma_addr, chroma_size, 16,
				       &regs->src_c);
	if (ret)
		return ret;

	ret = mt6797_venc_hevc_dma_reg(bufs->bs_addr, bufs->bs_size, 128,
				       &regs->bs_addr);
	if (ret)
		return ret;

	regs->bs_limit = bufs->bs_size >> 7;
	return 0;
}

/*
 * Model the vendor's normal, Main-profile, rate-controlled, non-UFO sequence
 * setup.  UFO/NBM, WFD, ViLTE, Main 10 and interlaced modes deliberately stay
 * outside this contract.
 */
static inline int
mt6797_venc_hevc_calc_seq_regs(const struct mt6797_venc_hevc_seq_params *params,
			       struct mt6797_venc_hevc_seq_regs *regs)
{
	u32 bitrate_kbps, config, ctu_count;
	u32 crop_right, crop_bottom;
	u32 width_div16, height_div16;

	if (!params || !regs || !params->width || !params->height ||
	    params->width > params->buf_width ||
	    params->height > params->buf_height ||
	    params->buf_width > MT6797_VENC_HEVC_MAX_WIDTH ||
	    params->buf_height > MT6797_VENC_HEVC_MAX_HEIGHT ||
	    !IS_ALIGNED(params->buf_width, 32) ||
	    !IS_ALIGNED(params->buf_height, 32) || !params->intra_period ||
	    params->intra_period > 0x3fff || !params->bitrate ||
	    params->bitrate > 60000000 || !params->framerate ||
	    params->framerate > 0xff)
		return -EINVAL;

	switch (params->level_idc) {
	case 10:
	case 11:
	case 12:
	case 13:
	case 20:
	case 21:
	case 22:
	case 30:
	case 31:
	case 32:
	case 40:
	case 41:
	case 50:
		break;
	default:
		return -EINVAL;
	}

	width_div16 = params->buf_width / 16;
	height_div16 = params->buf_height / 16;
	crop_right = params->buf_width - params->width;
	crop_bottom = params->buf_height - params->height;
	if (crop_right > 31 || crop_bottom > 31)
		return -EINVAL;
	if (check_mul_overflow(params->buf_width / 32, params->buf_height / 32,
			       &ctu_count))
		return -EOVERFLOW;

	if (params->width <= 1920 && params->height <= 1080 &&
	    params->framerate == 30)
		config = 0;
	else if (params->width == 1920 && params->height == 1080 &&
		 params->framerate == 60)
		config = 2;
	else
		config = 7;

	bitrate_kbps = params->bitrate / 1000;
	regs->reg_004 = MT6797_VENC_HEVC_DEFAULT_QP * 0x4000000U +
			params->intra_period * 0x4000 + params->intra_period;
	regs->reg_008 = MT6797_VENC_HEVC_DEFAULT_QP * 0x400 + 0x3a0;
	regs->reg_00c = 0;
	regs->reg_024 = width_div16 | height_div16 << 16;
	regs->reg_028 = width_div16 << 20 | width_div16 << 11 |
			crop_right << 5 | crop_bottom;
	regs->reg_02c = ctu_count;
	regs->reg_030 = 0x20000000 | params->level_idc * 0x400000;
	regs->reg_038 = config * 0x400 + 0x2123;
	regs->reg_048_header = bitrate_kbps + 0x60000;
	regs->reg_048_frame = bitrate_kbps + 0x40000;
	regs->reg_04c = params->framerate << 16 | 0x306;
	regs->reg_050 = 0x3308;
	regs->reg_054 = 0;
	regs->reg_128 = 0;
	regs->reg_12c = 0x600;
	regs->reg_1c0 = 0x10;
	regs->reg_1f0 = 2;

	return 0;
}

static inline int
mt6797_hevc_frame_regs(const struct mt6797_venc_hevc_frame_params *params,
		       struct mt6797_venc_hevc_frame_regs *regs)
{
	/* This is the vendor's NonRefPFrmPeriod == 1 path. */
	u32 non_ref = 0;
	u32 slice_type = 0;

	if (!params || !regs || !params->intra_period ||
	    params->intra_period > 0x3fff)
		return -EINVAL;

	regs->is_idr = !(params->frame_num % params->intra_period);
	regs->next_idr_toggle = params->idr_toggle;
	regs->next_last_idr_frame = params->last_idr_frame;
	if (regs->is_idr) {
		regs->next_idr_toggle = !params->idr_toggle;
		regs->next_last_idr_frame = params->frame_num;
		slice_type = 2;
	} else {
		non_ref = (params->frame_num - 1) % params->intra_period != 0;
	}

	regs->reg_010 = 0x8000 | non_ref << 8 | params->idr_toggle << 3 |
			regs->is_idr << 2 | slice_type;
	if (regs->is_idr)
		regs->reg_010 ^= BIT(3);
	regs->reg_014 = params->frame_num;
	regs->reg_018 = params->frame_num;
	regs->reg_01c = regs->next_last_idr_frame;
	regs->reg_038 = params->seq_reg_038 | 0x80 | params->sao_luma << 4 |
			params->sao_chroma << 3;
	regs->mode = params->frame_num ? 0x0fa03ff5 : 0x0f603ff5;

	return 0;
}

static inline int
mt6797_hevc_ref_regs(const u32 work_buf_regs[MT6797_VENC_HEVC_WORK_BUF_MAX],
		     u32 frame_num, struct mt6797_venc_hevc_ref_regs *regs)
{
	bool odd;

	if (!work_buf_regs || !regs)
		return -EINVAL;

	odd = frame_num & 1;
	regs->ref_luma =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_REC_LUMA :
				    MT6797_VENC_HEVC_WORK_BUF_REF_LUMA];
	regs->ref_chroma =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA :
				    MT6797_VENC_HEVC_WORK_BUF_REF_CHROMA];
	regs->rec_luma =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_REF_LUMA :
				    MT6797_VENC_HEVC_WORK_BUF_REC_LUMA];
	regs->rec_chroma =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_REF_CHROMA :
				    MT6797_VENC_HEVC_WORK_BUF_REC_CHROMA];
	regs->mv_info_1 =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_MV_INFO_2 :
				    MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1];
	regs->mv_info_2 =
		work_buf_regs[odd ? MT6797_VENC_HEVC_WORK_BUF_MV_INFO_1 :
				    MT6797_VENC_HEVC_WORK_BUF_MV_INFO_2];

	return 0;
}

#endif
