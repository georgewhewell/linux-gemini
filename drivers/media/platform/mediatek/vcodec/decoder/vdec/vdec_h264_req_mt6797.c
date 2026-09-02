// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 direct H.264 Request API backend.
 *
 * MT6797 does not use the VPU/SCP command interface used by the other
 * MediaTek stateless decoder backends.  Keep the control translation here,
 * separate from those firmware ABIs.  The device-tree node stays disabled
 * until the remaining output/DPB address sequence has been verified.
 */

#include <linux/slab.h>

#include "../mtk_vcodec_dec.h"
#include "../vdec_drv_base.h"
#include "vdec_h264_req_common.h"
#include "vdec_h264_req_mt6797.h"

struct mt6797_h264_inst {
	struct mtk_vcodec_dec_ctx *ctx;
};

static int mt6797_h264_init(struct mtk_vcodec_dec_ctx *ctx)
{
	struct mt6797_h264_inst *inst;

	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;

	inst->ctx = ctx;
	ctx->drv_handle = inst;

	return 0;
}

static void mt6797_h264_deinit(void *h_vdec)
{
	kfree(h_vdec);
}

static int mt6797_h264_decode(void *h_vdec, struct mtk_vcodec_mem *bs,
			      struct vdec_fb *unused, bool *res_chg)
{
	struct mt6797_h264_inst *inst = h_vdec;
	const struct v4l2_ctrl_h264_decode_params *dec;
	const struct v4l2_ctrl_h264_slice_params *slice;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	struct mt6797_h264_barrel_plan barrel;
	u32 sps_reg, pps_reg, slice_reg;
	int ret;

	*res_chg = false;
	if (!bs)
		return 0;

	sps = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_SPS);
	if (IS_ERR(sps))
		return PTR_ERR(sps);
	pps = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_PPS);
	if (IS_ERR(pps))
		return PTR_ERR(pps);
	slice = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	if (IS_ERR(slice))
		return PTR_ERR(slice);
	dec = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	if (IS_ERR(dec))
		return PTR_ERR(dec);

	ret = mt6797_h264_validate_request(sps, pps, slice, dec);
	if (ret)
		return ret;
	ret = mt6797_h264_barrel_plan(bs->dma_addr, bs->size,
				      slice->header_bit_size,
				      &barrel);
	if (ret)
		return ret;

	/*
	 * These words are independently recovered and unit tested.  Do not
	 * issue them until the capture-layout, MV-buffer and DPB address
	 * sequence is equally established: a partly programmed decoder can DMA
	 * outside the submitted buffers.
	 */
	sps_reg = mt6797_h264_sps_reg(sps);
	pps_reg = mt6797_h264_pps_reg(pps);
	slice_reg = mt6797_h264_slice_reg(slice, false, false);
	mtk_v4l2_vdec_dbg(3, inst->ctx,
			  "direct plan sps=%#x pps=%#x slice=%#x bs=%#x-%#x shift=%u/%u",
			  sps_reg, pps_reg, slice_reg, barrel.fetch_start,
			  barrel.fetch_end, barrel.shift_reads, barrel.final_shift);

	return -EOPNOTSUPP;
}

static int mt6797_h264_get_param(void *h_vdec, enum vdec_get_param_type type,
				 void *out)
{
	struct mt6797_h264_inst *inst = h_vdec;
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mt6797_h264_buffer_layout layout;
	struct vdec_pic_info *pic;
	struct v4l2_rect *crop;
	int ret;

	switch (type) {
	case GET_PARAM_PIC_INFO:
		pic = out;
		pic->pic_w = ctx->picinfo.pic_w;
		pic->pic_h = ctx->picinfo.pic_h;
		ret = mt6797_h264_buffer_layout(pic->pic_w, pic->pic_h, &layout);
		if (ret)
			return ret;
		pic->buf_w = layout.width;
		pic->buf_h = layout.height;
		pic->fb_sz[0] = layout.luma_size;
		pic->fb_sz[1] = layout.chroma_size;
		pic->cap_fourcc = V4L2_PIX_FMT_MM21;
		break;
	case GET_PARAM_DPB_SIZE:
		*(unsigned int *)out = V4L2_H264_NUM_DPB_ENTRIES;
		break;
	case GET_PARAM_CROP_INFO:
		crop = out;
		crop->left = 0;
		crop->top = 0;
		crop->width = ctx->picinfo.pic_w;
		crop->height = ctx->picinfo.pic_h;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

const struct vdec_common_if mtk_vdec_6797_h264_slice_if = {
	.init = mt6797_h264_init,
	.decode = mt6797_h264_decode,
	.get_param = mt6797_h264_get_param,
	.deinit = mt6797_h264_deinit,
};
