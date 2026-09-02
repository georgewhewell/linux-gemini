// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 direct HEVC encoder backend.
 *
 * This remains deliberately unselected by platform data.  It models only the
 * independently recovered normal/Main/NV12/non-UFO path with the vendor's
 * NonRefPFrmPeriod == 1 policy.  Exposing HEVC waits for coordinated hardware
 * validation and an approved source for the runtime RCPU image.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../mtk_vcodec_enc.h"
#include "../venc_drv_base.h"
#include "venc_hevc_mt6797.h"

#define MT6797_VENC_HEVC_REG_MODE		0x000
#define MT6797_VENC_HEVC_REG_SEQ_004		0x004
#define MT6797_VENC_HEVC_REG_SEQ_008		0x008
#define MT6797_VENC_HEVC_REG_SEQ_00C		0x00c
#define MT6797_VENC_HEVC_REG_FRAME_010		0x010
#define MT6797_VENC_HEVC_REG_FRAME_014		0x014
#define MT6797_VENC_HEVC_REG_FRAME_018		0x018
#define MT6797_VENC_HEVC_REG_FRAME_01C		0x01c
#define MT6797_VENC_HEVC_REG_SEQ_024		0x024
#define MT6797_VENC_HEVC_REG_SEQ_028		0x028
#define MT6797_VENC_HEVC_REG_SEQ_02C		0x02c
#define MT6797_VENC_HEVC_REG_SEQ_030		0x030
#define MT6797_VENC_HEVC_REG_SEQ_038		0x038
#define MT6797_VENC_HEVC_REG_RC_048		0x048
#define MT6797_VENC_HEVC_REG_RC_04C		0x04c
#define MT6797_VENC_HEVC_REG_RC_050		0x050
#define MT6797_VENC_HEVC_REG_RC_054		0x054
#define MT6797_VENC_HEVC_REG_START		0x058
#define MT6797_VENC_HEVC_REG_BS_ADDR		0x064
#define MT6797_VENC_HEVC_REG_BS_LIMIT		0x068
#define MT6797_VENC_HEVC_REG_SRC_Y		0x06c
#define MT6797_VENC_HEVC_REG_SRC_C		0x070
#define MT6797_VENC_HEVC_REG_REF_LUMA		0x074
#define MT6797_VENC_HEVC_REG_REF_CHROMA		0x078
#define MT6797_VENC_HEVC_REG_REC_LUMA		0x07c
#define MT6797_VENC_HEVC_REG_REC_CHROMA		0x080
#define MT6797_VENC_HEVC_REG_MV_INFO_1		0x084
#define MT6797_VENC_HEVC_REG_MV_INFO_2		0x088
#define MT6797_VENC_HEVC_REG_RCPU_CODE		0x08c
#define MT6797_VENC_HEVC_REG_RC_INFO		0x090
#define MT6797_VENC_HEVC_REG_SRC_V		0x094
#define MT6797_VENC_HEVC_REG_BS_COUNT		0x098
#define MT6797_VENC_HEVC_REG_REF_LIMIT		0x0a4
#define MT6797_VENC_HEVC_REG_RESET		0x0a8
#define MT6797_VENC_HEVC_REG_SEQ_128		0x128
#define MT6797_VENC_HEVC_REG_SEQ_12C		0x12c
#define MT6797_VENC_HEVC_REG_SEQ_1C0		0x1c0
#define MT6797_VENC_HEVC_REG_SEQ_1F0		0x1f0
#define MT6797_VENC_HEVC_REG_KICK		0x0ec

#define MT6797_VENC_HEVC_REF_LIMIT		0xbf

struct mt6797_venc_hevc_inst {
	struct mtk_vcodec_enc_ctx *ctx;
	void __iomem *hw_base;
	void *rcpu_va;
	dma_addr_t rcpu_dma;
	u32 rcpu_reg;
	struct mtk_vcodec_mem work_bufs[MT6797_VENC_HEVC_WORK_BUF_MAX];
	u32 work_buf_regs[MT6797_VENC_HEVC_WORK_BUF_MAX];
	struct venc_enc_param config;
	struct mt6797_venc_hevc_seq_regs seq_regs;
	struct mt6797_venc_hevc_rate_control rate_control;
	size_t header_sizes[3];
	u32 frame_num;
	u32 last_idr_frame;
	bool idr_toggle;
	bool work_bufs_allocated;
	bool configured;
	bool headers_ready;
	bool new_session;
	bool prepend_header;
};

