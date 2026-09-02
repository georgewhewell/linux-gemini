// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 direct HEVC Request API backend.
 *
 * The shipping MT6797 path is host-programmed and has no VPU/SCP decoder
 * firmware ABI.  Keep this backend dormant until the remaining inter-frame
 * header-parser operations are implemented and hardware-tested.  In
 * particular, no platform data selects this interface and no HEVC format is
 * advertised yet.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "../../common/mtk_vcodec_intr.h"
#include "../mtk_vcodec_dec.h"
#include "../vdec_drv_base.h"
#include "vdec_hevc_req_mt6797.h"

#define MT6797_HEVC_RESET_POLL_COUNT 50000U
#define MT6797_HEVC_FETCH_POLL_COUNT 50000U

struct mt6797_hevc_regs {
	void __iomem *misc;
	void __iomem *ld;
	void __iomem *top;
	void __iomem *mc;
	void __iomem *mv;
	void __iomem *pp;
	void __iomem *hevc;
};

struct mt6797_hevc_inst {
	struct mtk_vcodec_dec_ctx *ctx;
	struct mt6797_hevc_regs regs;
	struct mtk_vcodec_mem bitstream;
	struct mtk_vcodec_mem mv[MT6797_HEVC_MV_BUFFER_COUNT];
	struct mt6797_hevc_mv_slot mv_slots[MT6797_HEVC_MV_BUFFER_COUNT];
	struct mt6797_hevc_rps_set sps_rps[MT6797_HEVC_MAX_SPS_RPS];
	u32 mv_size;
};

static bool mt6797_hevc_poll_set(void __iomem *addr, u32 mask,
				 unsigned int count)
{
	while (count--) {
		if (readl(addr) & mask)
			return true;
		cpu_relax();
	}

	return false;
}

static int mt6797_hevc_get_regs(struct mt6797_hevc_inst *inst)
{
	void __iomem **base = inst->ctx->dev->reg_base;
	struct mt6797_hevc_regs *regs = &inst->regs;

	regs->misc = mtk_vcodec_get_reg_addr(base, VDEC_MISC);
	regs->ld = mtk_vcodec_get_reg_addr(base, VDEC_LD);
	regs->top = mtk_vcodec_get_reg_addr(base, VDEC_TOP);
	regs->mc = mtk_vcodec_get_reg_addr(base, VDEC_CM);
	regs->mv = mtk_vcodec_get_reg_addr(base, VDEC_AV);
	regs->pp = mtk_vcodec_get_reg_addr(base, VDEC_PP);
	regs->hevc = mtk_vcodec_get_reg_addr(base, VDEC_HWB);
	if (!regs->misc || !regs->ld || !regs->top || !regs->mc || !regs->mv ||
	    !regs->pp || !regs->hevc)
		return -ENODEV;

	return 0;
}

static void mt6797_hevc_free_mem(struct mt6797_hevc_inst *inst,
				 struct mtk_vcodec_mem *mem)
{
	if (mem->va)
		mtk_vcodec_mem_free(inst->ctx, mem);
}

static int mt6797_hevc_alloc_mem(struct mt6797_hevc_inst *inst,
				 struct mtk_vcodec_mem *mem, size_t size)
{
	int ret;

	mem->size = size;
	ret = mtk_vcodec_mem_alloc(inst->ctx, mem);
	if (!ret)
		memset(mem->va, 0, size);

	return ret;
}

static void mt6797_hevc_free_mv(struct mt6797_hevc_inst *inst)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(inst->mv); i++)
		mt6797_hevc_free_mem(inst, &inst->mv[i]);
	memset(inst->mv_slots, 0, sizeof(inst->mv_slots));
	inst->mv_size = 0;
}

static int mt6797_hevc_alloc_mv(struct mt6797_hevc_inst *inst, u32 size)
{
	unsigned int i;
	int ret;

	if (inst->mv_size == size)
		return 0;

	mt6797_hevc_free_mv(inst);
	for (i = 0; i < ARRAY_SIZE(inst->mv); i++) {
		ret = mt6797_hevc_alloc_mem(inst, &inst->mv[i], size);
		if (ret) {
			mt6797_hevc_free_mv(inst);
			return ret;
		}
	}
	inst->mv_size = size;

	return 0;
}

static void *mt6797_hevc_get_ctrl(struct mt6797_hevc_inst *inst, u32 id)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&inst->ctx->ctrl_hdl, id);
	if (!ctrl)
		return ERR_PTR(-EINVAL);

	return ctrl->p_cur.p;
}

