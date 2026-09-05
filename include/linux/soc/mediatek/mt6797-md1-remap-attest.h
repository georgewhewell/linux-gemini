/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 LK remap precondition classifier.
 *
 * This is data-only. It classifies a separately acquired snapshot against the
 * already validated LK diagnostic plan; it has no MMIO, DT, SMC, MPU, remap
 * writer, clock, power, reset, WDT, partition, firmware, or boot-vector path.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_ATTEST_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_ATTEST_H

#include <linux/bits.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/types.h>

#define MT6797_MD1_REMAP_LOCK_OFFSET		0xf80
#define MT6797_MD1_REMAP_LOCK_MD1		BIT(16)

enum mt6797_md1_remap_attested_mode {
	MT6797_MD1_REMAP_ATTEST_NONE,
	MT6797_MD1_REMAP_ATTEST_PHYS32,
	MT6797_MD1_REMAP_ATTEST_KERNEL_EMI_40000000,
};

/* Values must come from a future independently reviewed read-only observer. */
struct mt6797_md1_remap_snapshot {
	u32 bank0_map0;
	u32 bank0_map1;
	u32 bank4_map0;
	u32 bank4_map1;
	u32 lock;
};

/* A match says only that public-LK-shaped bank4/lock evidence was observed. */
struct mt6797_md1_remap_attestation {
	struct mt6797_md1_remap_snapshot observed;
	enum mt6797_md1_remap_attested_mode mode;
	bool lk_lock_observed;
	/* MT6797 MD1 secure MPU has no source-backed readback contract. */
	bool secure_mpu_unobservable;
};

int mt6797_md1_remap_attest_plan_snapshot(
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_remap_snapshot *snapshot,
	struct mt6797_md1_remap_attestation *attestation);

/* Reject any plan that does not have the validated-LK plan shape. */
int mt6797_md1_remap_attest_plan_validate(
	const struct mt6797_md1_mpu_remap_plan *plan);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_REMAP_ATTEST_H */