static inline void
mt6797_venc_hevc_write(struct mt6797_venc_hevc_inst *inst, u32 reg, u32 value)
{
	writel(value, inst->hw_base + reg);
}

static void mt6797_venc_hevc_reset(struct mt6797_venc_hevc_inst *inst)
{
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RESET, 0);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RESET, 0);
	readl(inst->hw_base + MT6797_VENC_HEVC_REG_RESET);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RESET, 1);
	readl(inst->hw_base + MT6797_VENC_HEVC_REG_RESET);
}

static int mt6797_venc_hevc_level_idc(unsigned int level, u32 *level_idc)
{
	switch (level) {
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_1:
		*level_idc = 10;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_2:
		*level_idc = 20;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1:
		*level_idc = 21;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_3:
		*level_idc = 30;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1:
		*level_idc = 31;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4:
		*level_idc = 40;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1:
		*level_idc = 41;
		return 0;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_5:
		*level_idc = 50;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
mt6797_venc_hevc_prepare_seq_regs(const struct venc_enc_param *config,
				  struct mt6797_venc_hevc_seq_regs *regs)
{
	struct mt6797_venc_hevc_seq_params params = {
		.width = config->width,
		.height = config->height,
		.buf_width = config->buf_width,
		.buf_height = config->buf_height,
		.intra_period = config->intra_period,
		.bitrate = config->bitrate,
		.framerate = config->frm_rate,
	};
	int ret;

	ret = mt6797_venc_hevc_level_idc(config->hevc_level,
					 &params.level_idc);
	if (ret)
		return ret;

	return mt6797_venc_hevc_calc_seq_regs(&params, regs);
}

static int mt6797_venc_hevc_validate_config(const struct venc_enc_param *config)
{
	if (!config || config->input_yuv_fmt != VENC_YUV_FORMAT_NV12 ||
	    config->hevc_profile != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN ||
	    !config->width || !config->height ||
	    (config->width & 1) || (config->height & 1) ||
	    config->width > config->buf_width ||
	    config->height > config->buf_height ||
	    !IS_ALIGNED(config->buf_width, 32) ||
	    !IS_ALIGNED(config->buf_height, 32) ||
	    !config->intra_period || !config->frm_rate || !config->bitrate)
		return -EINVAL;

	return 0;
}

static void mt6797_venc_hevc_free_work_bufs(struct mt6797_venc_hevc_inst *inst)
{
	unsigned int i;

	for (i = 0; i < MT6797_VENC_HEVC_WORK_BUF_MAX; i++) {
		if (inst->work_bufs[i].va)
			mtk_vcodec_mem_free(inst->ctx, &inst->work_bufs[i]);
	}
	inst->work_bufs_allocated = false;
}

static int
mt6797_venc_hevc_alloc_work_bufs(struct mt6797_venc_hevc_inst *inst,
				 const struct venc_enc_param *config)
{
	struct mt6797_venc_hevc_work_buf_sizes sizes;
	unsigned int i;
	int ret;

	ret = mt6797_venc_hevc_calc_buf_sizes(config->width, config->height,
					      &sizes);
	if (ret)
		return ret;

	mt6797_venc_hevc_free_work_bufs(inst);
	for (i = 0; i < MT6797_VENC_HEVC_WORK_BUF_MAX; i++) {
		struct mtk_vcodec_mem *mem = &inst->work_bufs[i];

		mem->size = sizes.size[i];
		ret = mtk_vcodec_mem_alloc(inst->ctx, mem);
		if (ret)
			goto err_free;
		memset(mem->va, 0, mem->size);
		ret = mt6797_venc_hevc_dma_reg(mem->dma_addr, mem->size,
					       MT6797_VENC_HEVC_WORK_BUF_ALIGN,
					       &inst->work_buf_regs[i]);
		if (ret) {
			mtk_venc_err(inst->ctx,
				     "work buffer %u DMA address %pad is outside the hardware range",
				     i, &mem->dma_addr);
			goto err_free;
		}
	}

	inst->work_bufs_allocated = true;
	return 0;

err_free:
	mt6797_venc_hevc_free_work_bufs(inst);
	return ret;
}

static void
mt6797_venc_hevc_write_rate_regs(struct mt6797_venc_hevc_inst *inst,
				 bool frame)
{
	const struct mt6797_venc_hevc_seq_regs *regs = &inst->seq_regs;

	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RC_048,
			       frame ? regs->reg_048_frame : regs->reg_048_header);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RC_04C,
			       regs->reg_04c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RC_050,
			       mt6797_hevc_rc_reg(&inst->rate_control));
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RC_054,
			       regs->reg_054);
}