static struct v4l2_ctrl_hevc_slice_params *
mt6797_hevc_get_slices(struct mt6797_hevc_inst *inst, unsigned int *count)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&inst->ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	if (!ctrl)
		return ERR_PTR(-EINVAL);
	*count = ctrl->new_elems;

	return ctrl->p_cur.p;
}

static int
mt6797_hevc_fill_ref_buffers(struct mt6797_hevc_inst *inst,
			     const struct v4l2_ctrl_hevc_decode_params *dec,
			     const struct mt6797_hevc_mv_slot_plan *slots,
			     const struct mt6797_hevc_buffer_layout *layout,
			     struct mt6797_hevc_ref_buffer *buffers)
{
	struct vb2_queue *vq;
	unsigned int i;
	u32 frame_size;

	frame_size = layout->luma_size + layout->chroma_size;
	vq = v4l2_m2m_get_vq(inst->ctx->m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	for (i = 0; i < dec->num_active_dpb_entries; i++) {
		const struct v4l2_hevc_dpb_entry *dpb = &dec->dpb[i];
		struct mt6797_hevc_output_plan output;
		struct vb2_buffer *vb;
		u64 c_addr;
		u8 slot = slots->dpb_slots[i];
		int ret;

		if (slot >= ARRAY_SIZE(inst->mv))
			return -EINVAL;
		vb = vb2_find_buffer(vq, dpb->timestamp);
		if (!vb) {
			mtk_v4l2_vdec_err(inst->ctx,
					  "missing HEVC DPB %u timestamp %llu",
					  i, dpb->timestamp);
			return -ENOENT;
		}
		if (vb2_plane_size(vb, 0) < frame_size)
			return -ENOSPC;

		buffers[i].y_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
		if (check_add_overflow(buffers[i].y_addr,
				       (u64)layout->luma_size, &c_addr))
			return -ERANGE;
		ret = mt6797_hevc_output_plan(buffers[i].y_addr, c_addr, layout,
					      &output);
		if (ret)
			return ret;

		buffers[i].reference_ts = dpb->timestamp;
		buffers[i].c_addr = c_addr;
		buffers[i].mv_addr = inst->mv[slot].dma_addr;
		buffers[i].mv_slot = slot;
		buffers[i].valid = true;
	}

	return 0;
}

static int mt6797_hevc_stage_slice(struct mt6797_hevc_inst *inst,
				   const struct mtk_vcodec_mem *source,
				   u32 offset, struct mtk_vcodec_mem *staged)
{
	size_t allocation_size, payload_size;
	int ret;

	if (!source->va || offset >= source->size)
		return -EINVAL;
	payload_size = source->size - offset;
	ret = mt6797_hevc_staging_size(payload_size, &allocation_size);
	if (ret)
		return ret;
	if (inst->bitstream.size < allocation_size) {
		mt6797_hevc_free_mem(inst, &inst->bitstream);
		ret = mt6797_hevc_alloc_mem(inst, &inst->bitstream,
					    allocation_size);
		if (ret)
			return ret;
	}

	memcpy(inst->bitstream.va, (u8 *)source->va + offset, payload_size);
	memset(inst->bitstream.va + payload_size, 0, MT6797_HEVC_BS_GUARD);
	*staged = inst->bitstream;
	staged->size = payload_size;

	return 0;
}

static int mt6797_hevc_reset(struct mt6797_hevc_inst *inst)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	u32 val;
	int ret;

	val = readl(regs->ld + 0xf4);
	if ((val & BIT(31)) &&
	    !mt6797_hevc_poll_set(regs->ld + 0xf4, BIT(0),
				  MT6797_HEVC_RESET_POLL_COUNT))
		return -ETIMEDOUT;

	writel(0x101, regs->ld + 0x108);
	writel(0, regs->misc + 0xf4);
	writel(0x101, regs->ld + 0x108);
	writel(0x0000feff, regs->misc + 0xc8);
	writel(0x77e30182, regs->misc + 0xcc);
	writel(0xfffffa60, regs->misc + 0x178);
	writel(2, regs->misc + 0x84);
	writel(0, regs->ld + 0x108);
	ret = mtk_vcodec_write_vdecsys(inst->ctx, 0x18, 1);
	if (ret)
		return ret;
	writel(1, regs->misc + 0xec);
	writel(0x100, regs->misc + 4);

	return 0;
}

