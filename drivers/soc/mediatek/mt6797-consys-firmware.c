// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 CONSYS firmware and patch metadata validation.
 *
 * The vendor WMT path asks its userspace patch-search owner for a bare
 * firmware name and four address bytes, then loads the named firmware through
 * request_firmware().  It removes a 28-byte WMT_PATCH header before it sends
 * any body fragments.  See vendor drivers/misc/mediatek/connectivity/common/
 * common_main/core/wmt_ctrl.c:570-592 and wmt_ic_soc.c:2072-2200.
 *
 * This file validates loader results before the WMT owner consumes them. It is
 * not a firmware downloader or WMT implementation.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-consys-firmware.h>

#define MT6797_CONSYS_FW_NAME_MAX	255
static bool mt6797_consys_fw_name_valid(const char *name)
{
	size_t length;

	if (!name)
		return false;

	length = strnlen(name, MT6797_CONSYS_FW_NAME_MAX + 1);
	return length && length <= MT6797_CONSYS_FW_NAME_MAX &&
		!strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static int
mt6797_consys_firmware_blob_validate(const struct mt6797_consys_firmware_blob *blob)
{
	if (!blob || !mt6797_consys_fw_name_valid(blob->name) || !blob->data ||
	    !blob->size)
		return -EINVAL;

	return 0;
}

int mt6797_consys_patch_validate(const struct mt6797_consys_patch_image *patch,
				 struct mt6797_consys_patch_info *info)
{
	const u8 *header;
	int ret;

	if (!patch)
		return -EINVAL;

	ret = mt6797_consys_firmware_blob_validate(&patch->blob);
	if (ret)
		return ret;
	if (!patch->load_address)
		return -ENODATA;
	if (patch->blob.size <= MT6797_CONSYS_PATCH_HEADER_SIZE)
		return -EMSGSIZE;

	if (!info)
		return 0;

	header = patch->blob.data;
	memcpy(info->build_time, header, sizeof(info->build_time));
	memcpy(info->platform, header + sizeof(info->build_time),
	       sizeof(info->platform));
	info->hw_version = get_unaligned_le16(header + 20);
	info->sw_version = get_unaligned_le16(header + 22);
	info->patch_version = get_unaligned_le32(header + 24);
	memcpy(info->load_address, patch->load_address,
	       sizeof(info->load_address));
	info->body_size = patch->blob.size - MT6797_CONSYS_PATCH_HEADER_SIZE;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_patch_validate);

int
mt6797_consys_firmware_manifest_validate(
	const struct mt6797_consys_firmware_manifest *manifest)
{
	size_t i;
	size_t j;
	int ret;

	if (!manifest || !manifest->patches || !manifest->patch_count)
		return -EINVAL;

	for (i = 0; i < manifest->patch_count; i++) {
		ret = mt6797_consys_patch_validate(&manifest->patches[i], NULL);
		if (ret)
			return ret;

		/* Duplicate names would make a discovered download sequence ambiguous. */
		for (j = 0; j < i; j++)
			if (!strcmp(manifest->patches[i].blob.name,
				    manifest->patches[j].blob.name))
				return -EEXIST;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_firmware_manifest_validate);

MODULE_DESCRIPTION("MT6797 CONSYS firmware metadata validator");
MODULE_LICENSE("GPL");
