/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENC_H264_MT6797_H_
#define _VENC_H264_MT6797_H_

#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/types.h>

/*
 * The hardware fetches the opaque image from a 128-byte-aligned DMA buffer.
 * Its producer defines no discoverable magic or version field, so reject
 * every size except the exact normal-mode RCPU image size used by MT6797.
 */
#define MT6797_VENC_H264_RCPU_ALIGN		0x80U
#define MT6797_VENC_H264_RCPU_SIZE		0x4040U
#define MT6797_VENC_H264_RCPU_FIRMWARE \
	"mediatek/mt6797-venc-h264-rcpu.bin"

#define MT6797_VENC_H264_WORK_BUF_ALIGN		0x80U
#define MT6797_VENC_H264_RC_INFO_SIZE		0x400U
#define MT6797_VENC_H264_SKIP_FRAME_SIZE	0x80U

enum mt6797_venc_h264_work_buf {
	MT6797_VENC_H264_WORK_BUF_RC_INFO,
	MT6797_VENC_H264_WORK_BUF_REC_LUMA,
	MT6797_VENC_H264_WORK_BUF_REC_CHROMA,
	MT6797_VENC_H264_WORK_BUF_REF_LUMA,
	MT6797_VENC_H264_WORK_BUF_REF_CHROMA,
	MT6797_VENC_H264_WORK_BUF_MV_INFO_1,
	MT6797_VENC_H264_WORK_BUF_MV_INFO_2,
	MT6797_VENC_H264_WORK_BUF_SKIP_FRAME,
	MT6797_VENC_H264_WORK_BUF_MAX,
};

struct mt6797_venc_h264_work_buf_sizes {
	size_t size[MT6797_VENC_H264_WORK_BUF_MAX];
};

static inline int mt6797_venc_h264_validate_rcpu(const u8 *data, size_t size)
{
	if (!data || size != MT6797_VENC_H264_RCPU_SIZE)
		return -EINVAL;

	return 0;
}

/*
 * Calculate the direct encoder's progressive-frame working set.  The engine
 * pads reconstructed pictures to an even number of 16-pixel macroblock rows.
 * Its two motion-vector buffers instead use the unpadded macroblock height
 * and one 64-byte record for each 128-pixel-wide group in a row.
 */
static inline int
mt6797_venc_h264_calc_work_buf_sizes(u32 width, u32 height,
				     struct mt6797_venc_h264_work_buf_sizes *sizes)
{
	u64 mb_width, mb_height, mv_width, mv_height;
	u64 rec_luma, rec_chroma, mv_info;

	if (!width || !height || !sizes)
		return -EINVAL;

	mb_width = DIV_ROUND_UP_ULL(width, 16);
	mb_height = ALIGN(DIV_ROUND_UP_ULL(height, 16), 2);
	if (check_mul_overflow(mb_width, mb_height, &rec_luma) ||
	    check_mul_overflow(rec_luma, 0x100ULL, &rec_luma))
		return -EOVERFLOW;

	if (check_mul_overflow(mb_width, mb_height, &rec_chroma) ||
	    check_mul_overflow(rec_chroma, 0x80ULL, &rec_chroma))
		return -EOVERFLOW;

	mv_width = DIV_ROUND_UP_ULL(width, 128);
	mv_height = DIV_ROUND_UP_ULL(height, 16);
	if (check_mul_overflow(mv_width, mv_height, &mv_info) ||
	    check_mul_overflow(mv_info, 0x40ULL, &mv_info) ||
	    check_add_overflow(mv_info, 0x7fULL, &mv_info))
		return -EOVERFLOW;
	mv_info &= ~0x7fULL;

	if (rec_luma > SIZE_MAX || rec_chroma > SIZE_MAX || mv_info > SIZE_MAX)
		return -EOVERFLOW;

	sizes->size[MT6797_VENC_H264_WORK_BUF_RC_INFO] =
		MT6797_VENC_H264_RC_INFO_SIZE;
	sizes->size[MT6797_VENC_H264_WORK_BUF_REC_LUMA] = rec_luma;
	sizes->size[MT6797_VENC_H264_WORK_BUF_REC_CHROMA] = rec_chroma;
	sizes->size[MT6797_VENC_H264_WORK_BUF_REF_LUMA] = rec_luma;
	sizes->size[MT6797_VENC_H264_WORK_BUF_REF_CHROMA] = rec_chroma;
	sizes->size[MT6797_VENC_H264_WORK_BUF_MV_INFO_1] = mv_info;
	sizes->size[MT6797_VENC_H264_WORK_BUF_MV_INFO_2] = mv_info;
	sizes->size[MT6797_VENC_H264_WORK_BUF_SKIP_FRAME] =
		MT6797_VENC_H264_SKIP_FRAME_SIZE;

	return 0;
}

#endif /* _VENC_H264_MT6797_H_ */
