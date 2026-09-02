/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 CONSYS firmware and patch metadata validation.
 *
 * This is deliberately data-only. It neither requests firmware nor accesses
 * BTIF/CONSYS hardware. The runtime owner supplies firmware-class results and
 * the opaque four-byte patch addresses used by the WMT patch-download owner.
 */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FIRMWARE_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FIRMWARE_H

#include <linux/types.h>

#define MT6797_CONSYS_PATCH_ADDRESS_SIZE	4
#define MT6797_CONSYS_PATCH_HEADER_SIZE	28

struct mt6797_consys_firmware_blob {
	/* Must be the bare name passed to request_firmware(), never a path. */
	const char *name;
	const u8 *data;
	size_t size;
};

struct mt6797_consys_patch_image {
	struct mt6797_consys_firmware_blob blob;
	/* Opaque four-byte address returned by patch discovery; never inferred. */
	const u8 *load_address;
};

struct mt6797_consys_patch_info {
	u8 build_time[16];
	u8 platform[4];
	u16 hw_version;
	u16 sw_version;
	u32 patch_version;
	u8 load_address[MT6797_CONSYS_PATCH_ADDRESS_SIZE];
	size_t body_size;
};

struct mt6797_consys_firmware_manifest {
	const struct mt6797_consys_patch_image *patches;
	size_t patch_count;
};

/* Verify and bound one patch; copy parsed, non-executable metadata if asked. */
int mt6797_consys_patch_validate(const struct mt6797_consys_patch_image *patch,
				 struct mt6797_consys_patch_info *info);

/* Validate the config and every ordered patch image without hardware access. */
int
mt6797_consys_firmware_manifest_validate(
	const struct mt6797_consys_firmware_manifest *manifest);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_FIRMWARE_H */
