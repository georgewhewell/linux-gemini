// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 direct H.264 Request API backend.
 *
 * MT6797 does not use the VPU/SCP command interface used by the other
 * MediaTek stateless decoder backends.  Keep the control translation here,
 * separate from those firmware ABIs.  The device-tree node stays disabled
 * until this backend has been validated on hardware.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../mtk_vcodec_dec.h"
#include "../vdec_drv_base.h"
#include "vdec_h264_req_common.h"
#include "vdec_h264_req_mt6797.h"

#define MT6797_H264_RESET_POLL_COUNT	50000U
#define MT6797_H264_FETCH_POLL_COUNT	0x1000U
#define MT6797_H264_START_CODE_POLL_COUNT 0x100000U
#define MT6797_H264_AV_READY_POLL_COUNT	0x100000U

struct mt6797_h264_regs {
	void __iomem *misc;
	void __iomem *ld;
	void __iomem *top;
	void __iomem *mc;
	void __iomem *ad;
	void __iomem *av;
	void __iomem *pp;
};

struct mt6797_h264_inst {
	struct mtk_vcodec_dec_ctx *ctx;
	struct mt6797_h264_regs regs;
	struct mtk_vcodec_mem prediction;
	struct mtk_vcodec_mem pp;
	struct mtk_vcodec_mem ld;
	struct mtk_vcodec_mem bitstream;
	struct mtk_vcodec_mem mv[MT6797_H264_MV_BUFFER_COUNT];
	struct mt6797_h264_mv_slot mv_slots[MT6797_H264_MV_BUFFER_COUNT];
	struct v4l2_ctrl_h264_slice_params slice_params;
	struct mt6797_h264_p_ref_plan p_refs;
	struct mt6797_h264_b_ref_plan b_refs;
	struct mt6797_h264_ref_buffer ref_buffers[V4L2_H264_NUM_DPB_ENTRIES];
	u32 mv_size;
};

static bool mt6797_h264_poll_set(void __iomem *addr, u32 mask,
				 unsigned int count)
{
	while (count--) {
		if (readl(addr) & mask)
			return true;
		cpu_relax();
	}

	return false;
}

static bool mt6797_h264_poll_clear(void __iomem *addr, u32 mask,
				   unsigned int count)
{
	while (count--) {
		if (!(readl(addr) & mask))
			return true;
		cpu_relax();
	}

	return false;
}

static int mt6797_h264_get_regs(struct mt6797_h264_inst *inst)
{
	void __iomem **base = inst->ctx->dev->reg_base;
	struct mt6797_h264_regs *regs = &inst->regs;

	regs->misc = mtk_vcodec_get_reg_addr(base, VDEC_MISC);
	regs->ld = mtk_vcodec_get_reg_addr(base, VDEC_LD);
	regs->top = mtk_vcodec_get_reg_addr(base, VDEC_TOP);
	regs->mc = mtk_vcodec_get_reg_addr(base, VDEC_CM);
	regs->ad = mtk_vcodec_get_reg_addr(base, VDEC_AD);
	regs->av = mtk_vcodec_get_reg_addr(base, VDEC_AV);
	regs->pp = mtk_vcodec_get_reg_addr(base, VDEC_PP);

	if (!regs->misc || !regs->ld || !regs->top || !regs->mc ||
	    !regs->ad || !regs->av || !regs->pp)
		return -ENODEV;

	return 0;
}

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
	memset(inst->mv_slots, 0, sizeof(inst->mv_slots));
	inst->mv_size = 0;
}

static int
mt6797_h264_fill_ref_buffers(struct mt6797_h264_inst *inst,
			     const struct v4l2_ctrl_h264_decode_params *dec,
			     const struct mt6797_h264_mv_slot_plan *slot_plan,
			     const struct mt6797_h264_buffer_layout *layout,
			     struct mt6797_h264_ref_buffer *buffers)
{
	struct vb2_queue *vq;
	unsigned int i;
	u32 frame_size;