static void
mt6797_venc_hevc_write_seq_regs(struct mt6797_venc_hevc_inst *inst,
				bool frame)
{
	const struct mt6797_venc_hevc_seq_regs *regs = &inst->seq_regs;

	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_004,
			       regs->reg_004);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_008,
			       regs->reg_008);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_00C,
			       regs->reg_00c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_024,
			       regs->reg_024);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_028,
			       regs->reg_028);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_02C,
			       regs->reg_02c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_030,
			       regs->reg_030);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_038,
			       regs->reg_038);
	mt6797_venc_hevc_write_rate_regs(inst, frame);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_128,
			       regs->reg_128);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_12C,
			       regs->reg_12c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_1C0,
			       regs->reg_1c0);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_1F0,
			       regs->reg_1f0);
}

static int
mt6797_venc_hevc_wait(struct mt6797_venc_hevc_inst *inst, u32 expected_irq)
{
	struct mtk_vcodec_enc_ctx *ctx = inst->ctx;
	u32 irq_status;
	int ret;

	ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, 0);
	irq_status = ctx->irq_status;
	if (ret)
		return -ETIMEDOUT;
	if (irq_status & MTK_VENC_IRQ_STATUS_DRAM)
		return -ENOSPC;
	if (irq_status != expected_irq) {
		mtk_venc_err(ctx, "unexpected IRQ status %#x (expected %#x)",
			     irq_status, expected_irq);
		return -EIO;
	}

	return 0;
}

static int
mt6797_venc_hevc_run_header(struct mt6797_venc_hevc_inst *inst,
			    enum mt6797_venc_hevc_header_type type,
			    enum mt6797_venc_hevc_work_buf work_buf,
			    bool first_sequence_header, size_t *size)
{
	struct mt6797_venc_hevc_header_regs regs;
	u32 bs_size;
	int ret;

	ret = mt6797_venc_hevc_header_regs(type, first_sequence_header, &regs);
	if (ret)
		return ret;

	inst->ctx->irq_status = 0;
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_BS_LIMIT,
			       inst->work_bufs[work_buf].size >> 7);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_BS_ADDR,
			       inst->work_buf_regs[work_buf]);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_MODE, regs.mode);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REF_LIMIT,
			       MT6797_VENC_HEVC_REF_LIMIT);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_KICK, 1);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_START, regs.start);

	ret = mt6797_venc_hevc_wait(inst, regs.expected_irq);
	if (ret)
		return ret;

	bs_size = readl(inst->hw_base + MT6797_VENC_HEVC_REG_BS_COUNT);
	if (!bs_size || bs_size > inst->work_bufs[work_buf].size)
		return -EIO;

	*size = bs_size;
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_038, 0);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_KICK, 0);
	readl(inst->hw_base + 0x0bc);
	return 0;
}

static int
mt6797_venc_hevc_build_headers(struct mt6797_venc_hevc_inst *inst)
{
	int ret;

	mt6797_venc_hevc_reset(inst);
	mt6797_venc_hevc_write_seq_regs(inst, false);
	enable_irq(inst->ctx->dev->enc_irq);
	ret = mt6797_venc_hevc_run_header(inst, MT6797_VENC_HEVC_HEADER_SPS,
					  MT6797_VENC_HEVC_WORK_BUF_SPS,
					  inst->new_session,
					  &inst->header_sizes[0]);
	if (ret)
		goto reset_disable_irq;
	inst->new_session = false;
	ret = mt6797_venc_hevc_run_header(inst, MT6797_VENC_HEVC_HEADER_VPS,
					  MT6797_VENC_HEVC_WORK_BUF_VPS,
					  false, &inst->header_sizes[1]);
	if (ret)
		goto reset_disable_irq;
	ret = mt6797_venc_hevc_run_header(inst, MT6797_VENC_HEVC_HEADER_PPS,
					  MT6797_VENC_HEVC_WORK_BUF_PPS,
					  false, &inst->header_sizes[2]);
	if (ret)
		goto reset_disable_irq;

	disable_irq(inst->ctx->dev->enc_irq);
	inst->headers_ready = true;
	return 0;

reset_disable_irq:
	mt6797_venc_hevc_reset(inst);
	disable_irq(inst->ctx->dev->enc_irq);
	return ret;
}

