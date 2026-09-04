/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_CONSYS_FW_H
#define __MTK_MT6797_CONSYS_FW_H

#include <linux/types.h>

struct device;
struct firmware;

#define MT6797_CONSYS_WMT_PATCH_COUNT		2
#define MT6797_CONSYS_WMT_HEADER_SIZE		28
#define MT6797_CONSYS_WMT_ADDRESS_SIZE		4
#define MT6797_CONSYS_FW_SHA256_SIZE		32

struct mt6797_consys_wmt_metadata {
	u8 build_time[16];
	u8 platform[4];
	u16 hw_version;
	u16 sw_version;
	u8 patch_count;
	u8 sequence;
	u8 address[MT6797_CONSYS_WMT_ADDRESS_SIZE];
	const u8 *payload;
	size_t payload_size;
};

struct mt6797_consys_fw_patch {
	const struct firmware *firmware;
	struct mt6797_consys_wmt_metadata metadata;
};

struct mt6797_consys_fw_bundle {
	const struct firmware *config;
	struct mt6797_consys_fw_patch patch[MT6797_CONSYS_WMT_PATCH_COUNT];
};

int mt6797_consys_fw_verify(const u8 *data, size_t size,
			    size_t expected_size,
			    const u8 expected_sha256[MT6797_CONSYS_FW_SHA256_SIZE]);
int mt6797_consys_wmt_parse(const u8 *data, size_t size, u8 expected_sequence,
			    struct mt6797_consys_wmt_metadata *metadata);

/* Private handoff for the eventual remoteproc owner; never a client ABI. */
int mt6797_consys_fw_request(struct device *dev,
			     struct mt6797_consys_fw_bundle **bundle);
void mt6797_consys_fw_release(struct mt6797_consys_fw_bundle *bundle);

#endif
