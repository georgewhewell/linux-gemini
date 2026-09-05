/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 read-only infracfg remap observer.
 *
 * The only hardware operation represented here is a caller-supplied 32-bit
 * read. There is intentionally no writer, RMW, lock, MPU, WDT, power, reset,
 * CCIF, clock, partition, firmware, transport, or boot-vector operation.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_OBSERVE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_OBSERVE_H

#include <linux/soc/mediatek/mt6797-md1-remap-attest.h>
#include <linux/types.h>

/*
 * Public LK and the MT6797 vendor CCCI source agree on these infracfg-AO
 * offsets. See ccci_lk_load_img_plat.c:72-90 and ccci_platform.c:183-187.
 */
#define MT6797_MD1_REMAP_BANK0_MAP0_OFFSET	0x300
#define MT6797_MD1_REMAP_BANK0_MAP1_OFFSET	0x304
#define MT6797_MD1_REMAP_BANK4_MAP0_OFFSET	0x310
#define MT6797_MD1_REMAP_BANK4_MAP1_OFFSET	0x314

typedef u32 (*mt6797_md1_remap_read_fn)(void *context, u32 offset);

/* Raw read evidence is retained even when the classifier rejects it. */
struct mt6797_md1_remap_observation {
	struct mt6797_md1_remap_snapshot snapshot;
	struct mt6797_md1_remap_attestation attestation;
	int classification;
	bool observed;
};

/*
 * Validate the LK plan before calling @read, then acquire exactly bank0,
 * bank4 and MD1 lock in the listed order. A valid but non-matching snapshot
 * returns zero and reports the classifier result in @observation; that result
 * is evidence of refusal, never an instruction to write a replacement map.
 */
int mt6797_md1_remap_observe(
	const struct mt6797_md1_mpu_remap_plan *plan,
	mt6797_md1_remap_read_fn read, void *context,
	struct mt6797_md1_remap_observation *observation);

/* Copy the one-shot platform observation; this grants no control authority. */
int mt6797_md1_remap_observation_get(
	struct mt6797_md1_remap_observation *observation);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_OBSERVE_H */
