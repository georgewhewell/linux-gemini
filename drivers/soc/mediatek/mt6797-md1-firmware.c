// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fail-closed validation of the MT6797 MD1 CCCI check-header hand-off.
 *
 * The 3.18 MT6797 vendor loader obtains both a `md1_chk` blob and an `md1img`
 * raw length from LK, then parses the blob before setting EMI MPU state:
 *
 * drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:775-786
 * drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c:284-504
 * drivers/misc/mediatek/eccci/mt6797/ccci_platform.c:381-384
 *
 * There is deliberately no block-device reader here.  A partition's padded
 * size cannot identify the check-header location: LK's raw-image length is a
 * separate hand-off value.  Treating a scan result as boot authority would
 * make a malformed or stale partition indistinguishable from a valid image.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/soc/mediatek/mt6797-md1-firmware.h>
#include <linux/string.h>
#include <linux/unaligned.h>

static bool mt6797_md1_range_valid(u32 offset, u32 length, u32 limit)
{
	u32 end;

	return !check_add_overflow(offset, length, &end) && end <= limit;
}

static int mt6797_md1_header_size_for_version(u32 version)
{
	switch (version) {
	case 4:
		return MT6797_MD1_CHECK_HEADER_V34_SIZE;
	case 5:
		return MT6797_MD1_CHECK_HEADER_V5_SIZE;
	case 6:
		return MT6797_MD1_CHECK_HEADER_V6_SIZE;
	default:
		return -EOPNOTSUPP;
	}
}

static int mt6797_md1_validate_regions(const u8 *header, u32 count,
				       const struct mt6797_md1_firmware_contract *contract)
{
	u32 i;

	if (count > MT6797_MD1_CHECK_HEADER_REGION_MAX)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		u32 offset = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			i * MT6797_MD1_CHECK_HEADER_REGION_STRIDE);
		u32 size = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			i * MT6797_MD1_CHECK_HEADER_REGION_STRIDE + sizeof(u32));

		if (!mt6797_md1_range_valid(offset, size,
					    contract->md_reserved_size))
			return -ERANGE;
	}

	return 0;
}

int mt6797_md1_firmware_validate(const void *data, size_t header_size,
				 const struct mt6797_md1_firmware_contract *contract,
				 struct mt6797_md1_firmware_info *info)
{
	const u8 *header = data;
	u32 declared_size, version, product_version, image_type, memory_size, image_size;
	u32 dsp_offset, dsp_size, arm7_offset = 0, arm7_size = 0;
	u32 region_count, ap_md_smem_size = 0, md_to_md_smem_size = 0;
	int expected_size, ret;

	if (!header || !contract || !info || !contract->md_reserved_size ||
	    !contract->raw_image_size)
		return -EINVAL;
	if (header_size < MT6797_MD1_CHECK_HEADER_V34_SIZE ||
	    header_size > MT6797_MD1_CHECK_HEADER_V6_SIZE)
		return -EMSGSIZE;
	if (memcmp(header, MT6797_MD1_CHECK_HEADER_MAGIC,
		   MT6797_MD1_CHECK_HEADER_MAGIC_SIZE))
		return -EBADMSG;

	version = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_VERSION_OFFSET);
	expected_size = mt6797_md1_header_size_for_version(version);
	if (expected_size < 0)
		return expected_size;
	if (header_size != expected_size)
		return -EMSGSIZE;

	declared_size = get_unaligned_le32(header + header_size - sizeof(u32));
	if (declared_size != header_size)
		return -EBADMSG;
	if (header[MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET] != 1)
		return -EXDEV;

	product_version = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	if (product_version != 1 && product_version != 2)
		return -EINVAL;
	image_type = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_IMAGE_TYPE_OFFSET);
	if (!image_type || image_type > MT6797_MD1_MAX_IMAGE_TYPE)
		return -EINVAL;

	memory_size = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_MEM_SIZE_OFFSET);
	image_size = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET);
	if (!memory_size || memory_size > contract->md_reserved_size ||
	    !image_size || image_size != contract->raw_image_size ||
	    image_size > memory_size)
		return -ERANGE;

	dsp_offset = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_DSP_OFFSET);
	dsp_size = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_DSP_SIZE_OFFSET);
	/* The vendor loader's disabled marker overrides dsp_size; preserve that. */
	if (dsp_offset != MT6797_MD1_DSP_ON_EMI_DISABLED &&
	    !mt6797_md1_range_valid(dsp_offset, dsp_size,
				    contract->md_reserved_size))
		return -ERANGE;

	region_count = get_unaligned_le32(header +
		MT6797_MD1_CHECK_HEADER_REGION_COUNT_OFFSET);
	/*
	 * The vendor parser copies all eight v4 entries (not just region_num),
	 * then MT6797 MPU code consumes fixed region indices.  It does not prove
	 * that zero or overlapping entries are invalid, so only reject arithmetic
	 * escapes here; a future MPU owner needs a reviewed semantic policy.
	 */
	ret = mt6797_md1_validate_regions(header,
					  MT6797_MD1_CHECK_HEADER_REGION_MAX, contract);
	if (ret)
		return ret;

	if (version >= 5) {
		arm7_offset = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_ARM7_OFFSET);
		arm7_size = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_ARM7_SIZE_OFFSET);
		/* The vendor loader treats ARM7 as absent unless both values are nonzero. */
		if (!!arm7_offset != !!arm7_size)
			return -EINVAL;
		if (arm7_size && !mt6797_md1_range_valid(arm7_offset, arm7_size,
							 contract->md_reserved_size))
			return -ERANGE;
	}

	if (version == 6) {
		ap_md_smem_size = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_V6_AP_SMEM_OFFSET);
		md_to_md_smem_size = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_V6_MD_TO_MD_SMEM_OFFSET);
		if (!contract->ap_md_shared_reserved_size ||
		    !contract->md_to_md_shared_reserved_size ||
		    ap_md_smem_size > contract->ap_md_shared_reserved_size ||
		    md_to_md_smem_size > contract->md_to_md_shared_reserved_size)
			return -ERANGE;
	}

	memset(info, 0, sizeof(*info));
	info->header_size = header_size;
	info->header_version = version;
	info->product_version = product_version;
	info->image_type = image_type;
	info->md_memory_size = memory_size;
	info->raw_image_size = image_size;
	info->dsp_offset = dsp_offset;
	info->dsp_size = dsp_size;
	info->arm7_offset = arm7_offset;
	info->arm7_size = arm7_size;
	info->ap_md_smem_size = ap_md_smem_size;
	info->md_to_md_smem_size = md_to_md_smem_size;
	info->bind_sys_id = header[MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET];
	info->region_count = region_count;
	memcpy(info->platform, header + MT6797_MD1_CHECK_HEADER_PLATFORM_OFFSET,
	       MT6797_MD1_CHECK_HEADER_PLATFORM_SIZE);
	info->dsp_on_emi = dsp_offset != MT6797_MD1_DSP_ON_EMI_DISABLED;
	info->has_arm7 = arm7_size != 0;
	info->has_v6_shared_memory = version == 6;
	return 0;
}

