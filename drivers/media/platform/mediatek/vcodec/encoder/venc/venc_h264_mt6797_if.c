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
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../mtk_vcodec_enc.h"
#include "../venc_drv_base.h"
#include "venc_h264_mt6797.h"
#include "venc_h264_mt6797_headers.h"

struct mt6797_venc_h264_inst {
	struct mtk_vcodec_enc_ctx *ctx;
	void __iomem *hw_base;
	void *rcpu_va;
	dma_addr_t rcpu_dma;
	u32 rcpu_reg;
	struct mtk_vcodec_mem work_bufs[MT6797_VENC_H264_WORK_BUF_MAX];
	u32 work_buf_regs[MT6797_VENC_H264_WORK_BUF_MAX];
	struct venc_enc_param config;
	struct mt6797_venc_h264_seq_regs seq_regs;
	u32 frame_num;
	u8 idr_pic_id;
	bool work_bufs_allocated;
	bool configured;
	bool force_intra;
	bool prepend_header;
};

#define MT6797_VENC_H264_REG_MODE		0x000
#define MT6797_VENC_H264_REG_SEQ_004		0x004
#define MT6797_VENC_H264_REG_SEQ_008		0x008
#define MT6797_VENC_H264_REG_SEQ_00C		0x00c
#define MT6797_VENC_H264_REG_FRAME_010		0x010
#define MT6797_VENC_H264_REG_FRAME_014		0x014
#define MT6797_VENC_H264_REG_FRAME_018		0x018
#define MT6797_VENC_H264_REG_FRAME_01C		0x01c
#define MT6797_VENC_H264_REG_SEQ_024		0x024
#define MT6797_VENC_H264_REG_SEQ_028		0x028
#define MT6797_VENC_H264_REG_SEQ_02C		0x02c
#define MT6797_VENC_H264_REG_SEQ_030		0x030
#define MT6797_VENC_H264_REG_RC_048		0x048
#define MT6797_VENC_H264_REG_RC_04C		0x04c
#define MT6797_VENC_H264_REG_RC_050		0x050
#define MT6797_VENC_H264_REG_RC_054		0x054
#define MT6797_VENC_H264_REG_START		0x058
#define MT6797_VENC_H264_REG_BS_ADDR		0x064
#define MT6797_VENC_H264_REG_BS_LIMIT		0x068
#define MT6797_VENC_H264_REG_SRC_Y		0x06c
#define MT6797_VENC_H264_REG_SRC_C		0x070
#define MT6797_VENC_H264_REG_REF_LUMA		0x074
#define MT6797_VENC_H264_REG_REF_CHROMA		0x078
#define MT6797_VENC_H264_REG_REC_LUMA		0x07c
#define MT6797_VENC_H264_REG_REC_CHROMA		0x080
#define MT6797_VENC_H264_REG_MV_INFO_1		0x084
#define MT6797_VENC_H264_REG_MV_INFO_2		0x088
#define MT6797_VENC_H264_REG_RCPU_CODE		0x08c
#define MT6797_VENC_H264_REG_RC_INFO		0x090
#define MT6797_VENC_H264_REG_SRC_V		0x094
#define MT6797_VENC_H264_REG_BS_COUNT		0x098
#define MT6797_VENC_H264_REG_REF_LIMIT		0x0a4
#define MT6797_VENC_H264_REG_RESET		0x0a8
#define MT6797_VENC_H264_REG_FRAME_124		0x124
#define MT6797_VENC_H264_REG_FRAME_128		0x128
#define MT6797_VENC_H264_REG_SEQ_12C		0x12c
#define MT6797_VENC_H264_REG_SEQ_0F4		0x0f4
#define MT6797_VENC_H264_REG_SEQ_0FC		0x0fc
#define MT6797_VENC_H264_REG_KICK		0x0ec

