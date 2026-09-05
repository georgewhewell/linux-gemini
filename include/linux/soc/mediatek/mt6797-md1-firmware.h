/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 firmware hand-off validation.
 *
 * The value-only helpers validate either an extracted check-header or the
 * complete request_firmware() file shape used by the vendor fallback loader.
 * They do not acquire firmware, map modem memory, or start MD1.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_H

#include <linux/types.h>
#include <linux/stddef.h>

#define MT6797_MD1_CHECK_HEADER_MAGIC	"CHECK_HEADER"
#define MT6797_MD1_CHECK_HEADER_MAGIC_SIZE	12

/*
 * These wire sizes come from the MT6797 3.18 CCCI check-header definitions.
 * v3 and v4 deliberately have the same 284-byte wire shape; MT6797 MD1
 * requires v4 or newer before the vendor programs its RMPU information.
 */
#define MT6797_MD1_CHECK_HEADER_V34_SIZE	284
#define MT6797_MD1_CHECK_HEADER_V5_SIZE	344
#define MT6797_MD1_CHECK_HEADER_V6_SIZE	512

#define MT6797_MD1_CHECK_HEADER_VERSION_OFFSET	12
#define MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET	16
#define MT6797_MD1_CHECK_HEADER_IMAGE_TYPE_OFFSET	20
#define MT6797_MD1_CHECK_HEADER_PLATFORM_OFFSET	24
#define MT6797_MD1_CHECK_HEADER_PLATFORM_SIZE	16
#define MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET	168
#define MT6797_MD1_CHECK_HEADER_MEM_SIZE_OFFSET	172
#define MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET	176
#define MT6797_MD1_CHECK_HEADER_DSP_OFFSET		184
#define MT6797_MD1_CHECK_HEADER_DSP_SIZE_OFFSET	188
#define MT6797_MD1_CHECK_HEADER_REGION_COUNT_OFFSET	192
#define MT6797_MD1_CHECK_HEADER_REGION_OFFSET		196
#define MT6797_MD1_CHECK_HEADER_REGION_STRIDE		8
#define MT6797_MD1_CHECK_HEADER_REGION_MAX		8
#define MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET	260
/* v5's arm7_img_offset/size follow v4's four domain attributes. */
#define MT6797_MD1_CHECK_HEADER_ARM7_OFFSET		276
#define MT6797_MD1_CHECK_HEADER_ARM7_SIZE_OFFSET	280
/* v6's padding_blk[8] ends at 348; its two shared-memory sizes follow. */
#define MT6797_MD1_CHECK_HEADER_V6_AP_SMEM_OFFSET	348
#define MT6797_MD1_CHECK_HEADER_V6_MD_TO_MD_SMEM_OFFSET	352

#define MT6797_MD1_DSP_ON_EMI_DISABLED	0xcdcdcdaa
#define MT6797_MD1_MAX_IMAGE_TYPE		22

/* Optional 512-byte MediaTek partition-image wrapper accepted by the BSP. */
#define MT6797_MD1_IMAGE_WRAPPER_MAGIC		0x58881688U
#define MT6797_MD1_IMAGE_WRAPPER_EXT_MAGIC	0x58891689U
#define MT6797_MD1_IMAGE_WRAPPER_SIZE		512U
#define MT6797_MD1_IMAGE_WRAPPER_DSIZE_OFFSET	4U
#define MT6797_MD1_IMAGE_WRAPPER_EXT_MAGIC_OFFSET	44U
#define MT6797_MD1_IMAGE_WRAPPER_HEADER_SIZE_OFFSET	48U

/*
 * Values in this contract must come from one coherent, read-only boot-loader
 * hand-off and partition acquisition.  A zero value means that the contract
 * is incomplete and validation must fail.
 */
struct mt6797_md1_firmware_contract {
	u32 md_reserved_size;
	u32 ap_md_shared_reserved_size;
	u32 md_to_md_shared_reserved_size;
	u32 raw_image_size;
};

struct mt6797_md1_firmware_info {
	u32 header_size;
	u32 header_version;
	u32 product_version;
	u32 image_type;
	u32 md_memory_size;
	u32 raw_image_size;
	u32 dsp_offset;
	u32 dsp_size;
	u32 arm7_offset;
	u32 arm7_size;
	u32 ap_md_smem_size;
	u32 md_to_md_smem_size;
	u8 bind_sys_id;
	u8 region_count;
	/* A bounded copy of the fixed 16-byte wire identity, not a policy match. */
	char platform[MT6797_MD1_CHECK_HEADER_PLATFORM_SIZE + 1];
	bool dsp_on_emi;
	bool has_arm7;
	bool has_v6_shared_memory;
};

/* Validated offsets into one immutable request_firmware() byte array. */
struct mt6797_md1_firmware_image {
	struct mt6797_md1_firmware_info info;
	size_t payload_offset;
	size_t payload_size;
	size_t check_header_offset;
};

int mt6797_md1_firmware_validate(const void *header, size_t header_size,
				 const struct mt6797_md1_firmware_contract *contract,
				 struct mt6797_md1_firmware_info *info);

/*
 * Strip the optional vendor wrapper without interpreting the payload.  This is
 * shared by the MD, DSP and ARM7 loaders; malformed wrappers fail closed.
 */
int mt6797_md1_firmware_payload(const void *data, size_t size,
				 size_t *payload_offset, size_t *payload_size);

/* Locate the trailing CHECK_HEADER and bind md_img_size to the payload. */
int mt6797_md1_firmware_image_validate(const void *data, size_t size,
			 const struct mt6797_md1_firmware_contract *contract,
			 struct mt6797_md1_firmware_image *image);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_FIRMWARE_H */