static void mt6797_hevc_safe_break(struct mt6797_hevc_inst *inst)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	unsigned int count = MT6797_HEVC_RESET_POLL_COUNT;
	u32 val;

	writel(readl(regs->misc + 0x100) | BIT(0), regs->misc + 0x100);
	do {
		val = readl(regs->misc + 0x104);
		if ((val & 0x11) == 0x11)
			break;
		cpu_relax();
	} while (--count);
}

static int mt6797_hevc_init_barrel(struct mt6797_hevc_inst *inst,
				   const struct mt6797_hevc_barrel_plan *plan)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	u32 val;

	writel(1, regs->hevc + 0x84);
	writel(0x10000000, regs->ld + 0xec);
	writel(plan->fetch_start_64, regs->ld + 0xb4);
	writel(plan->fetch_end_64, regs->ld + 0xb8);

	val = readl(regs->ld + 0xf4);
	if ((val & BIT(31)) &&
	    !mt6797_hevc_poll_set(regs->ld + 0xf4, BIT(0),
				  MT6797_HEVC_RESET_POLL_COUNT))
		return -ETIMEDOUT;
	writel(plan->read_start, regs->ld + 0xb0);
	writel(plan->read_end, regs->ld + 0x110);
	writel(0x100, regs->ld + 0x108);
	writel(0, regs->ld + 0x108);
	writel(0x00100000, regs->ld + 0x8c);
	if (!mt6797_hevc_poll_set(regs->ld + 0xe8, BIT(0),
				  MT6797_HEVC_FETCH_POLL_COUNT))
		return -ETIMEDOUT;
	writel(0x00800000, regs->ld + 0x8c);

	return 0;
}

static void
mt6797_hevc_program_output(struct mt6797_hevc_inst *inst,
			   const struct v4l2_ctrl_hevc_sps *sps,
			   const struct mt6797_hevc_output_plan *output,
			   u32 current_mv)
{
	struct mt6797_hevc_regs *regs = &inst->regs;

	writel(readl(regs->mc + 0xaf8) | BIT(8), regs->mc + 0xaf8);
	writel(sps->pic_width_in_luma_samples, regs->mc + 0x208);
	writel(sps->pic_height_in_luma_samples, regs->mc + 0x20c);
	writel(output->mb_width, regs->mc + 0x980);
	writel(output->c_offset, regs->mc + 0x934);
	writel(1, regs->mc + 0x220);
	writel(output->y_addr_512, regs->mc + 0x224);
	writel(output->c_addr_256, regs->mc + 0x228);
	writel(output->mb_width, regs->mc + 0x22c);
	writel(3, regs->mc + 0x238);
	writel(1, regs->mc + 0x250);
	writel(output->mb_width - 1, regs->mc + 0x260);
	writel(output->mb_height - 1, regs->mc + 0x264);
	writel(current_mv >> 4, regs->mv + 0x20c);
	writel(1, regs->mc + 0xb5c);
}

static void
mt6797_hevc_program_refs(struct mt6797_hevc_inst *inst,
			 const struct mt6797_hevc_ref_plan *refs)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	unsigned int i;

	writel(0, regs->mc + 0xa60);
	writel(refs->current_after_all, regs->mv + 0x320);
	if (!refs->num_l0 && !refs->num_l1)
		return;

	writel(refs->long_term_flags, regs->mv + 0x324);
	for (i = 0; i < MT6797_HEVC_REF_LIST_LEN; i++) {
		if (refs->num_l0) {
			writel(refs->l0[i].mv_addr_16, regs->mv + i * 4);
			writel(refs->l0[i].y_addr, regs->mc + 0x45c + i * 4);
			writel(refs->l0[i].c_addr, regs->mc + 0xbe0 + i * 4);
			writel(refs->l0[i].poc_slot, regs->mv + 0x338 + i * 4);
		}
		if (refs->num_l1) {
			writel(refs->l1[i].mv_addr_16, regs->mv + 0x40 + i * 4);
			writel(refs->l1[i].y_addr, regs->mc + 0x4dc + i * 4);
			writel(refs->l1[i].c_addr, regs->mc + 0xc60 + i * 4);
			writel(refs->l1[i].poc_slot, regs->mv + 0x378 + i * 4);
		}
	}
}

