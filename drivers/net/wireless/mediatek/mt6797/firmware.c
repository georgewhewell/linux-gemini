// SPDX-License-Identifier: GPL-2.0-only
/* Verified MT6797 GEN3 WLAN divided-image loader. */

#include <crypto/sha2.h>
#include <crypto/utils.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "firmware.h"

#define MT6797_WIFI_FIRMWARE_SIGNATURE	0x454b544d
#define MT6797_WIFI_HEADER_SIZE		24
#define MT6797_WIFI_SECTION_SIZE		16

static const u8 mt6797_wifi_firmware_sha256[MT6797_WIFI_SHA256_SIZE] = {
	0xa6, 0x93, 0x83, 0xd7, 0x4d, 0x82, 0x94, 0x30,
	0x48, 0x7c, 0x39, 0xee, 0xf6, 0xb5, 0xe2, 0x81,
	0xb2, 0x5f, 0x90, 0x15, 0x95, 0xc9, 0x03, 0xa6,
	0x32, 0xa1, 0x0a, 0xa8, 0x63, 0x14, 0x26, 0xdd,
};

/* Exact metadata from the preserved, SHA-256-pinned Gemini image. */
static const struct mt6797_wifi_section mt6797_wifi_expected_sections[] = {
	{
		.offset = 0x58,
		.length = 0x16d0,
		.destination = 0x020a0000,
		.encrypted = true,
	}, {
		.offset = 0x1728,
		.length = 0x2320,
		.destination = 0x000a2800,
		.encrypted = true,
	}, {
		.offset = 0x3a48,
		.length = 0x50e20,
		.destination = 0xf0000000,
	}, {
		.offset = 0x54868,
		.length = 0xff70,
		.destination = 0xf005c000,
	},
};

static_assert(ARRAY_SIZE(mt6797_wifi_expected_sections) ==
	      MT6797_WIFI_SECTION_COUNT);

int mt6797_wifi_firmware_verify(const u8 *data, size_t size,
				size_t expected_size,
				const u8 expected_sha256[MT6797_WIFI_SHA256_SIZE])
{
	u8 actual_sha256[MT6797_WIFI_SHA256_SIZE];

	if (!data || !expected_sha256 || !expected_size)
		return -EINVAL;
	if (size != expected_size)
		return -EMSGSIZE;

	sha256(data, size, actual_sha256);
	return crypto_memneq(actual_sha256, expected_sha256,
			    sizeof(actual_sha256)) ? -EKEYREJECTED : 0;
}

int mt6797_wifi_firmware_parse(const u8 *data, size_t size,
			       struct mt6797_wifi_section *sections,
			       size_t section_count)
{
	unsigned int i;

	if (!data || !sections || section_count != MT6797_WIFI_SECTION_COUNT)
		return -EINVAL;
	if (size != MT6797_WIFI_FIRMWARE_SIZE)
		return -EMSGSIZE;
	if (get_unaligned_le32(data) != MT6797_WIFI_FIRMWARE_SIGNATURE ||
	    get_unaligned_le32(data + 8) != MT6797_WIFI_SECTION_COUNT ||
	    get_unaligned_le16(data + 12) || get_unaligned_le16(data + 14) ||
	    get_unaligned_le32(data + 16) || get_unaligned_le32(data + 20) ||
	    get_unaligned_le32(data + 4) !=
		~crc32_le(~0, data + 8, size - 8))
		return -EBADMSG;

	for (i = 0; i < MT6797_WIFI_SECTION_COUNT; i++) {
		const struct mt6797_wifi_section *expected =
			&mt6797_wifi_expected_sections[i];
		const u8 *entry = data + MT6797_WIFI_HEADER_SIZE +
			i * MT6797_WIFI_SECTION_SIZE;
		u32 end;

		if (get_unaligned_le32(entry) != expected->offset ||
		    entry[4] != expected->key_index ||
		    entry[5] != expected->encrypted ||
		    get_unaligned_le16(entry + 6) ||
		    get_unaligned_le32(entry + 8) != expected->length ||
		    get_unaligned_le32(entry + 12) != expected->destination ||
		    check_add_overflow(expected->offset, expected->length, &end) ||
		    end > size)
			return -EBADMSG;
		sections[i] = *expected;
	}

	return 0;
}

int mt6797_wifi_firmware_request(struct device *dev,
				 struct mt6797_wifi_firmware *firmware)
{
	int ret;

	if (!dev || !firmware)
		return -EINVAL;
	memset(firmware, 0, sizeof(*firmware));
	ret = request_firmware_direct(&firmware->blob,
				      MT6797_WIFI_FIRMWARE_NAME, dev);
	if (ret)
		return ret;
	ret = mt6797_wifi_firmware_verify(firmware->blob->data,
					  firmware->blob->size,
					  MT6797_WIFI_FIRMWARE_SIZE,
					  mt6797_wifi_firmware_sha256);
	if (!ret)
		ret = mt6797_wifi_firmware_parse(firmware->blob->data,
						 firmware->blob->size,
						 firmware->section,
						 ARRAY_SIZE(firmware->section));
	if (ret)
		mt6797_wifi_firmware_release(firmware);
	return ret;
}

void mt6797_wifi_firmware_release(struct mt6797_wifi_firmware *firmware)
{
	if (!firmware)
		return;
	release_firmware(firmware->blob);
	memset(firmware, 0, sizeof(*firmware));
}

MODULE_DESCRIPTION("MediaTek MT6797 verified WLAN firmware loader");
MODULE_LICENSE("GPL");