#define MT6797_VENC_H264_REF_LIMIT		0x3f
static int mt6797_venc_h264_profile_idc(unsigned int profile, u32 *profile_idc)
{
	switch (profile) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		*profile_idc = MT6797_VENC_H264_PROFILE_BASELINE;
		return 0;
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		*profile_idc = MT6797_VENC_H264_PROFILE_MAIN;
		return 0;
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		*profile_idc = MT6797_VENC_H264_PROFILE_HIGH;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mt6797_venc_h264_level_idc(unsigned int level, u32 *level_idc)
{
	switch (level) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
		*level_idc = 10;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
		*level_idc = 11;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
		*level_idc = 12;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
		*level_idc = 13;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
		*level_idc = 20;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
		*level_idc = 21;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
		*level_idc = 22;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
		*level_idc = 30;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
		*level_idc = 31;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
		*level_idc = 32;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
		*level_idc = 40;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
		*level_idc = 41;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
		*level_idc = 42;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		*level_idc = 50;
		return 0;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
		*level_idc = 51;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
mt6797_venc_h264_prepare_seq_regs(const struct venc_enc_param *config,
				  struct mt6797_venc_h264_seq_regs *regs)
{
	struct mt6797_venc_h264_seq_params params = {
		.width = config->width,
		.height = config->height,
		.buf_width = config->buf_width,
		.buf_height = config->buf_height,
		.intra_period = config->intra_period,
		.gop_size = config->gop_size,
		.bitrate = config->bitrate,
		.framerate = config->frm_rate,
	};
	int ret;

	ret = mt6797_venc_h264_profile_idc(config->h264_profile,
					   &params.profile_idc);
	if (ret)
		return ret;

	ret = mt6797_venc_h264_level_idc(config->h264_level,
					 &params.level_idc);
	if (ret)
		return ret;

	return mt6797_venc_h264_calc_seq_regs(&params, regs);
}

static int
mt6797_venc_h264_update_config(struct mt6797_venc_h264_inst *inst,
			       const struct venc_enc_param *config)
{
	struct mt6797_venc_h264_seq_regs regs;
	int ret;

	ret = mt6797_venc_h264_prepare_seq_regs(config, &regs);
	if (ret)
		return ret;

	inst->config = *config;
	inst->seq_regs = regs;
	return 0;
}

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
		inst->work_buf_regs[i] = reg;
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
	    (config->width & 1) || (config->height & 1) ||
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

static inline void
mt6797_venc_h264_write(struct mt6797_venc_h264_inst *inst, u32 reg, u32 value)
{
	writel(value, inst->hw_base + reg);
}

static void
mt6797_venc_h264_write_rate_regs(struct mt6797_venc_h264_inst *inst)
{
	const struct mt6797_venc_h264_seq_regs *regs = &inst->seq_regs;

	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RC_048,
			       regs->reg_048);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RC_04C,
			       regs->reg_04c);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RC_050,
			       regs->reg_050);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RC_054,
			       regs->reg_054);
}

static void
mt6797_venc_h264_write_seq_regs(struct mt6797_venc_h264_inst *inst)
{
	const struct mt6797_venc_h264_seq_regs *regs = &inst->seq_regs;

	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_004,
			       regs->reg_004);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_008,
			       regs->reg_008);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_00C,
			       regs->reg_00c);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_024,
			       regs->reg_024);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_028,
			       regs->reg_028);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_02C,
			       regs->reg_02c);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_030,
			       regs->reg_030);
	mt6797_venc_h264_write_rate_regs(inst);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_12C,
			       regs->reg_12c);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_0F4,
			       regs->reg_0f4);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SEQ_0FC,
			       regs->reg_0fc);
}

static void
mt6797_venc_h264_write_frame_regs(struct mt6797_venc_h264_inst *inst,
				  const struct mt6797_venc_h264_frame_regs *regs)
{
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_010,
			       regs->reg_010);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_014,
			       regs->reg_014);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_018,
			       regs->reg_018);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_01C,
			       regs->reg_01c);
	mt6797_venc_h264_write_rate_regs(inst);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_124,
			       regs->reg_124);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_FRAME_128,
			       regs->reg_128);
}

static void mt6797_venc_h264_reset(struct mt6797_venc_h264_inst *inst)
{
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RESET, 0);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RESET, 0);
	readl(inst->hw_base + MT6797_VENC_H264_REG_RESET);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RESET, 1);
	readl(inst->hw_base + MT6797_VENC_H264_REG_RESET);
}

static int
mt6797_venc_h264_header_params(struct mt6797_venc_h264_inst *inst,
			       struct mt6797_venc_h264_header_params *params)
{
	int ret;

	params->width = inst->config.width;
	params->height = inst->config.height;
	ret = mt6797_venc_h264_profile_idc(inst->config.h264_profile,
					   &params->profile_idc);
	if (ret)
		return ret;

	return mt6797_venc_h264_level_idc(inst->config.h264_level,
					  &params->level_idc);
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

