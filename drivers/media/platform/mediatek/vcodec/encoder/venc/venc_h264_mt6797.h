/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENC_H264_MT6797_H_
#define _VENC_H264_MT6797_H_

#include <linux/errno.h>
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

static inline int mt6797_venc_h264_validate_rcpu(const u8 *data, size_t size)
{
	if (!data || size != MT6797_VENC_H264_RCPU_SIZE)
		return -EINVAL;

	return 0;
}

#endif /* _VENC_H264_MT6797_H_ */