static int
mt6797_venc_hevc_copy_headers(struct mt6797_venc_hevc_inst *inst,
			      void *dst, size_t capacity, size_t *total)
{
	static const enum mt6797_venc_hevc_work_buf order[] = {
		MT6797_VENC_HEVC_WORK_BUF_VPS,
		MT6797_VENC_HEVC_WORK_BUF_SPS,
		MT6797_VENC_HEVC_WORK_BUF_PPS,
	};
	static const unsigned int size_order[] = { 1, 0, 2 };
	size_t offset = 0;
	unsigned int i;

	if (!inst->headers_ready)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		size_t next;

		if (check_add_overflow(offset, inst->header_sizes[size_order[i]],
				       &next) || next > capacity)
			return -ENOSPC;
		memcpy((u8 *)dst + offset, inst->work_bufs[order[i]].va,
		       inst->header_sizes[size_order[i]]);
		offset = next;
	}
	*total = offset;
	return 0;
}

static int
mt6797_venc_hevc_encode_frame(struct mt6797_venc_hevc_inst *inst,
			      struct venc_frm_buf *frm_buf,
			      struct mtk_vcodec_mem *bs_buf,
			      struct venc_done_result *result)
{
	struct mt6797_venc_hevc_frame_params frame_params = {
		.frame_num = inst->frame_num,
		.intra_period = inst->config.intra_period,
		.last_idr_frame = inst->last_idr_frame,
		.seq_reg_038 = inst->seq_regs.reg_038,
		.idr_toggle = inst->idr_toggle,
		.sao_luma = true,
		.sao_chroma = (inst->seq_regs.reg_038 & 0x1c00) != 0x1c00,
	};
	struct mt6797_venc_hevc_frame_regs frame_regs;
	struct mt6797_venc_hevc_ref_regs ref_regs;
	struct mt6797_venc_hevc_io_bufs io_bufs;
	struct mt6797_venc_hevc_io_regs io_regs;
	size_t header_size = 0;
	size_t frame_capacity;
	u32 bs_size;
	int ret;

	if (!frm_buf || !frm_buf->fb_addr[0].size)
		return -EINVAL;
	if (inst->prepend_header) {
		if (!inst->headers_ready) {
			ret = mt6797_venc_hevc_build_headers(inst);
			if (ret)
				return ret;
		}
		ret = check_add_overflow(inst->header_sizes[0],
					 inst->header_sizes[1], &header_size);
		if (ret || check_add_overflow(header_size, inst->header_sizes[2],
					      &header_size) || header_size >= bs_buf->size)
			return -ENOSPC;
	}
	frame_capacity = round_down(bs_buf->size - header_size,
				    (size_t)MT6797_VENC_HEVC_WORK_BUF_ALIGN);
	if (!frame_capacity)
		return -ENOSPC;

	io_bufs.src_addr = frm_buf->fb_addr[0].dma_addr;
	io_bufs.src_size = frm_buf->fb_addr[0].size;
	io_bufs.buf_width = inst->config.buf_width;
	io_bufs.buf_height = inst->config.buf_height;
	io_bufs.bs_addr = bs_buf->dma_addr;
	io_bufs.bs_size = frame_capacity;
	ret = mt6797_venc_hevc_calc_io_regs(&io_bufs, &io_regs);
	if (ret)
		return ret;
	ret = mt6797_hevc_frame_regs(&frame_params, &frame_regs);
	if (ret)
		return ret;
	ret = mt6797_hevc_ref_regs(inst->work_buf_regs, inst->frame_num, &ref_regs);
	if (ret)
		return ret;