	ret = mt6797_venc_h264_dma_reg(inst->rcpu_dma, fw->size,
				       MT6797_VENC_H264_RCPU_ALIGN,
				       &inst->rcpu_reg);
	if (ret) {
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
	inst->hw_base = mtk_vcodec_get_reg_addr(ctx->dev->reg_base, VENC_SYS);
	if (!inst->hw_base) {
		kfree(inst);
		return -ENODEV;
	}

	ret = mt6797_venc_h264_load_rcpu(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}

	ctx->drv_handle = inst;
	return 0;
}

static int
mt6797_venc_h264_encode_frame(struct mt6797_venc_h264_inst *inst,
			      struct venc_frm_buf *frm_buf,
			      struct mtk_vcodec_mem *bs_buf,
			      struct venc_done_result *result)
{
	struct mt6797_venc_h264_header_params header_params;
	struct mt6797_venc_h264_frame_params frame_params;
	struct mt6797_venc_h264_frame_regs frame_regs;
	struct mt6797_venc_h264_ref_regs ref_regs;
	struct mt6797_venc_h264_io_bufs io_bufs;
	struct mt6797_venc_h264_io_regs io_regs;
	struct mtk_vcodec_mem frame_bs = *bs_buf;
	struct mtk_vcodec_enc_ctx *ctx = inst->ctx;
	unsigned int irq_status;
	size_t frame_offset = 0;
	u32 bs_size;
	int ret;

	if (!frm_buf || !frm_buf->fb_addr[0].size)
		return -EINVAL;

	ret = mt6797_venc_h264_header_params(inst, &header_params);
	if (ret)
		return ret;

	if (inst->prepend_header) {
		ret = mt6797_venc_h264_build_prepend(&header_params, bs_buf->va,
						     bs_buf->size,
						       &frame_offset);
		if (ret)
			return ret;
		frame_bs.va = (u8 *)bs_buf->va + frame_offset;
		frame_bs.dma_addr = bs_buf->dma_addr + frame_offset;
		frame_bs.size = bs_buf->size - frame_offset;
	}
	frame_bs.size = round_down(frame_bs.size,
				   (size_t)MT6797_VENC_H264_BS_ALIGN);
	if (!frame_bs.size)
		return -ENOSPC;

	io_bufs.src_addr = frm_buf->fb_addr[0].dma_addr;
	io_bufs.src_size = frm_buf->fb_addr[0].size;
	io_bufs.buf_width = inst->config.buf_width;
	io_bufs.buf_height = inst->config.buf_height;
	io_bufs.bs_addr = frame_bs.dma_addr;
	io_bufs.bs_size = frame_bs.size;
	ret = mt6797_venc_h264_calc_io_regs(&io_bufs, &io_regs);
	if (ret)
		return ret;

	frame_params.frame_num = inst->frame_num;
	frame_params.intra_period = inst->config.intra_period;
	frame_params.gop_size = inst->config.gop_size;
	frame_params.profile_idc = header_params.profile_idc;
	frame_params.idr_pic_id = inst->idr_pic_id;
	frame_params.force_intra = inst->force_intra;
	ret = mt6797_venc_h264_calc_frame_regs(&frame_params, &frame_regs);
	if (ret)
		return ret;

	ret = mt6797_venc_h264_calc_ref_regs(inst->work_buf_regs,
					     inst->frame_num, &ref_regs);
	if (ret)
		return ret;

	ctx->irq_status = 0;
	enable_irq(ctx->dev->enc_irq);
	mt6797_venc_h264_reset(inst);
	mt6797_venc_h264_write_seq_regs(inst);
	mt6797_venc_h264_write_frame_regs(inst, &frame_regs);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RCPU_CODE,
			       inst->rcpu_reg);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_RC_INFO,
			       inst->work_buf_regs[MT6797_VENC_H264_WORK_BUF_RC_INFO]);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_BS_LIMIT,
			       io_regs.bs_limit);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_BS_ADDR,
			       io_regs.bs_addr);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SRC_Y,
			       io_regs.src_y);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SRC_C,
			       io_regs.src_c);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_SRC_V, 0);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_REF_LUMA,
			       ref_regs.ref_luma);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_REF_CHROMA,
			       ref_regs.ref_chroma);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_REC_LUMA,
			       ref_regs.rec_luma);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_REC_CHROMA,
			       ref_regs.rec_chroma);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_MV_INFO_1,
			       ref_regs.mv_info_1);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_MV_INFO_2,
			       ref_regs.mv_info_2);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_REF_LIMIT,
			       MT6797_VENC_H264_REF_LIMIT);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_MODE,
			       frame_regs.mode);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_KICK, 1);
	mt6797_venc_h264_write(inst, MT6797_VENC_H264_REG_START, 4);

	ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, 0);
	irq_status = ctx->irq_status;
	if (ret) {
		ret = -ETIMEDOUT;
		goto reset_disable_irq;
	}
	if (irq_status == MTK_VENC_IRQ_STATUS_DRAM) {
		ret = -ENOSPC;
		goto reset_disable_irq;
	}
	if (irq_status != MTK_VENC_IRQ_STATUS_FRM) {
		mtk_venc_err(ctx, "unexpected IRQ status %#x", irq_status);
		ret = -EIO;
		goto reset_disable_irq;
	}

	bs_size = readl(inst->hw_base + MT6797_VENC_H264_REG_BS_COUNT);
	if (!bs_size || bs_size > frame_bs.size) {
		mtk_venc_err(ctx, "invalid bitstream size %u (limit %zu)",
			     bs_size, frame_bs.size);
		ret = -EIO;
		goto reset_disable_irq;
	}

	disable_irq(ctx->dev->enc_irq);
	result->bs_size = frame_offset + bs_size;
	result->is_key_frm = frame_regs.is_intra;
	inst->frame_num++;
	inst->idr_pic_id = frame_regs.next_idr_pic_id;
	inst->force_intra = false;
	inst->prepend_header = false;
	return 0;

