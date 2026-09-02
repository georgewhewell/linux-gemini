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
	struct mtk_vcodec_mem prediction;
	struct mtk_vcodec_mem pp;
	struct mtk_vcodec_mem ld;
	struct mtk_vcodec_mem mv[MT6797_H264_MV_BUFFER_COUNT];
	u32 mv_size;
};

static void mt6797_h264_free_mem(struct mt6797_h264_inst *inst,
				 struct mtk_vcodec_mem *mem)
{
	if (mem->va)
		mtk_vcodec_mem_free(inst->ctx, mem);
}

static void mt6797_h264_free_mv(struct mt6797_h264_inst *inst)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(inst->mv); i++)
		mt6797_h264_free_mem(inst, &inst->mv[i]);
	inst->mv_size = 0;
}

static int mt6797_h264_alloc_mem(struct mt6797_h264_inst *inst,
				 struct mtk_vcodec_mem *mem, size_t size)
{
	int ret;

	mem->size = size;
	ret = mtk_vcodec_mem_alloc(inst->ctx, mem);
	if (!ret)
		memset(mem->va, 0, size);

	return ret;
}

static int mt6797_h264_alloc_fixed(struct mt6797_h264_inst *inst)
{
	int ret;

	ret = mt6797_h264_alloc_mem(inst, &inst->prediction,
				    MT6797_H264_PREDICTION_SIZE);
	if (ret)
		return ret;
	ret = mt6797_h264_alloc_mem(inst, &inst->pp, MT6797_H264_PP_SIZE);
	if (ret)
		goto err_prediction;
	ret = mt6797_h264_alloc_mem(inst, &inst->ld, MT6797_H264_LD_SIZE);
	if (ret)
		goto err_pp;

	return 0;

err_pp:
	mt6797_h264_free_mem(inst, &inst->pp);
err_prediction:
	mt6797_h264_free_mem(inst, &inst->prediction);
	return ret;
}

static int mt6797_h264_alloc_mv(struct mt6797_h264_inst *inst, u32 size)
{
	unsigned int i;
	int ret;

	if (inst->mv_size == size)
		return 0;

	mt6797_h264_free_mv(inst);
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++) {
		ret = mt6797_h264_alloc_mem(inst, &inst->mv[i], size);
		if (ret) {
			mt6797_h264_free_mv(inst);
			return ret;
		}
	}
	inst->mv_size = size;

	return 0;
}

static int mt6797_h264_init(struct mtk_vcodec_dec_ctx *ctx)
{
	struct mt6797_h264_inst *inst;
	int ret;

	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;

	inst->ctx = ctx;
	ret = mt6797_h264_alloc_fixed(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}
	ctx->drv_handle = inst;

	return 0;
}

static void mt6797_h264_deinit(void *h_vdec)
{
	struct mt6797_h264_inst *inst = h_vdec;

	mt6797_h264_free_mv(inst);
	mt6797_h264_free_mem(inst, &inst->ld);
	mt6797_h264_free_mem(inst, &inst->pp);
	mt6797_h264_free_mem(inst, &inst->prediction);
	kfree(inst);
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
	struct mt6797_h264_buffer_layout layout;
	struct mt6797_h264_idr_plan idr;
	struct mt6797_h264_mv_range mv_range;
	struct mt6797_h264_output_plan output;
	struct mt6797_h264_start_plan start;
	struct mt6797_h264_working_plan working;
	struct vdec_fb *fb;
	u64 mv_addrs[MT6797_H264_MV_BUFFER_COUNT];
	u32 sps_reg, pps_reg, slice_reg, slice2_reg;
	unsigned int i;
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
	ret = mt6797_h264_buffer_layout((sps->pic_width_in_mbs_minus1 + 1) * 16,
					(sps->pic_height_in_map_units_minus1 + 1) * 16,
					&layout);
	if (ret)
		return ret;
	fb = inst->ctx->dev->vdec_pdata->get_cap_buffer(inst->ctx);
	if (!fb)
		return -EBUSY;
	if (fb->base_y.size < layout.luma_size ||
	    fb->base_c.size < layout.chroma_size)
		return -ENOSPC;
	ret = mt6797_h264_alloc_mv(inst, layout.mv_size);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++)
		mv_addrs[i] = inst->mv[i].dma_addr;
	ret = mt6797_h264_mv_range_plan(mv_addrs, layout.mv_size, &mv_range);
	if (ret)
		return ret;
	ret = mt6797_h264_working_plan(inst->prediction.dma_addr,
				       inst->pp.dma_addr, inst->ld.dma_addr,
				       inst->mv[0].dma_addr, &working);
	if (ret)
		return ret;
	ret = mt6797_h264_start_plan(&layout, pps, &start);
	if (ret)
		return ret;
	ret = mt6797_h264_output_plan(fb->base_y.dma_addr,
				      fb->base_c.dma_addr,
				      inst->mv[0].dma_addr, &layout, &output);
	if (ret)
		return ret;
	ret = mt6797_h264_idr_plan(sps, dec, fb->base_y.dma_addr,
				   inst->mv[0].dma_addr, &idr);
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
	slice2_reg = mt6797_h264_slice2_reg(slice, dec);
	mtk_v4l2_vdec_dbg(3, inst->ctx,
			  "direct plan sps=%#x pps=%#x slice=%#x/%#x bs=%#x-%#x shift=%u/%u size=%#x mb=%u y=%#x c=%#x pred=%#x pp=%#x ld=%#x mv=%#x range=%#x-%#x frame=%u poc=%#x",
			  sps_reg, pps_reg, slice_reg, slice2_reg, barrel.fetch_start,
			  barrel.fetch_end, barrel.shift_reads, barrel.final_shift,
			  start.packed_size, start.mb_count,
			  output.y_addr, output.c_addr_256,
			  working.prediction_addr, working.pp_addr,
			  working.ld_addr, working.mv_addr_16,
			  mv_range.min_addr, mv_range.max_end, idr.frame_num, idr.poc);

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
		pic->cap_fourcc = V4L2_PIX_FMT_MS21;
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
