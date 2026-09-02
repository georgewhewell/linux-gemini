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
};

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
	return -EOPNOTSUPP;
}

static int mt6797_venc_h264_deinit(void *handle)
{
	struct mt6797_venc_h264_inst *inst = handle;
	struct device *dev = &inst->ctx->dev->plat_dev->dev;

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
