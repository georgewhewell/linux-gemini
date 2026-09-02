// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 direct H.264 encoder backend.
 *
 * MT6797 drives VENC directly rather than through the VPU/SCP IPI ABI used
 * by the other MediaTek encoder backends.  The rate-control processor image
 * is opaque and is therefore requested from userspace instead of embedded in
 * the kernel.  Keep this interface unselected by platform data until the
 * complete register sequence has been implemented and reviewed.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "../mtk_vcodec_enc.h"
#include "../venc_drv_base.h"
#include "venc_h264_mt6797.h"

struct mt6797_venc_h264_inst {
	struct mtk_vcodec_enc_ctx *ctx;
	void *rcpu_va;
	dma_addr_t rcpu_dma;
	struct mtk_vcodec_mem work_bufs[MT6797_VENC_H264_WORK_BUF_MAX];
	struct venc_enc_param config;
	bool work_bufs_allocated;
	bool configured;
	bool force_intra;
	bool prepend_header;
};

static void mt6797_venc_h264_free_work_bufs(struct mt6797_venc_h264_inst *inst)
{
	unsigned int i;

	for (i = 0; i < MT6797_VENC_H264_WORK_BUF_MAX; i++) {
		if (inst->work_bufs[i].va)
			mtk_vcodec_mem_free(inst->ctx, &inst->work_bufs[i]);
	}

	inst->work_bufs_allocated = false;
}

static int
mt6797_venc_h264_alloc_work_bufs(struct mt6797_venc_h264_inst *inst,
				 const struct venc_enc_param *config)
{
	struct mt6797_venc_h264_work_buf_sizes sizes;
	unsigned int i;
	int ret;
	u32 reg;

	ret = mt6797_venc_h264_calc_work_buf_sizes(config->width,
						   config->height, &sizes);
	if (ret)
		return ret;

	mt6797_venc_h264_free_work_bufs(inst);
	for (i = 0; i < MT6797_VENC_H264_WORK_BUF_MAX; i++) {
		dma_addr_t dma_addr;
		unsigned int align = MT6797_VENC_H264_WORK_BUF_ALIGN;
		size_t size;

		inst->work_bufs[i].size = sizes.size[i];
		ret = mtk_vcodec_mem_alloc(inst->ctx, &inst->work_bufs[i]);
		if (ret)
			goto err_free;

		dma_addr = inst->work_bufs[i].dma_addr;
		size = inst->work_bufs[i].size;
		ret = mt6797_venc_h264_dma_reg(dma_addr, size, align, &reg);
		if (ret) {
			mtk_venc_err(inst->ctx,
				     "work buffer %u DMA address %pad is outside the hardware range",
				     i, &inst->work_bufs[i].dma_addr);
			goto err_free;
		}
	}

	inst->work_bufs_allocated = true;
	return 0;

err_free:
	mt6797_venc_h264_free_work_bufs(inst);
	return ret;
}

static int mt6797_venc_h264_validate_config(const struct venc_enc_param *config)
{
	if (!config || config->input_yuv_fmt != VENC_YUV_FORMAT_NV12 ||
	    !config->width || !config->height ||
	    config->width > config->buf_width ||
	    config->height > config->buf_height ||
	    !IS_ALIGNED(config->buf_width, 16) ||
	    !IS_ALIGNED(config->buf_height, 16) ||
	    !config->frm_rate || !config->bitrate)
		return -EINVAL;

	switch (config->h264_profile) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mt6797_venc_h264_load_rcpu(struct mt6797_venc_h264_inst *inst)
{
	struct device *dev = &inst->ctx->dev->plat_dev->dev;
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, MT6797_VENC_H264_RCPU_FIRMWARE, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load %s\n",
				     MT6797_VENC_H264_RCPU_FIRMWARE);

	ret = mt6797_venc_h264_validate_rcpu(fw->data, fw->size);
	if (ret) {
		dev_err(dev, "%s has invalid size %zu (expected %#x)\n",
			MT6797_VENC_H264_RCPU_FIRMWARE, fw->size,
			MT6797_VENC_H264_RCPU_SIZE);
		goto release_firmware;
	}

	inst->rcpu_va = dma_alloc_coherent(dev, fw->size, &inst->rcpu_dma,
					   GFP_KERNEL);
	if (!inst->rcpu_va) {
		ret = -ENOMEM;
		goto release_firmware;
	}

	if (!IS_ALIGNED(inst->rcpu_dma, MT6797_VENC_H264_RCPU_ALIGN) ||
	    upper_32_bits(inst->rcpu_dma)) {
		dev_err(dev, "RCPU DMA address %pad is outside the hardware range\n",
			&inst->rcpu_dma);
		dma_free_coherent(dev, fw->size, inst->rcpu_va, inst->rcpu_dma);
		inst->rcpu_va = NULL;
		ret = -ERANGE;
		goto release_firmware;
	}

	memcpy(inst->rcpu_va, fw->data, fw->size);

release_firmware:
	release_firmware(fw);
	return ret;
}