int mt6797_md1_firmware_payload(const void *data, size_t size,
				 size_t *payload_offset, size_t *payload_size)
{
	const u8 *bytes = data;
	u32 data_size, header_size;

	if (!bytes || !payload_offset || !payload_size || !size)
		return -EINVAL;

	if (size >= MT6797_MD1_IMAGE_WRAPPER_SIZE &&
	    get_unaligned_le32(bytes) == MT6797_MD1_IMAGE_WRAPPER_MAGIC) {
		if (get_unaligned_le32(bytes +
			MT6797_MD1_IMAGE_WRAPPER_EXT_MAGIC_OFFSET) !=
			MT6797_MD1_IMAGE_WRAPPER_EXT_MAGIC)
			return -EBADMSG;
		header_size = get_unaligned_le32(bytes +
			MT6797_MD1_IMAGE_WRAPPER_HEADER_SIZE_OFFSET);
		data_size = get_unaligned_le32(bytes +
			MT6797_MD1_IMAGE_WRAPPER_DSIZE_OFFSET);
		if (header_size < MT6797_MD1_IMAGE_WRAPPER_SIZE ||
		    header_size > size || !data_size || data_size > size - header_size)
			return -EMSGSIZE;
		*payload_offset = header_size;
		*payload_size = data_size;
		return 0;
	}

	*payload_offset = 0;
	*payload_size = size;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_firmware_payload);

int mt6797_md1_firmware_image_validate(const void *data, size_t size,
			 const struct mt6797_md1_firmware_contract *contract,
			 struct mt6797_md1_firmware_image *image)
{
	struct mt6797_md1_firmware_contract file_contract;
	struct mt6797_md1_firmware_image result;
	const u8 *payload;
	size_t header_offset;
	u32 header_size, raw_size;
	int ret;

	if (!data || !contract || !image || !contract->md_reserved_size)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	ret = mt6797_md1_firmware_payload(data, size, &result.payload_offset,
					 &result.payload_size);
	if (ret)
		return ret;
	if (result.payload_size < MT6797_MD1_CHECK_HEADER_V34_SIZE)
		return -EMSGSIZE;
	payload = (const u8 *)data + result.payload_offset;
	header_size = get_unaligned_le32(payload + result.payload_size - sizeof(u32));
	if (header_size != MT6797_MD1_CHECK_HEADER_V34_SIZE &&
	    header_size != MT6797_MD1_CHECK_HEADER_V5_SIZE &&
	    header_size != MT6797_MD1_CHECK_HEADER_V6_SIZE)
		return -EBADMSG;
	header_offset = result.payload_size - header_size;
	if (memcmp(payload + header_offset, MT6797_MD1_CHECK_HEADER_MAGIC,
		   MT6797_MD1_CHECK_HEADER_MAGIC_SIZE))
		return -EBADMSG;
	raw_size = get_unaligned_le32(payload + header_offset +
		MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET);
	/* Vendor accepts padding between md_img_size and the trailing header. */
	if (!raw_size || raw_size > header_offset)
		return -ERANGE;

	file_contract = *contract;
	file_contract.raw_image_size = raw_size;
	ret = mt6797_md1_firmware_validate(payload + header_offset, header_size,
					   &file_contract, &result.info);
	if (ret)
		return ret;
	result.check_header_offset = result.payload_offset + header_offset;
	*image = result;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_firmware_image_validate);
