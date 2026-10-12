/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD3_LK_HANDOFF_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD3_LK_HANDOFF_H

#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>

#define MT6797_MD3_LK_INDEX 2
#define MT6797_MD3_CHECK_HEADER_SIZE 188

/* Public LK mpu_cfg_t: four relevant records copied before SMEM clearing. */
struct mt6797_md3_lk_mpu_entry {
	u32 start, end, region, permission, relate_region;
};

struct mt6797_md3_lk_contract {
	u64 md_base;
	u32 md_size;
	u64 smem_base;
	u32 smem_size;
};

/* Detached image-load metadata only. No execution, power, MPU/remap or
 * transport authority. In particular, this does not advertise runtime C2K.
 * v1 header mem_size/md_img_size fields are hints: the public loader uses
 * the LK reservation/raw size instead, and the retained MD3 image needs it.
 */
struct mt6797_md3_lk_snapshot {
	struct mt6797_md1_lk_modem_info modem;
	struct mt6797_md1_lk_smem_layout smem;
	struct mt6797_md1_lk_runtime_options options;
	u32 raw_image_size;
	u32 header_version;
	u32 image_type;
	u32 header_mem_size;
	u32 header_image_size;
	u32 effective_image_size;
	s32 c2k_lte_mode;
	bool c2k_lte_mode_present;
	int mpu_error;
	u32 mpu_record_count;
	u32 mpu_present_mask;
	/* Ordered AP3(8), intermodem(9), ROM(16), RW(17). */
	struct mt6797_md3_lk_mpu_entry mpu[4];
	u8 check_header[MT6797_MD3_CHECK_HEADER_SIZE];
};

/* Caller first proves table and contract are LK-reserved memory. All errors
 * leave output unchanged. No addresses are dereferenced by this parser.
 */
int mt6797_md3_lk_snapshot_parse(const struct mt6797_md1_lk_info *info,
	const void *table, size_t size,
	const struct mt6797_md3_lk_contract *contract,
	struct mt6797_md3_lk_snapshot *snapshot);

/* The existing LK acquisition owner preserves this before MD1 clears SMEM.
 * Missing/invalid MD3 is reported here and never prevents normal MD1 boot.
 * The caller receives an owned copy with no pointer into cleared LK memory.
 */
int mt6797_md3_lk_handoff_get(struct mt6797_md3_lk_snapshot *snapshot);

#endif
