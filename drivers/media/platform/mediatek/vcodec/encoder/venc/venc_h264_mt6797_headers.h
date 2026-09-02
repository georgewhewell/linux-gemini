/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENC_H264_MT6797_HEADERS_H_
#define _VENC_H264_MT6797_HEADERS_H_

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "venc_h264_mt6797.h"

#define MT6797_VENC_H264_HEADER_RBSP_SIZE	128U

struct mt6797_venc_h264_header_params {
	u32 width;
	u32 height;
	u32 profile_idc;
	u32 level_idc;
};

struct mt6797_venc_h264_header_sizes {
	size_t sps;
	size_t pps;
};

struct mt6797_venc_h264_bit_writer {
	u8 *data;
	size_t size;
	size_t bitpos;
	int error;
};

static inline void
mt6797_venc_h264_put_bits(struct mt6797_venc_h264_bit_writer *writer,
			  u32 value, unsigned int count)
{
	unsigned int i;

	if (writer->error)
		return;
	if (count > 32 || count > writer->size * 8 - writer->bitpos) {
		writer->error = -ENOSPC;
		return;
	}

	for (i = 0; i < count; i++) {
		unsigned int shift = count - i - 1;
		size_t bit = writer->bitpos++;

		writer->data[bit / 8] |= ((value >> shift) & 1) <<
					 (7 - bit % 8);
	}
}

static inline void
mt6797_venc_h264_put_ue(struct mt6797_venc_h264_bit_writer *writer, u32 value)
{
	u32 code_num;
	unsigned int bits;

	if (writer->error)
		return;
	if (value == U32_MAX) {
		writer->error = -ERANGE;
		return;
	}

	code_num = value + 1;
	bits = fls(code_num);
	mt6797_venc_h264_put_bits(writer, 0, bits - 1);
	mt6797_venc_h264_put_bits(writer, code_num, bits);
}

static inline int
mt6797_venc_h264_rbsp_trailing(struct mt6797_venc_h264_bit_writer *writer)
{
	mt6797_venc_h264_put_bits(writer, 1, 1);

	if (writer->bitpos & 7)
		mt6797_venc_h264_put_bits(writer, 0, 8 - (writer->bitpos & 7));

	return writer->error;
}

static inline int
mt6797_venc_h264_emit_nal(u8 *dst, size_t size, u8 nal_header,
			  const struct mt6797_venc_h264_bit_writer *rbsp,
			  size_t *written)
{
	size_t dst_pos = 0;
	size_t src_pos;
	unsigned int zero_count = 0;

	if (!dst || !rbsp || !written || (rbsp->bitpos & 7) || size < 5)
		return -EINVAL;

	dst[dst_pos++] = 0;
	dst[dst_pos++] = 0;
	dst[dst_pos++] = 0;
	dst[dst_pos++] = 1;
	dst[dst_pos++] = nal_header;

	for (src_pos = 0; src_pos < rbsp->bitpos / 8; src_pos++) {
		u8 byte = rbsp->data[src_pos];

		if (zero_count == 2 && byte <= 3) {
			if (dst_pos == size)
				return -ENOSPC;
			dst[dst_pos++] = 3;
			zero_count = 0;
		}
		if (dst_pos == size)
			return -ENOSPC;
		dst[dst_pos++] = byte;
		zero_count = byte ? 0 : zero_count + 1;
	}

	*written = dst_pos;
	return 0;
}

