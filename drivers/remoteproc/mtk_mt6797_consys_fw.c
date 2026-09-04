// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 CONSYS firmware-class handoff.
 *
 * The vendor WMT firmware is not an ELF image and its load address is not an AP
 * physical address. The production launcher reads bytes 24..27 from each
 * 28-byte WMT header, obtains patch count/sequence from byte 24, clears that
 * byte, and passes the resulting four opaque bytes to the kernel patch
 * protocol. Keep that discovery contract here; a future remoteproc owner can
 * own the lifetime, while a separate WMT transport must own transmission.
 */

#include <crypto/sha2.h>
#include <crypto/utils.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mtk_mt6797_consys_fw.h"

struct mt6797_consys_fw_spec {
	const char *name;
	size_t size;
	u8 sha256[MT6797_CONSYS_FW_SHA256_SIZE];
};

static const struct mt6797_consys_fw_spec mt6797_consys_config = {
	.name = "WMT_SOC.cfg",
	.size = 80,
	.sha256 = {
		0xf4, 0xa5, 0x9b, 0x62, 0x2a, 0x4e, 0x0c, 0x14,
		0x70, 0xe4, 0x75, 0xce, 0x33, 0xf3, 0xed, 0xae,
		0x43, 0xb2, 0x7f, 0x1f, 0xbd, 0xeb, 0xa5, 0x4d,
		0xc7, 0xcf, 0x07, 0x50, 0x3d, 0x13, 0x28, 0x80,
	},
};

static const struct mt6797_consys_fw_spec mt6797_consys_patches[] = {
	{
		.name = "ROMv3_patch_1_1_hdr.bin",
		.size = 46472,
		.sha256 = {
			0x57, 0x32, 0xc0, 0x73, 0x03, 0x80, 0xe9, 0x37,
			0xb4, 0x8a, 0xd1, 0x69, 0xf2, 0x80, 0x5b, 0x65,
			0xe8, 0xd4, 0xa1, 0x78, 0x26, 0x55, 0x66, 0xc5,
			0x08, 0x3c, 0xb2, 0xcc, 0x2d, 0x24, 0x9f, 0x1e,
		},
	}, {
		.name = "ROMv3_patch_1_0_hdr.bin",
		.size = 210904,
		.sha256 = {
			0x45, 0x0c, 0x2b, 0x09, 0x49, 0xcf, 0x87, 0x92,
			0x17, 0xac, 0x9a, 0xef, 0x81, 0xb1, 0x8b, 0x86,
			0x09, 0x82, 0xf0, 0xe6, 0x93, 0x40, 0x78, 0x4b,
			0x44, 0x8b, 0x54, 0x36, 0x5d, 0x8c, 0xf6, 0x30,
		},
	},
};

int mt6797_consys_fw_verify(const u8 *data, size_t size,
			    size_t expected_size,
			    const u8 expected_sha256[MT6797_CONSYS_FW_SHA256_SIZE])
{
	u8 actual_sha256[MT6797_CONSYS_FW_SHA256_SIZE];

	if (!data || !expected_sha256 || !expected_size)
		return -EINVAL;
	if (size != expected_size)
		return -EMSGSIZE;

	sha256(data, size, actual_sha256);
	if (crypto_memneq(actual_sha256, expected_sha256,
			  sizeof(actual_sha256)))
		return -EKEYREJECTED;

	return 0;
}

int mt6797_consys_wmt_parse(const u8 *data, size_t size, u8 expected_sequence,
			    struct mt6797_consys_wmt_metadata *metadata)
{
	u8 discovery;

	if (!data || !metadata || !expected_sequence ||
	    expected_sequence > MT6797_CONSYS_WMT_PATCH_COUNT)
		return -EINVAL;
	if (size <= MT6797_CONSYS_WMT_HEADER_SIZE)
		return -EMSGSIZE;

	discovery = data[24];
	if (discovery >> 4 != MT6797_CONSYS_WMT_PATCH_COUNT ||
	    (discovery & 0x0f) != expected_sequence)
		return -EPROTO;

	memcpy(metadata->build_time, data, sizeof(metadata->build_time));
	memcpy(metadata->platform, data + 16, sizeof(metadata->platform));
	metadata->hw_version = get_unaligned_le16(data + 20);
	metadata->sw_version = get_unaligned_le16(data + 22);
	metadata->patch_count = discovery >> 4;
	metadata->sequence = discovery & 0x0f;
	metadata->address[0] = 0;
	memcpy(metadata->address + 1, data + 25,
	       sizeof(metadata->address) - 1);
	metadata->payload = data + MT6797_CONSYS_WMT_HEADER_SIZE;
	metadata->payload_size = size - MT6797_CONSYS_WMT_HEADER_SIZE;

	return 0;
}

static int mt6797_consys_fw_request_one(struct device *dev,
					const struct mt6797_consys_fw_spec *spec,
					const struct firmware **firmware)
{
	int ret;

	ret = request_firmware_direct(firmware, spec->name, dev);
	if (ret)
		return ret;

	ret = mt6797_consys_fw_verify((*firmware)->data, (*firmware)->size,
				      spec->size, spec->sha256);
	if (ret) {
		release_firmware(*firmware);
		*firmware = NULL;
	}

	return ret;
}

int mt6797_consys_fw_request(struct device *dev,
			     struct mt6797_consys_fw_bundle **result)
{
	struct mt6797_consys_fw_bundle *bundle;
	unsigned int i;
	int ret;

	if (!dev || !result)
		return -EINVAL;

	*result = NULL;
	bundle = kzalloc_obj(*bundle, GFP_KERNEL);
	if (!bundle)
		return -ENOMEM;

	ret = mt6797_consys_fw_request_one(dev, &mt6797_consys_config,
					   &bundle->config);
	if (ret)
		goto err_release;

	for (i = 0; i < ARRAY_SIZE(mt6797_consys_patches); i++) {
		const struct firmware *firmware;
		struct mt6797_consys_wmt_metadata *metadata;

		ret = mt6797_consys_fw_request_one(dev, &mt6797_consys_patches[i],
						   &bundle->patch[i].firmware);
		if (ret)
			goto err_release;

		firmware = bundle->patch[i].firmware;
		metadata = &bundle->patch[i].metadata;
		ret = mt6797_consys_wmt_parse(firmware->data, firmware->size,
					      i + 1, metadata);
		if (ret)
			goto err_release;
	}

	*result = bundle;
	return 0;

err_release:
	mt6797_consys_fw_release(bundle);
	return ret;
}

void mt6797_consys_fw_release(struct mt6797_consys_fw_bundle *bundle)
{
	unsigned int i;

	if (!bundle)
		return;

	for (i = 0; i < ARRAY_SIZE(bundle->patch); i++) {
		release_firmware(bundle->patch[i].firmware);
		bundle->patch[i].firmware = NULL;
		memset(&bundle->patch[i].metadata, 0,
		       sizeof(bundle->patch[i].metadata));
	}
	release_firmware(bundle->config);
	kfree(bundle);
}

MODULE_DESCRIPTION("MT6797 CONSYS firmware handoff");
MODULE_LICENSE("GPL");