static void
mt6797_hevc_program_syntax(struct mt6797_hevc_inst *inst,
			   const struct mt6797_hevc_sps_regs *sps,
			   const struct mt6797_hevc_pps_regs *pps,
			   const struct mt6797_hevc_rps_regs *rps,
			   const struct mt6797_hevc_slice_regs *slice,
			   const struct mt6797_hevc_tile_regs *tiles)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	unsigned int i;

	writel(0, regs->pp + 0x630);
	writel(slice->vld_0bc, regs->hevc + 0xbc);
	writel(slice->vld_0c0, regs->hevc + 0xc0);
	writel(slice->vld_0c4, regs->hevc + 0xc4);
	writel(rps->vld_0c8, regs->hevc + 0xc8);
	writel(slice->vld_0f4, regs->hevc + 0xf4);
	writel(sps->top_068, regs->top + 0x68);
	writel(sps->top_070, regs->top + 0x70);
	writel(sps->top_094, regs->top + 0x94);
	writel(sps->top_100, regs->top + 0x100);
	writel(sps->vld_0a0, regs->hevc + 0xa0);
	writel(sps->vld_0a4, regs->hevc + 0xa4);
	writel(sps->vld_0a8, regs->hevc + 0xa8);
	writel(pps->vld_0ac, regs->hevc + 0xac);
	writel(pps->vld_0b0, regs->hevc + 0xb0);
	writel(pps->vld_0b4, regs->hevc + 0xb4);
	writel(pps->vld_0b8, regs->hevc + 0xb8);
	for (i = 0; i < MT6797_HEVC_TILE_WORDS - 1; i++) {
		writel(tiles->column[i], regs->hevc + 0x140 + i * 4);
		writel(tiles->row[i], regs->hevc + 0x180 + i * 4);
	}
	writel(tiles->column[MT6797_HEVC_TILE_WORDS - 1], regs->hevc + 0x1c0);
	writel(tiles->row[MT6797_HEVC_TILE_WORDS - 1], regs->hevc + 0x1d0);
}

static int mt6797_hevc_start(struct mt6797_hevc_inst *inst, u32 ctb_width,
			     u32 ctb_height)
{
	struct mt6797_hevc_regs *regs = &inst->regs;
	u32 error, lcu, complete;
	int ret;

	inst->ctx->int_cond[MTK_VDEC_CORE] = 0;
	inst->ctx->int_type[MTK_VDEC_CORE] = 0;
	readl(regs->hevc);
	writel(1, regs->hevc + 0x90);
	ret = mtk_vcodec_wait_for_done_ctx(inst->ctx, MTK_INST_IRQ_RECEIVED,
					   WAIT_INTR_TIMEOUT_MS, MTK_VDEC_CORE);
	if (ret) {
		mt6797_hevc_safe_break(inst);
		return -ETIMEDOUT;
	}

	error = (readl(regs->hevc + 0xe8) | readl(regs->hevc + 0xe4)) &
		readl(regs->hevc + 0xe0);
	lcu = readl(regs->hevc + 0x12c);
	complete = readl(regs->hevc + 0x278);
	if (error || (!(complete & BIT(31)) &&
		      (FIELD_GET(GENMASK(15, 0), lcu) != ctb_width - 1 ||
		       FIELD_GET(GENMASK(31, 16), lcu) != ctb_height - 1))) {
		mtk_v4l2_vdec_err(inst->ctx,
				  "HEVC decode failed error=%#x lcu=%#x done=%#x",
				  error, lcu, complete);
		mt6797_hevc_safe_break(inst);
		return -EIO;
	}

	return 0;
}

static int
mt6797_hevc_validate_request(const struct v4l2_ctrl_hevc_sps *sps,
			     const struct v4l2_ctrl_hevc_pps *pps,
			     const struct v4l2_ctrl_hevc_slice_params *slices,
			     const struct v4l2_ctrl_hevc_decode_params *dec,
			     unsigned int slice_count, u32 ctb_count)
{
	const struct v4l2_ctrl_hevc_slice_params *slice;
	u64 unsupported_pps;
	int ret;

	unsupported_pps = V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED |
			  V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED |
			  V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT |
			  V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			  V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
	ret = mt6797_hevc_validate_frame_slices(slices, slice_count, ctb_count);
	if (ret)
		return ret;
	slice = &slices[0];
	if ((sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) ||
	    (pps->flags & unsupported_pps))
		return -EOPNOTSUPP;
	if (slice->nal_unit_type == 19 || slice->nal_unit_type == 20) {
		if (!(dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) ||
		    slice->slice_type != V4L2_HEVC_SLICE_TYPE_I ||
		    dec->num_active_dpb_entries)
			return -EINVAL;
	} else if (dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) {
		return -EINVAL;
	}

	return 0;
}