	inst->ctx->irq_status = 0;
	enable_irq(inst->ctx->dev->enc_irq);
	mt6797_venc_hevc_reset(inst);
	mt6797_venc_hevc_write_seq_regs(inst, true);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_FRAME_010,
			       frame_regs.reg_010);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_FRAME_014,
			       frame_regs.reg_014);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_FRAME_018,
			       frame_regs.reg_018);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_FRAME_01C,
			       frame_regs.reg_01c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_038,
			       frame_regs.reg_038);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RCPU_CODE,
			       inst->rcpu_reg);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_RC_INFO,
			       inst->work_buf_regs[MT6797_VENC_HEVC_WORK_BUF_RC_INFO]);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_BS_LIMIT,
			       io_regs.bs_limit);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_BS_ADDR,
			       io_regs.bs_addr);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SRC_Y, io_regs.src_y);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SRC_C, io_regs.src_c);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SRC_V, 0);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REF_LUMA,
			       ref_regs.ref_luma);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REF_CHROMA,
			       ref_regs.ref_chroma);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REC_LUMA,
			       ref_regs.rec_luma);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REC_CHROMA,
			       ref_regs.rec_chroma);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_MV_INFO_1,
			       ref_regs.mv_info_1);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_MV_INFO_2,
			       ref_regs.mv_info_2);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_REF_LIMIT,
			       MT6797_VENC_HEVC_REF_LIMIT);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_MODE,
			       frame_regs.mode);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_KICK, 1);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_START, BIT(2));

	ret = mt6797_venc_hevc_wait(inst, MTK_VENC_IRQ_STATUS_FRM);
	if (ret)
		goto reset_disable_irq;
	bs_size = readl(inst->hw_base + MT6797_VENC_HEVC_REG_BS_COUNT);
	if (!bs_size || bs_size > frame_capacity) {
		ret = -EIO;
		goto reset_disable_irq;
	}
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_SEQ_038, 0);
	mt6797_venc_hevc_write(inst, MT6797_VENC_HEVC_REG_KICK, 0);
	disable_irq(inst->ctx->dev->enc_irq);

	if (inst->prepend_header) {
		memmove((u8 *)bs_buf->va + header_size, bs_buf->va, bs_size);
		ret = mt6797_venc_hevc_copy_headers(inst, bs_buf->va, header_size,
						    &header_size);
		if (ret)
			return ret;
	}
	result->bs_size = header_size + bs_size;
	result->is_key_frm = frame_regs.is_idr;
	inst->frame_num++;
	mt6797_hevc_rc_update(&inst->rate_control, inst->config.frm_rate,
			      inst->frame_num - 1, bs_size);
	inst->last_idr_frame = frame_regs.next_last_idr_frame;
	inst->idr_toggle = frame_regs.next_idr_toggle;
	inst->prepend_header = false;
	return 0;

reset_disable_irq:
	mt6797_venc_hevc_reset(inst);
	disable_irq(inst->ctx->dev->enc_irq);
	return ret;
}

static int mt6797_venc_hevc_encode(void *handle, enum venc_start_opt opt,
				   struct venc_frm_buf *frm_buf,
				   struct mtk_vcodec_mem *bs_buf,
				   struct venc_done_result *result)
{
	struct mt6797_venc_hevc_inst *inst = handle;
	size_t header_size;
	int ret;

	if (!inst || !inst->configured || !bs_buf || !bs_buf->va || !result)
		return -EINVAL;

	switch (opt) {
	case VENC_START_OPT_ENCODE_SEQUENCE_HEADER:
		ret = mt6797_venc_hevc_build_headers(inst);
		if (ret)
			return ret;
		ret = mt6797_venc_hevc_copy_headers(inst, bs_buf->va, bs_buf->size,
						    &header_size);
		if (ret)
			return ret;
		result->bs_size = header_size;
		result->is_key_frm = false;
		return 0;
	case VENC_START_OPT_ENCODE_FRAME:
		return mt6797_venc_hevc_encode_frame(inst, frm_buf, bs_buf,
						       result);
	default:
		return -EINVAL;
	}
}

static int
mt6797_venc_hevc_update_config(struct mt6797_venc_hevc_inst *inst,
			       const struct venc_enc_param *config)
{
	struct mt6797_venc_hevc_seq_regs regs;
	int ret;

	ret = mt6797_venc_hevc_prepare_seq_regs(config, &regs);
	if (ret)
		return ret;
	inst->config = *config;
	inst->seq_regs = regs;
	inst->headers_ready = false;
	inst->new_session = true;
	return 0;
}

static int mt6797_venc_hevc_set_param(void *handle,
				      enum venc_set_param_type type,
				      struct venc_enc_param *in)
{
	struct mt6797_venc_hevc_inst *inst = handle;
	struct venc_enc_param config;
	int ret;

