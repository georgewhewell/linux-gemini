/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENC_HEVC_MT6797_H_
#define _VENC_HEVC_MT6797_H_

#include <linux/dma-mapping.h>
#include <linux/errno.h>
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

#endif