static int mt6797_hevc_init(struct mtk_vcodec_dec_ctx *ctx)
{
	struct mt6797_hevc_inst *inst;
	int ret;

	inst = kzalloc_obj(*inst);
	if (!inst)
		return -ENOMEM;
	inst->ctx = ctx;
	ret = mt6797_hevc_get_regs(inst);
	if (ret) {
		kfree(inst);
		return ret;
	}
	ctx->drv_handle = inst;

	return 0;
}

static void mt6797_hevc_deinit(void *h_vdec)
{
	struct mt6797_hevc_inst *inst = h_vdec;

	mt6797_hevc_free_mem(inst, &inst->bitstream);
	mt6797_hevc_free_mv(inst);
	kfree(inst);
}

static int mt6797_hevc_decode(void *h_vdec, struct mtk_vcodec_mem *bs,
			      struct vdec_fb *unused, bool *res_chg)
{
	struct mt6797_hevc_inst *inst = h_vdec;
	const struct v4l2_ctrl_hevc_decode_params *dec;
	const struct v4l2_ctrl_hevc_ext_sps_st_rps *ext_sps_rps = NULL;
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	struct v4l2_ctrl_hevc_slice_params *slices, staged_slice;
	struct mt6797_hevc_buffer_layout layout;
	struct mt6797_hevc_barrel_plan barrel;
	struct mt6797_hevc_mv_slot_plan mv_slots;
	struct mt6797_hevc_output_plan output;
	struct mt6797_hevc_ref_buffer ref_buffers[V4L2_HEVC_DPB_ENTRIES_NUM_MAX] = {};
	struct mt6797_hevc_ref_plan refs;
	struct mt6797_hevc_sps_regs sps_regs;
	struct mt6797_hevc_pps_regs pps_regs;
	struct mt6797_hevc_rps_regs rps_regs;
	struct mt6797_hevc_slice_regs slice_regs;
	struct mt6797_hevc_tile_regs tiles;
	struct mtk_video_dec_buf *dst_buf, *src_buf;
	struct mtk_vcodec_mem staged_bs;
	struct vdec_fb *fb;
	const u32 ext_rps_id = V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS;
	unsigned int slice_count;
	int ret;

	*res_chg = false;
	if (!bs)
		return 0;

