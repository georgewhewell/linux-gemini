/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6797_CONSYS_FW_H
#define __MTK_MT6797_CONSYS_FW_H

#include <linux/types.h>

#include <linux/remoteproc/mtk_mt6797_consys.h>
#include <linux/soc/mediatek/mtk-wmt.h>

struct device;
struct firmware;

#define MT6797_CONSYS_WMT_PATCH_COUNT		MT6797_CONSYS_FW_PATCH_COUNT
#define MT6797_CONSYS_WMT_HEADER_SIZE		28
#define MT6797_CONSYS_WMT_ADDRESS_SIZE		4
#define MT6797_CONSYS_FW_SHA256_SIZE		32
#define MT6797_CONSYS_WMT_FRAGMENT_SIZE		1000
#define MT6797_CONSYS_WMT_PATCH_PACKET_MAX	\
	(MTK_WMT_HEADER_SIZE + 1 + MT6797_CONSYS_WMT_FRAGMENT_SIZE)

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

int mt6797_consys_fw_build_patch_packet(const struct mt6797_consys_fw_bundle *bundle,
					unsigned int patch_index, size_t offset,
					u8 *packet, size_t packet_size,
					size_t *packet_len, size_t *consumed,
					bool *last);

#endif