static inline int
mt6797_venc_h264_build_sps(const struct mt6797_venc_h264_header_params *params,
			   u8 *dst, size_t size, size_t *written)
{
	u8 data[MT6797_VENC_H264_HEADER_RBSP_SIZE] = { };
	struct mt6797_venc_h264_bit_writer writer = {
		.data = data,
		.size = sizeof(data),
	};
	u32 mb_width, mb_height, crop_right, crop_bottom;
	int ret;

#define PUT_BITS(value, count) mt6797_venc_h264_put_bits(&writer, value, count)
#define PUT_UE(value) mt6797_venc_h264_put_ue(&writer, value)

	if (!params || !dst || !written || !params->width || !params->height ||
	    (params->width & 1) || (params->height & 1))
		return -EINVAL;

	switch (params->profile_idc) {
	case MT6797_VENC_H264_PROFILE_BASELINE:
	case MT6797_VENC_H264_PROFILE_MAIN:
	case MT6797_VENC_H264_PROFILE_HIGH:
		break;
	default:
		return -EINVAL;
	}

	mb_width = DIV_ROUND_UP(params->width, 16);
	mb_height = DIV_ROUND_UP(params->height, 16);
	crop_right = (mb_width * 16 - params->width) / 2;
	crop_bottom = (mb_height * 16 - params->height) / 2;

	PUT_BITS(params->profile_idc, 8);
	PUT_BITS(0, 8);
	PUT_BITS(params->level_idc, 8);
	PUT_UE(0);
	if (params->profile_idc == MT6797_VENC_H264_PROFILE_HIGH) {
		PUT_UE(1);
		PUT_UE(0);
		PUT_UE(0);
		PUT_BITS(0, 1);
		PUT_BITS(0, 1);
	}
	PUT_UE(12);
	PUT_UE(0);
	PUT_UE(12);
	PUT_UE(1);
	PUT_BITS(0, 1);
	PUT_UE(mb_width - 1);
	PUT_UE(mb_height - 1);
	PUT_BITS(1, 1);
	PUT_BITS(1, 1);
	PUT_BITS(crop_right || crop_bottom, 1);
	if (crop_right || crop_bottom) {
		PUT_UE(0);
		PUT_UE(crop_right);
		PUT_UE(0);
		PUT_UE(crop_bottom);
	}

	PUT_BITS(1, 1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	PUT_BITS(1, 1);
	PUT_BITS(5, 3);
	PUT_BITS(0, 1);
	PUT_BITS(1, 1);
	PUT_BITS(5, 8);
	PUT_BITS(2, 8);
	PUT_BITS(2, 8);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	PUT_BITS(1, 1);
	PUT_BITS(1, 1);
	PUT_UE(0);
	PUT_UE(0);
	PUT_UE(16);
	PUT_UE(16);
	PUT_UE(1);
	PUT_UE(2);

	ret = mt6797_venc_h264_rbsp_trailing(&writer);
	if (ret)
		return ret;

	return mt6797_venc_h264_emit_nal(dst, size, 0x67, &writer, written);

#undef PUT_UE
#undef PUT_BITS
}

static inline int
mt6797_venc_h264_build_pps(const struct mt6797_venc_h264_header_params *params,
			   u8 *dst, size_t size, size_t *written)
{
	u8 data[MT6797_VENC_H264_HEADER_RBSP_SIZE] = { };
	struct mt6797_venc_h264_bit_writer writer = {
		.data = data,
		.size = sizeof(data),
	};
	bool high_tools;
	int ret;

#define PUT_BITS(value, count) mt6797_venc_h264_put_bits(&writer, value, count)
#define PUT_UE(value) mt6797_venc_h264_put_ue(&writer, value)

	if (!params || !dst || !written)
		return -EINVAL;

	switch (params->profile_idc) {
	case MT6797_VENC_H264_PROFILE_BASELINE:
		high_tools = false;
		break;
	case MT6797_VENC_H264_PROFILE_MAIN:
	case MT6797_VENC_H264_PROFILE_HIGH:
		high_tools = true;
		break;
	default:
		return -EINVAL;
	}
	PUT_UE(0);
	PUT_UE(0);
	PUT_BITS(high_tools, 1);
	PUT_BITS(0, 1);
	PUT_UE(0);
	PUT_UE(1);
	PUT_UE(1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 2);
	PUT_UE(0);
	PUT_UE(0);
	PUT_UE(0);
	PUT_BITS(1, 1);
	PUT_BITS(0, 1);
	PUT_BITS(0, 1);
	if (high_tools) {
		PUT_BITS(1, 1);
		PUT_BITS(0, 1);
		PUT_UE(0);
	}

	ret = mt6797_venc_h264_rbsp_trailing(&writer);
	if (ret)
		return ret;

	return mt6797_venc_h264_emit_nal(dst, size, 0x68, &writer, written);

#undef PUT_UE
#undef PUT_BITS
}

static inline int
mt6797_venc_h264_build_headers(const struct mt6797_venc_h264_header_params *params,
			       u8 *dst, size_t size,
			       struct mt6797_venc_h264_header_sizes *sizes)
{
	int ret;

	if (!sizes)
		return -EINVAL;

	ret = mt6797_venc_h264_build_sps(params, dst, size, &sizes->sps);
	if (ret)
		return ret;
	ret = mt6797_venc_h264_build_pps(params, dst + sizes->sps,
					 size - sizes->sps, &sizes->pps);
	if (ret)
		return ret;

	return 0;
}

#endif /* _VENC_H264_MT6797_HEADERS_H_ */
