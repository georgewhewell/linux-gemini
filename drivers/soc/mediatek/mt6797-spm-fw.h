/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 SPM PCM firmware images, as loaded by the MediaTek 3.18 kernel
 * (drivers/misc/mediatek/base/power/spm_v2/mt_spm.c, spm_load_pcm_firmware()).
 */
#ifndef __MT6797_SPM_FW_H
#define __MT6797_SPM_FW_H

#include <linux/types.h>

/* One PCM_FIRMWARE_SIZE (8 KiB) slot of the SPM firmware carveout per image. */
#define MT6797_SPM_PCM_MAX_WORDS	(0x2000 / 4)
#define MT6797_SPM_PCM_VECTORS		16
/* struct pcm_desc from .size to .vec15: 4 x u16/u8 fields + 16 vectors */
#define MT6797_SPM_PCM_DESC_SIZE	(8 + 4 * MT6797_SPM_PCM_VECTORS)
#define MT6797_SPM_PCM_VERSION_SIZE	128

/* EVENT_VEC(event, resume, imme, pc) of mt_spm_internal.h */
#define MT6797_SPM_PCM_VEC_PC(vec)	((vec) >> 16)

/* Index order of the vendor dyna_load_pcm_path[] table for MT6797. */
enum mt6797_spm_fw_id {
	MT6797_SPM_FW_SUSPEND,
	MT6797_SPM_FW_SUSPEND_BY_MP1,
	MT6797_SPM_FW_VCOREFS_LPM,
	MT6797_SPM_FW_VCOREFS_HPM,
	MT6797_SPM_FW_VCOREFS_ULTRA,
	MT6797_SPM_FW_SODI,
	MT6797_SPM_FW_SODI_BY_MP1,
	MT6797_SPM_FW_DEEPIDLE,
	MT6797_SPM_FW_DEEPIDLE_BY_MP1,
	MT6797_SPM_FW_COUNT,
};

enum mt6797_spm_scenario {
	MT6797_SPM_SCENARIO_SUSPEND,
	MT6797_SPM_SCENARIO_SODI,
	MT6797_SPM_SCENARIO_DEEPIDLE,
};

struct mt6797_spm_pcm {
	/* @words little-endian PCM instructions; not naturally aligned */
	const u8 *code;
	u16 words;
	/* PCM_IM_LEN is programmed as @size - 1 */
	u16 size;
	u8 sess;
	u8 replace;
	u16 addr_2nd;
	u32 vec[MT6797_SPM_PCM_VECTORS];
	char version[MT6797_SPM_PCM_VERSION_SIZE];
};

extern const char *const mt6797_spm_fw_name[MT6797_SPM_FW_COUNT];

int mt6797_spm_pcm_parse(const u8 *data, size_t len,
			 struct mt6797_spm_pcm *pcm);
int mt6797_spm_fw_select(enum mt6797_spm_scenario scenario,
			 unsigned int cpu);

#endif