	sps = mt6797_hevc_get_ctrl(inst, V4L2_CID_STATELESS_HEVC_SPS);
	if (IS_ERR(sps))
		return PTR_ERR(sps);
	pps = mt6797_hevc_get_ctrl(inst, V4L2_CID_STATELESS_HEVC_PPS);
	if (IS_ERR(pps))
		return PTR_ERR(pps);
	dec = mt6797_hevc_get_ctrl(inst, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	if (IS_ERR(dec))
		return PTR_ERR(dec);
	slices = mt6797_hevc_get_slices(inst, &slice_count);
	if (IS_ERR(slices))
		return PTR_ERR(slices);
	ret = mt6797_hevc_sps_regs(sps, pps, &sps_regs);
	if (ret)
		return ret;
	ret = mt6797_hevc_validate_request(sps, pps, slices, dec, slice_count,
					   sps_regs.ctb_width *
						   sps_regs.ctb_height);
	if (ret)
		return ret;
	if (sps->num_short_term_ref_pic_sets) {
		ext_sps_rps = mt6797_hevc_get_ctrl(inst, ext_rps_id);
		if (IS_ERR(ext_sps_rps))
			return PTR_ERR(ext_sps_rps);
		ret = mt6797_hevc_sps_rps_sets(ext_sps_rps,
					       sps->num_short_term_ref_pic_sets,
					       inst->sps_rps);
		if (ret)
			return ret;
	}
	ret = mt6797_hevc_slice_rps_regs(bs->va, bs->size, sps, pps, slices,
						dec,
						sps->num_short_term_ref_pic_sets ?
						inst->sps_rps : NULL,
						&rps_regs);
	if (ret)
		return ret;

	ret = mt6797_hevc_pps_regs(sps, pps, &pps_regs);
	if (ret)
		return ret;
	ret = mt6797_hevc_slice_regs(pps, slices,
				     sps_regs.ctb_width * sps_regs.ctb_height,
				     &slice_regs);
	if (ret)
		return ret;
	ret = mt6797_hevc_tile_regs(pps, sps_regs.ctb_width,
				    sps_regs.ctb_height, &tiles);
	if (ret)
		return ret;
	ret = mt6797_hevc_buffer_layout(sps->pic_width_in_luma_samples,
					sps->pic_height_in_luma_samples,
					&layout);
	if (ret)
		return ret;
	ret = mt6797_hevc_stage_slice(inst, bs, slices->data_byte_offset,
				      &staged_bs);
	if (ret)
		return ret;
	staged_slice = *slices;
	staged_slice.data_byte_offset = 0;
	ret = mt6797_hevc_barrel_plan(staged_bs.dma_addr, staged_bs.size,
				      &staged_slice, &barrel);
	if (ret)
		return ret;

	fb = inst->ctx->dev->vdec_pdata->get_cap_buffer(inst->ctx);
	if (!fb)
		return -EBUSY;
	if (fb->base_y.size < layout.luma_size ||
	    fb->base_c.size < layout.chroma_size)
		return -ENOSPC;
	ret = mt6797_hevc_output_plan(fb->base_y.dma_addr, fb->base_c.dma_addr,
				      &layout, &output);
	if (ret)
		return ret;
	ret = mt6797_hevc_alloc_mv(inst, layout.mv_size);
	if (ret)
		return ret;
	ret = mt6797_hevc_mv_slot_plan(dec, inst->mv_slots, &mv_slots);
	if (ret)
		return ret;
	ret = mt6797_hevc_fill_ref_buffers(inst, dec, &mv_slots, &layout, ref_buffers);
	if (ret)
		return ret;
	ret = mt6797_hevc_ref_plan(slices, dec, ref_buffers, &refs);
	if (ret)
		return ret;
	if (inst->mv[mv_slots.current_slot].dma_addr &
		    (MT6797_HEVC_MV_ALIGN - 1) ||
	    inst->mv[mv_slots.current_slot].dma_addr > U32_MAX)
		return -ERANGE;

	src_buf = container_of(bs, struct mtk_video_dec_buf, bs_buffer);
	dst_buf = container_of(fb, struct mtk_video_dec_buf, frame_buffer);
	v4l2_m2m_buf_copy_metadata(&src_buf->m2m_buf.vb, &dst_buf->m2m_buf.vb);
	ret = mt6797_hevc_reset(inst);
	if (ret)
		return ret;
	ret = mt6797_hevc_init_barrel(inst, &barrel);
	if (ret)
		return ret;
	mt6797_hevc_program_refs(inst, &refs);
	mt6797_hevc_program_output(inst, sps, &output,
				   inst->mv[mv_slots.current_slot].dma_addr);
	mt6797_hevc_program_syntax(inst, &sps_regs, &pps_regs, &rps_regs,
				   &slice_regs, &tiles);
	ret = mt6797_hevc_start(inst, sps_regs.ctb_width, sps_regs.ctb_height);
	if (ret)
		return ret;

	return mt6797_hevc_mv_slot_commit(inst->mv_slots,
					 mv_slots.current_slot,
					 dst_buf->m2m_buf.vb.vb2_buf.timestamp);
}

static int mt6797_hevc_get_param(void *h_vdec, enum vdec_get_param_type type,
				 void *out)
{
	struct mt6797_hevc_inst *inst = h_vdec;
	struct mtk_vcodec_dec_ctx *ctx = inst->ctx;
	struct mt6797_hevc_buffer_layout layout;
	struct vdec_pic_info *pic;
	struct v4l2_rect *crop;
	int ret;

	switch (type) {
	case GET_PARAM_PIC_INFO:
		pic = out;
		pic->pic_w = ctx->picinfo.pic_w;
		pic->pic_h = ctx->picinfo.pic_h;
		ret = mt6797_hevc_buffer_layout(pic->pic_w, pic->pic_h,
						&layout);
		if (ret)
			return ret;
		pic->buf_w = layout.width;
		pic->buf_h = layout.height;
		pic->fb_sz[0] = layout.luma_size;
		pic->fb_sz[1] = layout.chroma_size;
		pic->cap_fourcc = V4L2_PIX_FMT_MS21;
		break;
	case GET_PARAM_DPB_SIZE:
		*(unsigned int *)out = V4L2_HEVC_DPB_ENTRIES_NUM_MAX;
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

const struct vdec_common_if mtk_vdec_6797_hevc_slice_if = {
	.init = mt6797_hevc_init,
	.decode = mt6797_hevc_decode,
	.get_param = mt6797_hevc_get_param,
	.deinit = mt6797_hevc_deinit,
};
