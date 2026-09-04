/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FW_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FW_H

#include <linux/types.h>

#define MT6797_CONSYS_FW_PATCH_COUNT	2

struct device;
struct mt6797_consys_fw_bundle;

/*
 * Immutable view into one already size/hash/header-verified bundle image.
 * The view remains valid only until the canonical owner releases @bundle.
 */
struct mt6797_consys_fw_patch_view {
	u8 address[4];
	const u8 *payload;
	size_t payload_size;
};

int mt6797_consys_fw_request(struct device *dev,
			     struct mt6797_consys_fw_bundle **bundle);
void mt6797_consys_fw_release(struct mt6797_consys_fw_bundle *bundle);
int mt6797_consys_fw_patch_view(const struct mt6797_consys_fw_bundle *bundle,
				unsigned int patch_index,
				struct mt6797_consys_fw_patch_view *view);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FW_H */