reset_disable_irq:
	mt6797_venc_h264_reset(inst);
	disable_irq(ctx->dev->enc_irq);
	return ret;
}

static int mt6797_venc_h264_encode(void *handle, enum venc_start_opt opt,
				   struct venc_frm_buf *frm_buf,
				   struct mtk_vcodec_mem *bs_buf,
				   struct venc_done_result *result)
{
	struct mt6797_venc_h264_header_params params;
	struct mt6797_venc_h264_header_sizes sizes;
	struct mt6797_venc_h264_inst *inst = handle;
	int ret;

	if (!inst || !inst->configured || !bs_buf || !bs_buf->va || !result)
		return -EINVAL;

	switch (opt) {
	case VENC_START_OPT_ENCODE_SEQUENCE_HEADER:
		ret = mt6797_venc_h264_header_params(inst, &params);
		if (ret)
			return ret;
		ret = mt6797_venc_h264_build_headers(&params, bs_buf->va,
						     bs_buf->size, &sizes);
		if (ret)
			return ret;
		result->bs_size = sizes.sps + sizes.pps;
		result->is_key_frm = false;
		return 0;
	case VENC_START_OPT_ENCODE_FRAME:
		return mt6797_venc_h264_encode_frame(inst, frm_buf, bs_buf,
						       result);
	default:
		return -EINVAL;
	}
}

static int mt6797_venc_h264_set_param(void *handle,
				      enum venc_set_param_type type,
				      struct venc_enc_param *in)
{
	struct mt6797_venc_h264_inst *inst = handle;
	struct mt6797_venc_h264_seq_regs regs;
	struct venc_enc_param config;
	int ret;

	switch (type) {
	case VENC_SET_PARAM_ENC:
		ret = mt6797_venc_h264_validate_config(in);
		if (ret)
			return ret;
		ret = mt6797_venc_h264_prepare_seq_regs(in, &regs);
		if (ret)
			return ret;

		inst->configured = false;
		ret = mt6797_venc_h264_alloc_work_bufs(inst, in);
		if (ret)
			return ret;

		inst->config = *in;
		inst->seq_regs = regs;
		inst->configured = true;
		inst->frame_num = 0;
		inst->idr_pic_id = 0;
		inst->force_intra = false;
		inst->prepend_header = false;
		return 0;
	case VENC_SET_PARAM_FORCE_INTRA:
		if (!inst->configured)
			return -EINVAL;
		inst->force_intra = true;
		return 0;
	case VENC_SET_PARAM_ADJUST_BITRATE:
		if (!inst->configured || !in || !in->bitrate)
			return -EINVAL;
		config = inst->config;
		config.bitrate = in->bitrate;
		return mt6797_venc_h264_update_config(inst, &config);
	case VENC_SET_PARAM_ADJUST_FRAMERATE:
		if (!inst->configured || !in || !in->frm_rate)
			return -EINVAL;
		config = inst->config;
		config.frm_rate = in->frm_rate;
		return mt6797_venc_h264_update_config(inst, &config);
	case VENC_SET_PARAM_GOP_SIZE:
		if (!inst->configured || !in)
			return -EINVAL;
		config = inst->config;
		config.gop_size = in->gop_size;
		ret = mt6797_venc_h264_update_config(inst, &config);
		if (!ret) {
			inst->frame_num = 0;
			inst->idr_pic_id = 0;
		}
		return ret;
	case VENC_SET_PARAM_INTRA_PERIOD:
		if (!inst->configured || !in)
			return -EINVAL;
		config = inst->config;
		config.intra_period = in->intra_period;
		ret = mt6797_venc_h264_update_config(inst, &config);
		if (!ret) {
			inst->frame_num = 0;
			inst->idr_pic_id = 0;
		}
		return ret;
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
