// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 CONSYS firmware provenance and patch metadata preflight.
 *
 * The vendor WMT path asks its userspace patch-search owner for a bare
 * firmware name and four address bytes, then loads the named firmware through
 * request_firmware().  It removes a 28-byte WMT_PATCH header before it sends
 * any body fragments.  See vendor drivers/misc/mediatek/connectivity/common/
 * common_main/core/wmt_ctrl.c:570-592 and wmt_ic_soc.c:2072-2200.
 *
 * This file deliberately stops before every one of those side effects.  It is
 * a gate for a later owner, not a firmware downloader or WMT implementation.
 */

#include <crypto/sha2.h>
#include <crypto/utils.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-consys-firmware.h>

#define MT6797_CONSYS_FW_NAME_MAX	255
#define MT6797_CONSYS_CONFIG_NAME	"WMT_SOC.cfg"

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
mt6797_consys_firmware_blob_preflight(const struct mt6797_consys_firmware_blob *blob)
{
	u8 actual_sha256[MT6797_CONSYS_FW_SHA256_SIZE];

	if (!blob || !mt6797_consys_fw_name_valid(blob->name) || !blob->data ||
	    !blob->size || !blob->sha256)
		return -EINVAL;

	sha256(blob->data, blob->size, actual_sha256);
	if (crypto_memneq(actual_sha256, blob->sha256, sizeof(actual_sha256)))
		return -EKEYREJECTED;

	return 0;
}

int mt6797_consys_patch_preflight(const struct mt6797_consys_patch_image *patch,
				  struct mt6797_consys_patch_info *info)
{
	const u8 *header;
	int ret;

	if (!patch)
		return -EINVAL;

	ret = mt6797_consys_firmware_blob_preflight(&patch->blob);
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
EXPORT_SYMBOL_GPL(mt6797_consys_patch_preflight);

int mt6797_consys_firmware_manifest_preflight(
		const struct mt6797_consys_firmware_manifest *manifest)
{
	size_t i;
	size_t j;
	int ret;

	if (!manifest || !manifest->config || !manifest->patches ||
	    !manifest->patch_count)
		return -EINVAL;

	ret = mt6797_consys_firmware_blob_preflight(manifest->config);
	if (ret)
		return ret;
	if (strcmp(manifest->config->name, MT6797_CONSYS_CONFIG_NAME))
		return -EPROTO;

	for (i = 0; i < manifest->patch_count; i++) {
		ret = mt6797_consys_patch_preflight(&manifest->patches[i], NULL);
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
EXPORT_SYMBOL_GPL(mt6797_consys_firmware_manifest_preflight);

MODULE_DESCRIPTION("MT6797 CONSYS firmware provenance preflight gate");
MODULE_LICENSE("GPL");
