/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Validation of the MT6797 MD1 LK CCCI hand-off metadata.
 *
 * No function in this header maps physical memory or accesses a block device.
 * A caller must first establish that an acquired byte range belongs to the
 * CCCI reserved-memory resource identified by LK and Device Tree.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_LK_HANDOFF_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_LK_HANDOFF_H

#include <linux/stddef.h>
#include <linux/types.h>

#define MT6797_MD1_LK_INFO_V2_VERSION		2
#define MT6797_MD1_LK_INFO_MAX_SIZE		0x10000
#define MT6797_MD1_LK_INFO_MD_COUNT		4
/* sizeof(ccci_lk_info_t_v2): 44 bytes of fields plus native 64-bit padding. */
#define MT6797_MD1_LK_INFO_V2_SIZE		48

#define MT6797_MD1_LK_TAG_NAME_SIZE		64
#define MT6797_MD1_LK_TAG_V2_SIZE		76
#define MT6797_MD1_LK_TAG_MAX_COUNT \
	(MT6797_MD1_LK_INFO_MAX_SIZE / MT6797_MD1_LK_TAG_V2_SIZE)

#define MT6797_MD1_LK_MODEM_INFO_SIZE		24
#define MT6797_MD1_LK_SMEM_LAYOUT_SIZE	40

/* LK uses MD_SYS1 == 0 internally, while its firmware header binds SYS1 as 1. */
#define MT6797_MD1_LK_MD1_INDEX		0
#define MT6797_MD1_LK_MD1_LOAD_FLAG		(1U << MT6797_MD1_LK_MD1_INDEX)

struct mt6797_md1_mpu_remap_plan;
struct mt6797_md1_mpu_remap_layout;

struct mt6797_md1_lk_info {
	u64 table_phys;
	u32 table_size;
	s32 error;
	u32 version;
	u32 tag_count;
	u32 load_flags;
	s32 md_error[MT6797_MD1_LK_INFO_MD_COUNT];
};

struct mt6797_md1_lk_tag {
	u32 data_offset;
	u32 data_size;
	u32 next_tag_offset;
};

struct mt6797_md1_lk_modem_info {
	u64 base_phys;
	u32 size;
	u8 md_id;
	s8 load_error;
	u8 md_type;
	u8 version;
};

struct mt6797_md1_lk_smem_layout {
	u64 base_phys;
	u32 ap_md1_offset;
	u32 ap_md1_size;
	u32 ap_md3_offset;
	u32 ap_md3_size;
	u32 md1_md3_offset;
	u32 md1_md3_size;
	u32 total_size;
};

/*
 * The LK v2 load flag is evidence only that LK completed its MD1 image-load
 * step.  It is not a power, reset, clock, MPU, remap, CCIF, transport, or
 * boot-vector state.  In particular, NONE and LOADED_EXECUTION_UNKNOWN never
 * grant Linux authority to perform a cold start or to release MD1.
 */
enum mt6797_md1_lk_md1_load_evidence {
	MT6797_MD1_LK_MD1_LOAD_NONE,
	MT6797_MD1_LK_MD1_LOAD_FAILED,
	MT6797_MD1_LK_MD1_LOAD_EXECUTION_UNKNOWN,
};

/*
 * Published only by the separately-gated probe after it has validated the
 * fixed LK v2 table, MD1 record, check header, CCCI carve-outs and diagnostic
 * plan. It is still image-load evidence, never an MD execution/ownership bit.
 */
bool mt6797_md1_lk_handoff_image_loaded(void);

/*
 * Copy the plan only after the separately gated probe has fully validated LK
 * table/header/carve-out inputs. This is source-derived diagnostic data, not
 * a hardware readback or MPU/remap ownership transfer.
 */
int mt6797_md1_lk_handoff_plan_get(struct mt6797_md1_mpu_remap_plan *plan);

/*
 * Copy the exact MD1 and AP-shared-memory ranges that the read-only probe
 * already checked against LK's live CCCI mblocks. This remains diagnostic
 * source data, never a mapping, lease, DMA, or MMIO ownership transfer.
 */
int mt6797_md1_lk_handoff_layout_get(struct mt6797_md1_mpu_remap_layout *layout);

/*
 * LK exposes ccci,modem_info_v2 as native little-endian bytes, not standard
 * Device Tree cells. The 3.18 MT6797 consumer memcpy()s this exact wire
 * structure before mapping its table. This parser accepts only that v2 form.
 */
int mt6797_md1_lk_info_parse_v2(const void *data, size_t size,
				struct mt6797_md1_lk_info *info);

/*
 * Validate the linked v2 tag table and find one named item. The returned
 * offsets remain relative to @data; this routine never dereferences physical
 * addresses and never copies an item's payload.
 */
int mt6797_md1_lk_tag_find_v2(const void *data, size_t size, u32 tag_count,
			       const char *name, struct mt6797_md1_lk_tag *tag);

int mt6797_md1_lk_modem_info_find(const void *data, size_t size, u8 md_id,
					   struct mt6797_md1_lk_modem_info *info);
int mt6797_md1_lk_smem_layout_parse(const void *data, size_t size,
					    struct mt6797_md1_lk_smem_layout *layout);

/*
 * Classify just the MD1 load evidence contained in a parsed v2 hand-off and
 * its hdr_tbl_inf record.  Returns 0 only for a successful image load, but
 * even then sets EXECUTION_UNKNOWN.  Returns -ENODATA when LK did not load
 * MD1 and -EIO when LK or the MD1 record reports a load error.
 */
int mt6797_md1_lk_md1_load_evidence(
	const struct mt6797_md1_lk_info *lk_info,
	const struct mt6797_md1_lk_modem_info *md1,
	enum mt6797_md1_lk_md1_load_evidence *evidence);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_LK_HANDOFF_H */