	switch (type) {
	case VENC_SET_PARAM_ENC:
		ret = mt6797_venc_hevc_validate_config(in);
		if (ret)
			return ret;
		inst->configured = false;
		ret = mt6797_venc_hevc_alloc_work_bufs(inst, in);
		if (ret)
			return ret;
		ret = mt6797_venc_hevc_update_config(inst, in);
		if (ret) {
			mt6797_venc_hevc_free_work_bufs(inst);
			return ret;
		}
		inst->frame_num = 0;
		inst->last_idr_frame = 0;
		inst->idr_toggle = false;
		inst->prepend_header = false;
		mt6797_hevc_rc_init(&inst->rate_control);
		inst->configured = true;
		return 0;
	case VENC_SET_PARAM_FORCE_INTRA:
		if (!inst->configured)
			return -EINVAL;
		inst->frame_num = 0;
		inst->last_idr_frame = 0;
		return 0;
	case VENC_SET_PARAM_ADJUST_BITRATE:
		if (!inst->configured || !in || !in->bitrate)
			return -EINVAL;
		config = inst->config;
		config.bitrate = in->bitrate;
		return mt6797_venc_hevc_update_config(inst, &config);
	case VENC_SET_PARAM_ADJUST_FRAMERATE:
		if (!inst->configured || !in || !in->frm_rate)
			return -EINVAL;
		config = inst->config;
		config.frm_rate = in->frm_rate;
		return mt6797_venc_hevc_update_config(inst, &config);
	case VENC_SET_PARAM_INTRA_PERIOD:
	case VENC_SET_PARAM_GOP_SIZE:
		if (!inst->configured || !in)
			return -EINVAL;
		config = inst->config;
		config.intra_period = type == VENC_SET_PARAM_GOP_SIZE ?
				      in->gop_size : in->intra_period;
		ret = mt6797_venc_hevc_update_config(inst, &config);
		if (!ret) {
			inst->frame_num = 0;
			inst->last_idr_frame = 0;
		}
		return ret;
	case VENC_SET_PARAM_PREPEND_HEADER:
		if (!inst->configured)
			return -EINVAL;
		inst->prepend_header = true;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int mt6797_venc_hevc_load_rcpu(struct mt6797_venc_hevc_inst *inst)
{
	struct device *dev = &inst->ctx->dev->plat_dev->dev;
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, MT6797_VENC_HEVC_RCPU_FIRMWARE, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load %s\n",
				     MT6797_VENC_HEVC_RCPU_FIRMWARE);
	ret = mt6797_venc_hevc_validate_rcpu(fw->data, fw->size);
	if (ret) {
		dev_err(dev, "%s has invalid size %zu (expected %#x)\n",
			MT6797_VENC_HEVC_RCPU_FIRMWARE, fw->size,
			MT6797_VENC_HEVC_RCPU_SIZE);
		goto release_firmware;
	}
	inst->rcpu_va = dma_alloc_coherent(dev, fw->size, &inst->rcpu_dma,
					   GFP_KERNEL);
	if (!inst->rcpu_va) {
		ret = -ENOMEM;
		goto release_firmware;
	}
	ret = mt6797_venc_hevc_dma_reg(inst->rcpu_dma, fw->size,
				       MT6797_VENC_HEVC_RCPU_ALIGN,
				       &inst->rcpu_reg);
	if (ret) {
		dev_err(dev, "RCPU DMA address %pad is outside the hardware range\n",
			&inst->rcpu_dma);
		dma_free_coherent(dev, fw->size, inst->rcpu_va, inst->rcpu_dma);
		inst->rcpu_va = NULL;
		goto release_firmware;
	}
	memcpy(inst->rcpu_va, fw->data, fw->size);

release_firmware:
	release_firmware(fw);
	return ret;
}

static int mt6797_venc_hevc_init(struct mtk_vcodec_enc_ctx *ctx)
{
	struct mt6797_venc_hevc_inst *inst;
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
	ret = mt6797_venc_hevc_load_rcpu(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}
	ctx->drv_handle = inst;
	return 0;
}

static int mt6797_venc_hevc_deinit(void *handle)
{
	struct mt6797_venc_hevc_inst *inst = handle;
	struct device *dev = &inst->ctx->dev->plat_dev->dev;

	mt6797_venc_hevc_free_work_bufs(inst);
	dma_free_coherent(dev, MT6797_VENC_HEVC_RCPU_SIZE, inst->rcpu_va,
			  inst->rcpu_dma);
	kfree(inst);
	return 0;
}

const struct venc_common_if mtk_venc_6797_hevc_if = {
	.init = mt6797_venc_hevc_init,
	.encode = mt6797_venc_hevc_encode,
	.set_param = mt6797_venc_hevc_set_param,
	.deinit = mt6797_venc_hevc_deinit,
};

MODULE_FIRMWARE(MT6797_VENC_HEVC_RCPU_FIRMWARE);
