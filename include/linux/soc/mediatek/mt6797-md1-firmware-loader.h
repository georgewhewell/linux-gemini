/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_LOADER_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_LOADER_H

#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>

struct device;

struct mt6797_md1_firmware_names {
	const char *md;
	const char *dsp;
	const char *arm7;
};

typedef int (*mt6797_md1_firmware_cold_verify_fn)(void *context);

/*
 * Request, validate and copy the vendor split-image set while MD1 is proven
 * cold/reset-held.  The caller owns that lifecycle proof and the returned plan.
 */
int mt6797_md1_firmware_load(struct device *dev,
	const struct mt6797_md1_firmware_names *names,
	const struct mt6797_md1_mpu_remap_layout *layout,
	const struct mt6797_md1_mpu_remap_plan *lk_plan,
	mt6797_md1_firmware_cold_verify_fn verify_cold, void *verify_context,
	struct mt6797_md1_mpu_remap_plan *plan);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_LOADER_H */