	frame_size = layout->luma_size + layout->chroma_size;
	vq = v4l2_m2m_get_vq(inst->ctx->m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *dpb = &dec->dpb[i];
		struct vb2_buffer *vb;
		u8 slot;

		if (!(dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;

		slot = slot_plan->dpb_slots[i];
		if (slot >= ARRAY_SIZE(inst->mv))
			return -EINVAL;
		vb = vb2_find_buffer(vq, dpb->reference_ts);
		if (!vb) {
			mtk_v4l2_vdec_err(inst->ctx,
					  "missing DPB %u timestamp %llu", i,
					  dpb->reference_ts);
			return -ENOENT;
		}
		if (vb2_plane_size(vb, 0) < frame_size)
			return -ENOSPC;

		buffers[i].y_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
		buffers[i].mv_addr = inst->mv[slot].dma_addr;
		buffers[i].dpb_slot = slot;
	}

	return 0;
}

static int mt6797_h264_reset(struct mt6797_h264_inst *inst)
{
	struct mt6797_h264_regs *regs = &inst->regs;
	u32 val;
	int ret;

	ret = mtk_vcodec_write_vdecsys(inst->ctx, 0, 1);
	if (ret)
		return ret;
	val = readl(regs->ld + 0xf4);
	if ((val & BIT(15)) &&
	    !mt6797_h264_poll_set(regs->ld + 0xf4, BIT(0),
				    MT6797_H264_RESET_POLL_COUNT))
		return -ETIMEDOUT;

	writel(0x101, regs->ld + 0x108);
	writel(2, regs->misc + 0x84);
	val = readl(regs->misc);
	writel((val & 0xff400000) | 0x00801ff0, regs->misc);
	writel(0x0000fff7, regs->misc + 0xc8);
	writel(0x51e30182, regs->misc + 0xcc);
	writel(0xfffffe60, regs->misc + 0x178);
	writel(0, regs->misc + 0xf4);
	writel(2, regs->misc + 0x84);
	writel(0, regs->ld + 0x108);
	ret = mtk_vcodec_write_vdecsys(inst->ctx, 0x18, 1);
	if (ret)
		return ret;
	writel(1, regs->misc + 0xec);
	writel(0x10, regs->misc + 4);
	writel(0x0065ff10, regs->misc + 0x160);

	return 0;
}

static int
mt6797_h264_init_barrel(struct mt6797_h264_inst *inst,
			const struct mt6797_h264_barrel_plan *barrel,
			const struct mt6797_h264_working_plan *working)
{
	struct mt6797_h264_regs *regs = &inst->regs;
	u32 remaining, val;
	unsigned int i;

	writel(0x00040000, regs->ld + 0xc0);
	writel(0xae00e007, regs->ad + 0x84);
	writel(readl(regs->top + 0x54) | BIT(0), regs->top + 0x54);
	writel(6, regs->mc + 0x24);
	writel(barrel->fetch_start, regs->ld + 0xb4);
	writel(barrel->fetch_end, regs->ld + 0xb8);
	writel(working->prediction_addr, regs->top + 0x28);
	writel(1, regs->ad + 0x28c);
	writel(0, regs->ad + 0x28c);

	val = readl(regs->ld + 0xf4);
	if ((val & BIT(15)) &&
	    !mt6797_h264_poll_set(regs->ld + 0xf4, BIT(0),
				    MT6797_H264_RESET_POLL_COUNT))
		return -ETIMEDOUT;
	writel(readl(regs->ld + 0x110) | BIT(0), regs->ld + 0x110);
	writel(barrel->start_addr, regs->ld + 0xb0);
	writel(barrel->start_addr, regs->ld + 0xb0);
	writel(barrel->end_addr + MT6797_H264_BS_GUARD, regs->ld + 0x110);
	writel(1, regs->ad + 0x28c);
	writel(0x100, regs->ld + 0x108);
	writel(0, regs->ld + 0x108);
	writel(0, regs->ld + 0xcc);
	writel(0x00100000, regs->ld + 0x8c);
	if (!mt6797_h264_poll_set(regs->ld + 0xe8, BIT(0),
				  MT6797_H264_FETCH_POLL_COUNT))
		return -ETIMEDOUT;
	writel(0x00800000, regs->ld + 0x8c);
	writel(0, regs->ad + 0x28c);

	val = readl(regs->ad);
	if ((val & 0xffffff00) != 0x00000100 ||
	    (s32)readl(regs->ad + 0x84) < 0) {
		writel(1, regs->ad + 0x2d8);
		if (!mt6797_h264_poll_clear(regs->ad + 0x2d8, BIT(0),
					    MT6797_H264_START_CODE_POLL_COUNT) &&
		    !(readl(regs->ad + 0x2c8) & BIT(11)))
			return -ENODATA;
	}

	/* Consume the Annex B start code and NAL header. */
	readl(regs->ad + 0x80);
	remaining = (barrel->shift_reads - 1) * MT6797_H264_BARREL_MAX_SHIFT +
		    barrel->final_shift;
	for (i = 0; i < barrel->shift_reads; i++) {
		u32 chunk = mt6797_h264_barrel_shift_chunk(remaining);

		readl(regs->ad + chunk * sizeof(u32));
		remaining -= chunk;
	}

	return 0;
}

static void
mt6797_h264_program_controls(struct mt6797_h264_inst *inst,
			     const struct v4l2_ctrl_h264_sps *sps,
			     const struct v4l2_ctrl_h264_pps *pps,
			     const struct v4l2_ctrl_h264_slice_params *slice,
			     const struct v4l2_ctrl_h264_decode_params *dec)
{
	struct mt6797_h264_regs *regs = &inst->regs;
	u32 slice_reg = mt6797_h264_slice_reg(slice, false, false);
	u32 val;

	writel(mt6797_h264_sps_reg(sps), regs->ad + 0x88);
	writel(sps->pic_width_in_mbs_minus1 |
	       sps->pic_height_in_map_units_minus1 << 16, regs->top + 0x68);
	writel(sps->bit_depth_luma_minus8 |
	       sps->bit_depth_chroma_minus8 << 4, regs->top + 0x100);
	writel(mt6797_h264_pps_reg(pps), regs->ad + 0x90);
	writel(mt6797_h264_pps_id_reg(pps), regs->ad + 0x94);

	val = readl(regs->ad + 0x8c);
	writel((val & ~MT6797_H264_SH_FIRST_MB) |
	       (slice_reg & MT6797_H264_SH_FIRST_MB), regs->ad + 0x8c);
	val = readl(regs->ad + 0x98);
	writel((val & MT6797_H264_SH_FIRST_MB) |
	       (slice_reg & ~MT6797_H264_SH_FIRST_MB), regs->ad + 0x98);
	writel(mt6797_h264_slice2_reg(slice, dec), regs->ad + 0x9c);

	writel((u32)min(dec->top_field_order_cnt,
			dec->bottom_field_order_cnt) & MT6797_H264_POC_VALUE,
	       regs->av + 0x200);
	writel((u32)dec->top_field_order_cnt & MT6797_H264_POC_VALUE,
	       regs->av + 0x204);
	writel((u32)dec->bottom_field_order_cnt & MT6797_H264_POC_VALUE,
	       regs->av + 0x208);
}

static int
mt6797_h264_program_refs(struct mt6797_h264_inst *inst,
			 const struct v4l2_ctrl_h264_sps *sps,
			 const struct v4l2_ctrl_h264_decode_params *dec,
			 const struct mt6797_h264_output_plan *output,
			 const struct mt6797_h264_p_ref_plan *p_refs,
			 const struct mt6797_h264_b_ref_plan *b_refs)
{
	struct mt6797_h264_regs *regs = &inst->regs;
	unsigned int i;

	writel(0, regs->mc + 0x564);
	writel(1, regs->ad + 0x228);
	if (!mt6797_h264_poll_clear(regs->ad + 0x228, BIT(0),
				    MT6797_H264_AV_READY_POLL_COUNT))
		return -ETIMEDOUT;
	writel(1U << (sps->log2_max_frame_num_minus4 + 4), regs->ad + 0xa0);
	writel(dec->frame_num, regs->ad + 0xa4);

	for (i = 0; i < V4L2_H264_REF_LIST_LEN; i++) {
		writel(output->y_addr, regs->mc + 0x3dc + i * 4);
		writel(output->mv_addr_16, regs->av + 0x180 + i * 4);
		writel(output->y_addr, regs->mc + 0x45c + i * 4);
		writel(output->y_addr, regs->mc + 0x4dc + i * 4);
	}
	writel(0, regs->mc + 0x568);
	writel(0, regs->mc + 0x56c);

	if (p_refs) {
		for (i = 0; i < p_refs->num_refs; i++) {
			const struct mt6797_h264_p_ref *ref = &p_refs->refs[i];

			writel(ref->y_addr, regs->mc + 0x3dc + i * 4);
			writel(ref->pic_num, regs->ad + 0xa8 + i * 4);
			writel(ref->top, regs->av + i * 8);
			writel(ref->bottom, regs->av + i * 8 + 4);
		}
	}
	if (b_refs) {
		for (i = 0; i < b_refs->num_l0; i++) {
			const struct mt6797_h264_b_ref *ref = &b_refs->l0[i];

			writel(ref->y_addr, regs->mc + 0x45c + i * 4);
			writel(ref->pic_num, regs->ad + 0x128 + i * 4);
			writel(ref->top, regs->av + 0x80 + i * 8);
			writel(ref->bottom, regs->av + 0x84 + i * 8);
		}
		for (i = 0; i < b_refs->num_l1; i++) {
			const struct mt6797_h264_b_ref *ref = &b_refs->l1[i];

			writel(ref->y_addr, regs->mc + 0x4dc + i * 4);
			writel(ref->pic_num, regs->ad + 0x1a8 + i * 4);
			writel(ref->top, regs->av + 0x100 + i * 8);
			writel(ref->bottom, regs->av + 0x104 + i * 8);
			writel(ref->mv_top_addr_16, regs->av + 0x180 + i * 8);
			writel(ref->mv_bottom_addr_16, regs->av + 0x184 + i * 8);
		}
	}

	return 0;
}

static int
mt6797_h264_start(struct mt6797_h264_inst *inst,
		  const struct mt6797_h264_start_plan *start,
		  const struct mt6797_h264_output_plan *output,
		  const struct mt6797_h264_working_plan *working,
		  const struct mt6797_h264_mv_range *mv_range)
{
	struct mt6797_h264_regs *regs = &inst->regs;
	u32 complete, error, mb_x, mb_y;
	int ret;

	writel(3, regs->misc + 4);
	writel(4, regs->mc + 0x90);
	writel(U32_MAX, regs->top + 0x58);
	writel(start->width, regs->mc + 0x208);
	writel(start->height, regs->mc + 0x20c);
	writel(0x10, regs->misc + 4);
	writel(start->packed_size, regs->top + 0x70);
	writel(0xbfdfb0f7, regs->ad + 0x280);
	writel(1, regs->mc + 0x220);
	writel(output->y_addr_512, regs->mc + 0x224);
	writel(output->c_addr_256, regs->mc + 0x228);
	writel(1, regs->mc + 0x250);
	writel(3, regs->mc + 0x238);
	writel(start->mb_width_minus1, regs->mc + 0x260);
	writel(start->mb_height_minus1, regs->mc + 0x264);
	writel(start->mb_width, regs->mc + 0x22c);
	writel(output->end_addr_16, regs->mc + 0x850);
	writel(output->y_addr_16, regs->mc + 0x854);
	writel(1, regs->mc + 0x858);
	writel(0x00020000, regs->mc + 0x5e4);
	writel(output->y_addr, regs->mc + 0x560);
	writel(output->c_offset, regs->mc + 0x55c);
	writel(start->mb_width, regs->mc + 0x980);
	writel(0xf9040039, regs->ad + 0x2c8);
	writel(output->mv_addr_16, regs->av + 0x20c);
	if (start->constrained_intra_pred)
		writel(1, regs->ad + 0x234);
	if (!mt6797_h264_poll_clear(regs->ad + 0x25c, BIT(25),
				    MT6797_H264_AV_READY_POLL_COUNT))
		return -ETIMEDOUT;
	writel(readl(regs->mc + 0x5e4) | 0x1000, regs->mc + 0x5e4);
	writel(readl(regs->mc + 0x920) | BIT(24), regs->mc + 0x920);
	writel((readl(regs->mc + 0x768) & ~GENMASK(2, 0)) | 4,
	       regs->mc + 0x768);
	writel(readl(regs->misc + 0xf0) | BIT(0), regs->misc + 0xf0);
	writel(start->mb_count, regs->top + 0x6c);
	writel(U32_MAX, regs->top + 0x50);
	writel(working->ld_addr, regs->mc + 0xa20);
	writel(working->pp_addr, regs->mc + 0xa24);
	writel(0x101, regs->ad + 0x2d0);
	writel(0xc210, regs->ad + 0x2d4);
	writel(readl(regs->misc + 0x28) | BIT(0), regs->misc + 0x28);
	writel(output->crc_y_addr, regs->misc + 0x3c);
	writel(output->crc_end_addr, regs->misc + 0x40);
	writel(mv_range->min_addr, regs->misc + 0x54);
	writel(mv_range->max_end, regs->misc + 0x58);
	writel(0xae00e007, regs->ad + 0x84);

	inst->ctx->int_cond[MTK_VDEC_CORE] = 0;
	inst->ctx->int_type[MTK_VDEC_CORE] = 0;
	writel(1, regs->ad + 0x238);
	ret = mtk_vcodec_wait_for_done_ctx(inst->ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, MTK_VDEC_CORE);
	if (ret)
		return -ETIMEDOUT;

	complete = readl(regs->ad + 0x278);
	error = readl(regs->ad + 0x280) & readl(regs->ad + 0x284);
	mb_x = readl(regs->mc + 0x28);
	mb_y = readl(regs->mc + 0x2c);
	if (!(complete & BIT(31)) || (error && error != 8)) {
		mtk_v4l2_vdec_err(inst->ctx,
				  "decode failed complete=%#x error=%#x mb=%u,%u",
				  complete, error, mb_x, mb_y);
		return -EIO;
	}

	return 0;
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

static int mt6797_h264_stage_bitstream(struct mt6797_h264_inst *inst,
				       const struct mtk_vcodec_mem *source,
				       struct mtk_vcodec_mem *staged)
{
	static const u8 end_marker[MT6797_H264_END_MARKER_SIZE] = {
		0x00, 0x00, 0x00, 0x01, 0x09,
	};
	size_t allocation_size;
	int ret;

	ret = mt6797_h264_staging_size(source->size, &allocation_size);
	if (ret)
		return ret;
	if (inst->bitstream.size < allocation_size) {
		mt6797_h264_free_mem(inst, &inst->bitstream);
		ret = mt6797_h264_alloc_mem(inst, &inst->bitstream,
					    allocation_size);
		if (ret)
			return ret;
	}

	memcpy(inst->bitstream.va, source->va, source->size);
	memset(inst->bitstream.va + source->size, 0, MT6797_H264_BS_GUARD);
	memcpy(inst->bitstream.va + source->size, end_marker,
	       sizeof(end_marker));

	*staged = inst->bitstream;
	staged->size = source->size;
	return 0;
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
	ret = mt6797_h264_get_regs(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}
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

	mt6797_h264_free_mem(inst, &inst->bitstream);
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
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	struct v4l2_h264_reflist_builder reflist_builder;
	struct v4l2_ctrl_h264_slice_params *slice = &inst->slice_params;
	struct v4l2_h264_reference *ref_list0 = slice->ref_pic_list0;
	struct v4l2_h264_reference *ref_list1 = slice->ref_pic_list1;
	struct mt6797_h264_barrel_plan barrel;
	struct mt6797_h264_buffer_layout layout;
	struct mt6797_h264_idr_plan idr;
	struct mt6797_h264_b_ref_plan *b_refs = &inst->b_refs;
	struct mt6797_h264_mv_range mv_range;
	struct mt6797_h264_mv_slot_plan mv_slots;
	struct mt6797_h264_output_plan output;
	struct mt6797_h264_p_ref_plan *p_refs = &inst->p_refs;
	struct mt6797_h264_ref_buffer *ref_buffers = inst->ref_buffers;
	struct mt6797_h264_start_plan start;
	struct mt6797_h264_working_plan working;
	struct mtk_vcodec_mem staged_bs;
	struct mtk_video_dec_buf *dst_buf, *src_buf;
	struct vdec_fb *fb;
	u64 mv_addrs[MT6797_H264_MV_BUFFER_COUNT];
	u32 coded_height, coded_width;
	u32 sps_reg, pps_reg, slice_reg, slice2_reg;
	unsigned int i;
	int ret;

	*res_chg = false;
	if (!bs)
		return 0;
	memset(slice, 0, sizeof(*slice));
	memset(p_refs, 0, sizeof(*p_refs));
	memset(b_refs, 0, sizeof(*b_refs));
	memset(ref_buffers, 0, sizeof(inst->ref_buffers));

	sps = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_SPS);
	if (IS_ERR(sps))
		return PTR_ERR(sps);
	pps = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_PPS);
	if (IS_ERR(pps))
		return PTR_ERR(pps);
	dec = mtk_vdec_h264_get_ctrl_ptr(inst->ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	if (IS_ERR(dec))
		return PTR_ERR(dec);

	ret = mt6797_h264_parse_frame_headers(bs->va, bs->size, sps, pps, dec, slice);
	if (ret)
		return ret;
	if (slice->slice_type == V4L2_H264_SLICE_TYPE_P ||
	    slice->slice_type == V4L2_H264_SLICE_TYPE_B) {
		v4l2_h264_init_reflist_builder(&reflist_builder, dec, sps, dec->dpb);
		if (slice->slice_type == V4L2_H264_SLICE_TYPE_P)
			v4l2_h264_build_p_ref_list(&reflist_builder, ref_list0);
		else
			v4l2_h264_build_b_ref_lists(&reflist_builder, ref_list0, ref_list1);
	}
	ret = mt6797_h264_validate_request(sps, pps, slice, dec);
	if (ret)
		return ret;
	ret = mt6797_h264_stage_bitstream(inst, bs, &staged_bs);
	if (ret)
		return ret;
	ret = mt6797_h264_barrel_plan(staged_bs.dma_addr, staged_bs.size,
				      slice->header_bit_size,
				      &barrel);
	if (ret)
		return ret;
	coded_width = (sps->pic_width_in_mbs_minus1 + 1) * 16;
	coded_height = (sps->pic_height_in_map_units_minus1 + 1) * 16;
	ret = mt6797_h264_buffer_layout(coded_width, coded_height, &layout);
	if (ret)
		return ret;
	fb = inst->ctx->dev->vdec_pdata->get_cap_buffer(inst->ctx);
	if (!fb)
		return -EBUSY;
	if (fb->base_y.size < layout.luma_size ||
	    fb->base_c.size < layout.chroma_size)
		return -ENOSPC;
	src_buf = container_of(bs, struct mtk_video_dec_buf, bs_buffer);
	dst_buf = container_of(fb, struct mtk_video_dec_buf, frame_buffer);
	v4l2_m2m_buf_copy_metadata(&src_buf->m2m_buf.vb, &dst_buf->m2m_buf.vb);
	ret = mt6797_h264_alloc_mv(inst, layout.mv_size);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++)
		mv_addrs[i] = inst->mv[i].dma_addr;
	ret = mt6797_h264_mv_range_plan(mv_addrs, layout.mv_size, &mv_range);
	if (ret)
		return ret;
	ret = mt6797_h264_mv_slot_plan(dec, inst->mv_slots, &mv_slots);
	if (ret)
		return ret;
	ret = mt6797_h264_working_plan(inst->prediction.dma_addr,
				       inst->pp.dma_addr, inst->ld.dma_addr,
				       inst->mv[mv_slots.current_slot].dma_addr,
				       &working);
	if (ret)
		return ret;
	ret = mt6797_h264_start_plan(coded_width, coded_height, &layout, pps,
				     &start);
	if (ret)
		return ret;
	ret = mt6797_h264_output_plan(fb->base_y.dma_addr,
				      fb->base_c.dma_addr,
				      inst->mv[mv_slots.current_slot].dma_addr,
				      &layout, &output);
	if (ret)
		return ret;
	if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) {
		ret = mt6797_h264_idr_plan(sps, dec, fb->base_y.dma_addr,
					   inst->mv[mv_slots.current_slot].dma_addr,
					   &idr);
		if (ret)
			return ret;
	} else {
		ret = mt6797_h264_fill_ref_buffers(inst, dec, &mv_slots,
						   &layout, ref_buffers);
		if (ret)
			return ret;
		if (slice->slice_type == V4L2_H264_SLICE_TYPE_P)
			ret = mt6797_h264_p_ref_plan(slice, dec, ref_buffers,
						     p_refs);
		else
			ret = mt6797_h264_b_ref_plan(slice, dec, ref_buffers,
						     b_refs);
		if (ret)
			return ret;
	}

