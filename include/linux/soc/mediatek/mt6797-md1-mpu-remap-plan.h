/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 RMPU/remap diagnostic plan.
 *
 * This is a pure calculation library.  It neither discovers nor maps a
 * resource, and it has no register-write API.  In particular, an LK report
 * that its MD1 image loaded successfully is evidence of a completed load,
 * not evidence that Linux owns (or may rewrite) the EMI MPU or remap state.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_MPU_REMAP_PLAN_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_MPU_REMAP_PLAN_H

#include <linux/soc/mediatek/mt6797-md1-firmware.h>
#include <linux/types.h>

#define MT6797_MD1_MPU_PLAN_HEADER_ENTRIES	5
#define MT6797_MD1_MPU_PLAN_ENTRIES		6
#define MT6797_MD1_MPU_SHARED_ENTRY		5

#define MT6797_MD1_REMAP_SLOT_SIZE		0x02000000ULL
#define MT6797_MD1_REMAP_WINDOW_SIZE		(8 * MT6797_MD1_REMAP_SLOT_SIZE)

/* These are offsets within the MT6797 infracfg AO resource, not addresses. */
#define MT6797_MD1_BANK0_MAP0_OFFSET		0x300
#define MT6797_MD1_BANK0_MAP1_OFFSET		0x304
#define MT6797_MD1_BANK4_MAP0_OFFSET		0x310
#define MT6797_MD1_BANK4_MAP1_OFFSET		0x314

/* The two source-selected vendor address transformations. */
enum mt6797_md1_remap_mode {
	MT6797_MD1_REMAP_PHYS32,
	MT6797_MD1_REMAP_KERNEL_EMI_40000000,
	MT6797_MD1_REMAP_MODE_COUNT,
};

/* A physical LK layout, independently validated against reserved memory. */
struct mt6797_md1_mpu_remap_layout {
	u64 md_base;
	u32 md_size;
	u64 ap_md_smem_base;
	u32 ap_md_smem_size;

	/*
	 * This is copied into diagnostics only.  It must never be interpreted as
	 * permission to alter protection or remap registers.
	 */
	bool lk_reported_load_complete;
};

struct mt6797_md1_mpu_plan_entry {
	u64 start;
	u64 end;
	u32 access_permission;
	u8 emi_mpu_region;
	u8 check_header_region;
};

struct mt6797_md1_remap_values {
	u32 bank0_map0;
	u32 bank0_map1;
	u32 bank4_map0;
	u32 bank4_map1;
};

struct mt6797_md1_mpu_remap_plan {
	struct mt6797_md1_firmware_info firmware;
	struct mt6797_md1_mpu_plan_entry mpu[MT6797_MD1_MPU_PLAN_ENTRIES];
	struct mt6797_md1_remap_values remap[MT6797_MD1_REMAP_MODE_COUNT];
	u64 md_bank4_base;
	u64 smem_offset_ap_to_md;
	bool lk_reported_load_complete;
};

int mt6797_md1_mpu_remap_plan_build(const void *header, size_t header_size,
	const struct mt6797_md1_firmware_contract *contract,
	const struct mt6797_md1_mpu_remap_layout *layout,
	struct mt6797_md1_mpu_remap_plan *plan);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_MPU_REMAP_PLAN_H */
