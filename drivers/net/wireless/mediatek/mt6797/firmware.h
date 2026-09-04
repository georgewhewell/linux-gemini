/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_FIRMWARE_H
#define __MT6797_WIFI_FIRMWARE_H

#include <linux/types.h>

#define MT6797_WIFI_FIRMWARE_NAME	"WIFI_RAM_CODE_6797"
#define MT6797_WIFI_FIRMWARE_SIZE	411632U
#define MT6797_WIFI_SECTION_COUNT	4
#define MT6797_WIFI_SHA256_SIZE		32

struct device;
struct firmware;

struct mt6797_wifi_section {
	u32 offset;
	u32 length;
	u32 destination;
	u8 key_index;
	bool encrypted;
};

struct mt6797_wifi_firmware {
	const struct firmware *blob;
	struct mt6797_wifi_section section[MT6797_WIFI_SECTION_COUNT];
};

int mt6797_wifi_firmware_verify(const u8 *data, size_t size,
				size_t expected_size,
				const u8 expected_sha256[MT6797_WIFI_SHA256_SIZE]);
int mt6797_wifi_firmware_parse(const u8 *data, size_t size,
			       struct mt6797_wifi_section *sections,
			       size_t section_count);
int mt6797_wifi_firmware_request(struct device *dev,
				 struct mt6797_wifi_firmware *firmware);
void mt6797_wifi_firmware_release(struct mt6797_wifi_firmware *firmware);

#endif /* __MT6797_WIFI_FIRMWARE_H */