	sps_reg = mt6797_h264_sps_reg(sps);
	pps_reg = mt6797_h264_pps_reg(pps);
	slice_reg = mt6797_h264_slice_reg(slice, false, false);
	slice2_reg = mt6797_h264_slice2_reg(slice, dec);
	mtk_v4l2_vdec_dbg(3, inst->ctx,
			  "direct plan sps=%#x pps=%#x slice=%#x/%#x bs=%#x-%#x shift=%u/%u size=%#x mb=%u y=%#x c=%#x pred=%#x pp=%#x ld=%#x mv=%#x slot=%u range=%#x-%#x",
			  sps_reg, pps_reg, slice_reg, slice2_reg, barrel.fetch_start,
			  barrel.fetch_end, barrel.shift_reads, barrel.final_shift,
			  start.packed_size, start.mb_count,
			  output.y_addr, output.c_addr_256,
			  working.prediction_addr, working.pp_addr,
			  working.ld_addr, working.mv_addr_16,
			  mv_slots.current_slot, mv_range.min_addr, mv_range.max_end);
	if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)
		mtk_v4l2_vdec_dbg(3, inst->ctx,
				  "direct IDR frame=%u poc=%#x", idr.frame_num,
				  idr.poc);
	else if (slice->slice_type == V4L2_H264_SLICE_TYPE_P)
		mtk_v4l2_vdec_dbg(3, inst->ctx, "direct P refs=%u",
				  p_refs->num_refs);
	else
		mtk_v4l2_vdec_dbg(3, inst->ctx, "direct B refs=%u/%u",
				  b_refs->num_l0, b_refs->num_l1);

	ret = mt6797_h264_mv_slot_begin(inst->mv_slots,
					mv_slots.current_slot);
	if (ret)
		return ret;
	ret = mt6797_h264_reset(inst);
	if (ret)
		return ret;
	ret = mt6797_h264_init_barrel(inst, &barrel, &working);
	if (ret)
		return ret;
	mt6797_h264_program_controls(inst, sps, pps, slice, dec);
	if (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)
		ret = mt6797_h264_program_refs(inst, sps, dec, &output,
					       NULL, NULL);
	else if (slice->slice_type == V4L2_H264_SLICE_TYPE_P)
		ret = mt6797_h264_program_refs(inst, sps, dec, &output,
					       p_refs, NULL);
	else
		ret = mt6797_h264_program_refs(inst, sps, dec, &output,
					       NULL, b_refs);
	if (ret)
		return ret;
	ret = mt6797_h264_start(inst, &start, &output, &working, &mv_range);
	if (ret)
		return ret;

	return mt6797_h264_mv_slot_commit(inst->mv_slots,
					  mv_slots.current_slot,
					  dst_buf->m2m_buf.vb.vb2_buf.timestamp);
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
