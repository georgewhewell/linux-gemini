// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek MT6797 Wi-Fi AHB SDIO-like PIO transport. */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/string.h>

#include "hif.h"

#define MT6797_WIFI_AHB_CMD_SETUP	0x0000
#define MT6797_WIFI_AHB_CMD52_DATA	0x0004
#define MT6797_WIFI_AHB_IRQ_GATE		0x0200
#define MT6797_WIFI_AHB_CMD53_DATA	0x1000

#define MT6797_WIFI_AHB_CMD_COUNT	GENMASK(8, 0)
#define MT6797_WIFI_AHB_CMD_ADDRESS	GENMASK(25, 9)
#define MT6797_WIFI_AHB_CMD_INCREMENT	BIT(26)
#define MT6797_WIFI_AHB_CMD_BLOCK	BIT(27)
#define MT6797_WIFI_AHB_CMD_FUNCTION	GENMASK(30, 28)
#define MT6797_WIFI_AHB_CMD_WRITE	BIT(31)

#define MT6797_WIFI_AHB_WIFI_FUNCTION	1

int mt6797_wifi_ahb_hif_init(struct mt6797_wifi_ahb_hif *hif,
			     void __iomem *base)
{
	if (!hif || !base)
		return -EINVAL;

	hif->base = base;
	mutex_init(&hif->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_hif_init);

int mt6797_wifi_ahb_pack_cmd52(bool write, u8 function, u32 address, u8 value,
			       u32 *command)
{
	if (!command || function > FIELD_MAX(MT6797_WIFI_AHB_CMD_FUNCTION) ||
	    address > FIELD_MAX(MT6797_WIFI_AHB_CMD_ADDRESS))
		return -EINVAL;

	*command = FIELD_PREP(MT6797_WIFI_AHB_CMD_FUNCTION, function) |
		FIELD_PREP(MT6797_WIFI_AHB_CMD_ADDRESS, address) | value;
	if (write)
		*command |= MT6797_WIFI_AHB_CMD_WRITE;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_pack_cmd52);

int mt6797_wifi_ahb_plan_cmd53(bool write, u32 address, size_t length,
			       u32 *command, size_t *transfer_length)
{
	size_t wire_length;
	u32 count;

	if (!command || !transfer_length || !length ||
	    length > MT6797_WIFI_AHB_MAX_TRANSFER_SIZE ||
	    address > FIELD_MAX(MT6797_WIFI_AHB_CMD_ADDRESS))
		return -EINVAL;

	wire_length = ALIGN(length, sizeof(u32));
	if (wire_length >= MT6797_WIFI_AHB_BLOCK_SIZE) {
		wire_length = ALIGN(wire_length, MT6797_WIFI_AHB_BLOCK_SIZE);
		count = wire_length / MT6797_WIFI_AHB_BLOCK_SIZE;
	} else {
		count = wire_length;
	}

	*command = FIELD_PREP(MT6797_WIFI_AHB_CMD_COUNT, count) |
		FIELD_PREP(MT6797_WIFI_AHB_CMD_ADDRESS, address) |
		FIELD_PREP(MT6797_WIFI_AHB_CMD_FUNCTION,
			   MT6797_WIFI_AHB_WIFI_FUNCTION);
	if (wire_length >= MT6797_WIFI_AHB_BLOCK_SIZE)
		*command |= MT6797_WIFI_AHB_CMD_BLOCK;
	if (write)
		*command |= MT6797_WIFI_AHB_CMD_WRITE;

	*transfer_length = wire_length;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_plan_cmd53);

static int mt6797_wifi_ahb_validate(const struct mt6797_wifi_ahb_hif *hif)
{
	return hif && hif->base ? 0 : -EINVAL;
}

int mt6797_wifi_ahb_cmd52_read(struct mt6797_wifi_ahb_hif *hif, u8 function,
			       u32 address, u8 *value)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_validate(hif);
	if (ret || !value)
		return -EINVAL;

	ret = mt6797_wifi_ahb_pack_cmd52(false, function, address, 0, &command);
	if (ret)
		return ret;

	mutex_lock(&hif->lock);
	writel(command, hif->base + MT6797_WIFI_AHB_CMD_SETUP);
	*value = readl(hif->base + MT6797_WIFI_AHB_CMD52_DATA);
	mutex_unlock(&hif->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_cmd52_read);

int mt6797_wifi_ahb_cmd52_write(struct mt6797_wifi_ahb_hif *hif, u8 function,
				u32 address, u8 value)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_validate(hif);
	if (ret)
		return ret;

	ret = mt6797_wifi_ahb_pack_cmd52(true, function, address, value, &command);
	if (ret)
		return ret;

	mutex_lock(&hif->lock);
	writel(command, hif->base + MT6797_WIFI_AHB_CMD_SETUP);
	writel(value, hif->base + MT6797_WIFI_AHB_CMD52_DATA);
	mutex_unlock(&hif->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_cmd52_write);

int mt6797_wifi_ahb_cmd53_read(struct mt6797_wifi_ahb_hif *hif, u32 address,
			       void *buffer, size_t length)
{
	size_t offset, transfer_length;
	u8 *destination = buffer;
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_validate(hif);
	if (ret || !buffer)
		return -EINVAL;

	ret = mt6797_wifi_ahb_plan_cmd53(false, address, length, &command,
					 &transfer_length);
	if (ret)
		return ret;

	mutex_lock(&hif->lock);
	writel(command, hif->base + MT6797_WIFI_AHB_CMD_SETUP);
	for (offset = 0; offset < transfer_length; offset += sizeof(u32)) {
		__le32 word = cpu_to_le32(readl(hif->base +
					       MT6797_WIFI_AHB_CMD53_DATA));
		size_t bytes = offset < length ?
			min_t(size_t, sizeof(word), length - offset) : 0;

		if (bytes)
			memcpy(destination + offset, &word, bytes);
	}
	mutex_unlock(&hif->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_cmd53_read);

int mt6797_wifi_ahb_cmd53_write(struct mt6797_wifi_ahb_hif *hif, u32 address,
				const void *buffer, size_t length)
{
	size_t offset, transfer_length;
	const u8 *source = buffer;
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_validate(hif);
	if (ret || !buffer)
		return -EINVAL;

	ret = mt6797_wifi_ahb_plan_cmd53(true, address, length, &command,
					 &transfer_length);
	if (ret)
		return ret;

	mutex_lock(&hif->lock);
	writel(command, hif->base + MT6797_WIFI_AHB_CMD_SETUP);
	for (offset = 0; offset < transfer_length; offset += sizeof(u32)) {
		__le32 word = 0;
		size_t bytes = offset < length ?
			min_t(size_t, sizeof(word), length - offset) : 0;

		if (bytes)
			memcpy(&word, source + offset, bytes);
		writel(le32_to_cpu(word), hif->base + MT6797_WIFI_AHB_CMD53_DATA);
	}
	mutex_unlock(&hif->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_cmd53_write);

void mt6797_wifi_ahb_irq_mask(struct mt6797_wifi_ahb_hif *hif)
{
	if (hif && hif->base)
		writel(1, hif->base + MT6797_WIFI_AHB_IRQ_GATE);
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_irq_mask);

void mt6797_wifi_ahb_irq_unmask(struct mt6797_wifi_ahb_hif *hif)
{
	if (hif && hif->base)
		writel(0, hif->base + MT6797_WIFI_AHB_IRQ_GATE);
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_irq_unmask);

MODULE_DESCRIPTION("MediaTek MT6797 Wi-Fi AHB transport library");
MODULE_LICENSE("GPL");