static int mt6797_venc_h264_init(struct mtk_vcodec_enc_ctx *ctx)
{
	struct mt6797_venc_h264_inst *inst;
	int ret;

	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;

	inst->ctx = ctx;
	ret = mt6797_venc_h264_load_rcpu(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}

	ctx->drv_handle = inst;
	return 0;
}

static int mt6797_venc_h264_encode(void *handle, enum venc_start_opt opt,
				   struct venc_frm_buf *frm_buf,
				   struct mtk_vcodec_mem *bs_buf,
				   struct venc_done_result *result)
{
	return -EOPNOTSUPP;
}

static int mt6797_venc_h264_set_param(void *handle,
				      enum venc_set_param_type type,
				      struct venc_enc_param *in)
{
	struct mt6797_venc_h264_inst *inst = handle;
	int ret;

	switch (type) {
	case VENC_SET_PARAM_ENC:
		ret = mt6797_venc_h264_validate_config(in);
		if (ret)
			return ret;

		ret = mt6797_venc_h264_alloc_work_bufs(inst, in);
		if (ret)
			return ret;

		inst->config = *in;
		inst->configured = true;
		inst->force_intra = false;
		return 0;
	case VENC_SET_PARAM_FORCE_INTRA:
		if (!inst->configured)
			return -EINVAL;
		inst->force_intra = true;
		return 0;
	case VENC_SET_PARAM_ADJUST_BITRATE:
		if (!inst->configured || !in->bitrate)
			return -EINVAL;
		inst->config.bitrate = in->bitrate;
		return 0;
	case VENC_SET_PARAM_ADJUST_FRAMERATE:
		if (!inst->configured || !in->frm_rate)
			return -EINVAL;
		inst->config.frm_rate = in->frm_rate;
		return 0;
	case VENC_SET_PARAM_GOP_SIZE:
		if (!inst->configured)
			return -EINVAL;
		inst->config.gop_size = in->gop_size;
		return 0;
	case VENC_SET_PARAM_INTRA_PERIOD:
		if (!inst->configured)
			return -EINVAL;
		inst->config.intra_period = in->intra_period;
		return 0;
	case VENC_SET_PARAM_PREPEND_HEADER:
		inst->prepend_header = true;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int mt6797_venc_h264_deinit(void *handle)
{
	struct mt6797_venc_h264_inst *inst = handle;
	struct device *dev = &inst->ctx->dev->plat_dev->dev;

	mt6797_venc_h264_free_work_bufs(inst);
	dma_free_coherent(dev, MT6797_VENC_H264_RCPU_SIZE, inst->rcpu_va,
			  inst->rcpu_dma);
	kfree(inst);
	return 0;
}

const struct venc_common_if mtk_venc_6797_h264_if = {
	.init = mt6797_venc_h264_init,
	.encode = mt6797_venc_h264_encode,
	.set_param = mt6797_venc_h264_set_param,
	.deinit = mt6797_venc_h264_deinit,
};

MODULE_FIRMWARE(MT6797_VENC_H264_RCPU_FIRMWARE);
