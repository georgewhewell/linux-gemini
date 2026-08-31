// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded MT6797 GEN3 WLAN firmware download/start proof.
 *
 * The vendor sequence is deliberately followed rather than approximated:
 * nic/nic_pwr_mgt.c:262-290 obtains driver ownership and disables global
 * interrupt delivery; common/wlan_lib.c:2577-2612 configures and sends every
 * divided section; :827-887 sends sections 0-1 over HIF and copies later
 * sections through the first 512 KiB of CONSYS EMI; and nic_pwr_mgt.c:348-389
 * starts firmware and waits for WCIR.READY. The vendor only enables IRQ after
 * it has installed a normal adapter/IST; it supplies no isolated post-enable
 * IRQ self-test event. W1 therefore clears the same status but leaves global
 * IRQ delivery off and reports READY only, rather than claiming an IRQ proof
 * from a stale TX_DONE level.
 *
 * This is not a WLAN driver. It has no netdev, MAC, DMA, cfg80211, retry, or
 * userspace API. It performs exactly one pinned image attempt inside the
 * bootstrap's synchronous full-WMT stage. The enclosing owner fences/resets
 * CONSYS afterwards even on success; any failure after a stateful write is
 * consequently terminal for that attempt.
 */

#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/bitops.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w0-phase.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>

#define MT6797_WIFI_W1_FIRMWARE_NAME		"WIFI_RAM_CODE_6797"
#define MT6797_WIFI_W1_FIRMWARE_SIZE		411632U
#define MT6797_WIFI_W1_EMI_SIZE		(512U * 1024U)

#define MT6797_WIFI_W1_MCR_WHLPCR		0x0004U
#define MT6797_WIFI_W1_MCR_WHISR		0x0010U
#define MT6797_WIFI_W1_MCR_WHIER		0x0014U
#define MT6797_WIFI_W1_MCR_WTDR1		0x0034U
#define MT6797_WIFI_W1_MCR_WRDR0		0x0050U
#define MT6797_WIFI_W1_MCR_WRPLR		0x0090U
#define MT6797_WIFI_W1_MCR_WTQCR0		0x0130U

#define MT6797_WIFI_W1_WHLPCR_DRIVER_OWN	BIT(8)
#define MT6797_WIFI_W1_WHLPCR_OWN_CLR		BIT(9)
#define MT6797_WIFI_W1_WHLPCR_INT_CLR		BIT(1)
#define MT6797_WIFI_W1_WHIER_DEFAULT		0xffffff47U

#define MT6797_WIFI_W1_PQ_CMD			0x8000U
#define MT6797_WIFI_W1_PQ_PDA			0xc000U
#define MT6797_WIFI_W1_CMD_PACKET_TYPE		0xa0U
#define MT6797_WIFI_W1_EVENT_PACKET_TYPE	0xe000U
#define MT6797_WIFI_W1_CMD_DOWNLOAD_CONFIG	1U
#define MT6797_WIFI_W1_CMD_WIFI_START		2U
#define MT6797_WIFI_W1_EVENT_CMD_RESULT	1U
#define MT6797_WIFI_W1_DOWNLOAD_ENCRYPT	BIT(0)
#define MT6797_WIFI_W1_DOWNLOAD_RESET		BIT(3)
#define MT6797_WIFI_W1_DOWNLOAD_ACK		BIT(31)

#define MT6797_WIFI_W1_HIF_HEADER_SIZE		8U
#define MT6797_WIFI_W1_IMAGE_PACKET_SIZE	2048U
#define MT6797_WIFI_W1_HIF_TRANSFER_MAX	2560U
#define MT6797_WIFI_W1_RESPONSE_MAX		32U
#define MT6797_WIFI_W1_OWN_POLLS		256U
#define MT6797_WIFI_W1_RESPONSE_POLLS		20000U
#define MT6797_WIFI_W1_READY_POLLS		100U
#define MT6797_WIFI_W1_IDLM_FRAME_COUNT	8U
#define MT6797_WIFI_W1_IDLM_TRANSFER_BYTES	18432U

static const u8 mt6797_wifi_w1_firmware_sha256[] = {
	0xa6, 0x93, 0x83, 0xd7, 0x4d, 0x82, 0x94, 0x30,
	0x48, 0x7c, 0x39, 0xee, 0xf6, 0xb5, 0xe2, 0x81,
	0xb2, 0x5f, 0x90, 0x15, 0x95, 0xc9, 0x03, 0xa6,
	0x32, 0xa1, 0x0a, 0xa8, 0x63, 0x14, 0x26, 0xdd,
};

/* Exact MTKE divided-header fields from the SHA-256-pinned image. */
static const struct mt6797_wifi_ahb_w1_section mt6797_wifi_w1_sections[] = {
	{
		.offset = 0x58, .length = 0x16d0, .destination = 0x020a0000,
		.key_index = 0, .encrypted = true,
	}, {
		.offset = 0x1728, .length = 0x2320, .destination = 0x000a2800,
		.key_index = 0, .encrypted = true,
	}, {
		.offset = 0x3a48, .length = 0x50e20, .destination = 0xf0000000,
		.key_index = 0, .encrypted = false,
	}, {
		.offset = 0x54868, .length = 0xff70, .destination = 0xf005c000,
		.key_index = 0, .encrypted = false,
	},
};

static DEFINE_MUTEX(mt6797_wifi_w1_portal_lock);
static bool enable_experimental_wifi_w1_firmware_preflight;
module_param_named(enable_experimental_wifi_w1_firmware_preflight,
		   enable_experimental_wifi_w1_firmware_preflight, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w1_firmware_preflight,
		 "allow one MT6797 WLAN firmware download/start/READY proof");

struct mt6797_wifi_w1_power {
	struct device *owner;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
	struct regulator *bt_paldo;
	struct regulator *wifi_paldo;
	const struct firmware *firmware;
	struct mt6797_wifi_ahb_w1_image image;
};

bool mt6797_wifi_ahb_w1_firmware_enabled(struct device *owner)
{
	return owner && READ_ONCE(enable_experimental_wifi_w1_firmware_preflight) &&
		device_property_read_bool(owner,
			"mediatek,experimental-wifi-w1-firmware-preflight");
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_enabled);

static int mt6797_wifi_w1_digest(const u8 *data, size_t size, u8 *digest)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	int ret;

	if (!data || !digest)
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		ret = -ENOMEM;
		goto out_free_tfm;
	}
	desc->tfm = tfm;
	ret = crypto_shash_digest(desc, data, size, digest);
	kfree(desc);
out_free_tfm:
	crypto_free_shash(tfm);
	return ret;
}

int mt6797_wifi_ahb_w1_firmware_validate(const u8 *data, size_t size,
					 size_t expected_size,
					 const u8 *expected_digest)
{
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	if (!data || !expected_digest || !expected_size)
		return -EINVAL;
	if (size != expected_size)
		return -EFBIG;
	ret = mt6797_wifi_w1_digest(data, size, digest);
	if (ret)
		return ret;
	return memcmp(digest, expected_digest, sizeof(digest)) ? -EBADMSG : 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_validate);

int mt6797_wifi_ahb_w1_firmware_parse(const u8 *data, size_t size,
				       struct mt6797_wifi_ahb_w1_image *image)
{
	unsigned int i;

	if (!data || !image || size != MT6797_WIFI_W1_FIRMWARE_SIZE ||
	    get_unaligned_le32(data) != 0x454b544d ||
	    get_unaligned_le32(data + 8) != ARRAY_SIZE(mt6797_wifi_w1_sections) ||
	    get_unaligned_le16(data + 12) || get_unaligned_le16(data + 14) ||
	    get_unaligned_le32(data + 16) || get_unaligned_le32(data + 20) ||
	    get_unaligned_le32(data + 4) != ~crc32_le(~0, data + 8, size - 8))
		return -EBADMSG;

	for (i = 0; i < ARRAY_SIZE(mt6797_wifi_w1_sections); i++) {
		const struct mt6797_wifi_ahb_w1_section *expected =
			&mt6797_wifi_w1_sections[i];
		const u8 *section = data + 24 + i * 16;
		u32 end;

		if (get_unaligned_le32(section) != expected->offset ||
		    section[4] != expected->key_index ||
		    section[5] != expected->encrypted ||
		    get_unaligned_le16(section + 6) ||
		    get_unaligned_le32(section + 8) != expected->length ||
		    get_unaligned_le32(section + 12) != expected->destination ||
		    check_add_overflow(expected->offset, expected->length, &end) ||
		    end > size)
			return -EBADMSG;
		image->section[i] = *expected;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_parse);

static int mt6797_wifi_w1_cmd52_read(void __iomem *portal, u32 address,
				     u8 *data)
{
	u32 command;
	int ret;

	if (!data)
		return -EINVAL;
	ret = mt6797_wifi_ahb_cmd52_pack(false, 0, address, 0, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	*data = readl(portal + MT6797_WIFI_AHB_CMD52_DATA);
	return 0;
}

static int mt6797_wifi_w1_cmd52_write(void __iomem *portal, u32 address,
				      u8 data)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_cmd52_pack(true, 0, address, data, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	writel(data, portal + MT6797_WIFI_AHB_CMD52_DATA);
	return 0;
}

static int mt6797_wifi_w1_cmd53_write_pack(u32 address, size_t transfer,
					    u32 *command)
{
	u32 blocks;

	if (!command || !transfer || transfer & 3 ||
	    address > MT6797_WIFI_AHB_CMD_ADDR_MAX)
		return -EINVAL;
	if (transfer < MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return mt6797_wifi_ahb_cmd53_write_pack(address, transfer, command);
	if (transfer % MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return -EINVAL;
	blocks = transfer / MT6797_WIFI_AHB_SDIO_BLOCK_SIZE;
	if (!blocks || blocks > MT6797_WIFI_AHB_CMD53_COUNT_MAX)
		return -EINVAL;
	/* Vendor sdio.h:76-87: fixed port, block mode, function 1, write. */
	*command = blocks | (address << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) |
		BIT(27) | BIT(MT6797_WIFI_AHB_CMD_FUNC_SHIFT) | BIT(31);
	return 0;
}

static int mt6797_wifi_w1_cmd53_write(void __iomem *portal, u32 address,
				       const u8 *data, size_t transfer)
{
	u32 command;
	size_t i;
	int ret;

	if (!data || transfer > MT6797_WIFI_W1_HIF_TRANSFER_MAX)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd53_write_pack(address, transfer, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	/* The portal must observe CMD53 setup before the fixed-port payload. */
	wmb();
	for (i = 0; i < transfer; i += sizeof(u32))
		writel(get_unaligned_le32(data + i),
		       portal + MT6797_WIFI_AHB_CMD53_DATA);
	return 0;
}

static int mt6797_wifi_w1_cmd53_read(void __iomem *portal, u32 address,
				      u8 *data, size_t length)
{
	u32 command;
	size_t i;
	int ret;

	if (!data || !length || length & 3 ||
	    length > MT6797_WIFI_AHB_CMD53_COUNT_MAX)
		return -EINVAL;
	ret = mt6797_wifi_ahb_cmd53_read_pack(address, length, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	for (i = 0; i < length; i += sizeof(u32))
		put_unaligned_le32(readl(portal + MT6797_WIFI_AHB_CMD53_DATA),
				   data + i);
	return 0;
}

static int mt6797_wifi_w1_reg_write(void __iomem *portal, u32 address, u32 value)
{
	u8 data[sizeof(value)];

	put_unaligned_le32(value, data);
	return mt6797_wifi_w1_cmd53_write(portal, address, data, sizeof(data));
}

static int mt6797_wifi_w1_reg_read(void __iomem *portal, u32 address, u32 *value)
{
	u8 data[sizeof(*value)];
	int ret;

	if (!value)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd53_read(portal, address, data, sizeof(data));
	if (!ret)
		*value = get_unaligned_le32(data);
	return ret;
}

static int mt6797_wifi_w1_enable_function(void __iomem *portal, u8 *ioex_before,
					   bool *enabled)
{
	u8 iorx;
	int ret;

	if (!ioex_before || !enabled)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_before);
	if (ret)
		return ret;
	if (*ioex_before & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI))
		return -EBUSY;
	ret = mt6797_wifi_w1_cmd52_write(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX,
		*ioex_before | BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI));
	if (ret)
		return ret;
	*enabled = true;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IORX, &iorx);
	if (ret)
		return ret;
	return iorx & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI) ? 0 : -ETIMEDOUT;
}

static int mt6797_wifi_w1_set_block_size(void __iomem *portal, u8 *low_before,
					  u8 *high_before, bool *changed)
{
	int ret;

	if (!low_before || !high_before || !changed)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
	if (ret)
		return ret;
	if (*low_before) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, 0);
		if (ret)
			return ret;
		*changed = true;
	}
	if (*high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1,
			MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8);
		if (ret)
			return ret;
		*changed = true;
	}
	return 0;
}

static int mt6797_wifi_w1_restore_block_size(void __iomem *portal,
					      u8 low_before, u8 high_before)
{
	u8 low_after, high_after;
	int ret;

	if (high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
		if (ret)
			return ret;
	}
	if (low_before) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, &low_after);
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, &high_after);
	if (ret)
		return ret;
	return low_after == low_before && high_after == high_before ? 0 : -EUCLEAN;
}

static int mt6797_wifi_w1_restore_function(void __iomem *portal,
					     u8 ioex_before)
{
	u8 ioex_after;
	int ret;

	ret = mt6797_wifi_w1_cmd52_write(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, &ioex_after);
	if (ret)
		return ret;
	return ioex_after == ioex_before ? 0 : -EUCLEAN;
}

static int mt6797_wifi_w1_driver_own(void __iomem *portal)
{
	u32 value;
	unsigned int i;
	int ret;

	/* Vendor hal.h:384-408: request driver own and poll WHLPCR bit 8. */
	ret = mt6797_wifi_w1_reg_write(portal, MT6797_WIFI_W1_MCR_WHLPCR,
		MT6797_WIFI_W1_WHLPCR_OWN_CLR);
	if (ret)
		return ret;
	for (i = 0; i < MT6797_WIFI_W1_OWN_POLLS; i++) {
		ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WHLPCR,
			&value);
		if (ret)
			return ret;
		if (value & MT6797_WIFI_W1_WHLPCR_DRIVER_OWN)
			return 0;
		udelay(8);
	}
	return -ETIMEDOUT;
}

static size_t mt6797_wifi_w1_tx_transfer_size(size_t wire_length)
{
	if (wire_length < MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return ALIGN(wire_length, sizeof(u32));
	return ALIGN(wire_length, MT6797_WIFI_AHB_SDIO_BLOCK_SIZE);
}

static void mt6797_wifi_w1_init_header(u8 *packet, u16 wire_length, u16 pq,
					u8 command, u8 sequence)
{
	put_unaligned_le16(wire_length, packet);
	put_unaligned_le16(pq, packet + 2);
	packet[4] = command;
	packet[5] = MT6797_WIFI_W1_CMD_PACKET_TYPE;
	packet[7] = sequence;
}

static int mt6797_wifi_w1_wait_config_ack(void __iomem *portal, u8 sequence)
{
	u8 event[MT6797_WIFI_W1_RESPONSE_MAX] = { };
	u32 response_length;
	unsigned int i;
	int ret;

	/* wlan_lib.c:2769-2808 reads WRPLR's RX0 length and then WRDR0. */
	for (i = 0; i < MT6797_WIFI_W1_RESPONSE_POLLS; i++) {
		ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WRPLR,
			&response_length);
		if (ret)
			return ret;
		response_length &= U16_MAX;
		if (response_length)
			break;
		udelay(50);
	}
	if (!response_length)
		return -ETIMEDOUT;
	/* 8-byte init header + 20-byte MT6797 INIT_EVENT_CMD_RESULT. */
	if (response_length != 28 || response_length + 4 > sizeof(event))
		return -EPROTO;
	ret = mt6797_wifi_w1_cmd53_read(portal, MT6797_WIFI_W1_MCR_WRDR0,
		event, ALIGN(response_length + 4, sizeof(u32)));
	if (ret)
		return ret;
	if (get_unaligned_le16(event) != response_length ||
	    get_unaligned_le16(event + 2) != MT6797_WIFI_W1_EVENT_PACKET_TYPE ||
	    event[4] != MT6797_WIFI_W1_EVENT_CMD_RESULT || event[5] != sequence ||
	    event[8])
		return -EPROTO;
	return 0;
}

static int mt6797_wifi_w1_send_config(void __iomem *portal,
				       const struct mt6797_wifi_ahb_w1_section *section,
				       bool reset, u8 sequence)
{
	u8 packet[20] = { };
	u32 mode = MT6797_WIFI_W1_DOWNLOAD_ACK;
	int ret;

	if (!section)
		return -EINVAL;
	/* wlan_lib.c:2400-2500, including MT6797 encryption/key-index bits. */
	if (section->encrypted)
		mode |= MT6797_WIFI_W1_DOWNLOAD_ENCRYPT |
			((section->key_index & 0x3) << 1);
	if (reset)
		mode |= MT6797_WIFI_W1_DOWNLOAD_RESET;
	mt6797_wifi_w1_init_header(packet, sizeof(packet), MT6797_WIFI_W1_PQ_CMD,
		MT6797_WIFI_W1_CMD_DOWNLOAD_CONFIG, sequence);
	put_unaligned_le32(section->destination, packet + 8);
	put_unaligned_le32(section->length, packet + 12);
	put_unaligned_le32(mode, packet + 16);
	ret = mt6797_wifi_w1_cmd53_write(portal, MT6797_WIFI_W1_MCR_WTDR1,
		packet, sizeof(packet));
	if (ret)
		return ret;
	return mt6797_wifi_w1_wait_config_ack(portal, sequence);
}

static int mt6797_wifi_w1_send_image_packet(void __iomem *portal,
					    u8 *packet, const u8 *image,
					    size_t image_length)
{
	size_t wire_length, transfer;

	if (!packet || !image || !image_length ||
	    image_length > MT6797_WIFI_W1_IMAGE_PACKET_SIZE)
		return -EINVAL;
	wire_length = MT6797_WIFI_W1_HIF_HEADER_SIZE + image_length;
	transfer = mt6797_wifi_w1_tx_transfer_size(wire_length);
	if (transfer > MT6797_WIFI_W1_HIF_TRANSFER_MAX)
		return -EOVERFLOW;
	/* wlan_lib.c:2512-2556; ahb.c:1091-1117 selects this block/byte mode. */
	memset(packet, 0, transfer);
	mt6797_wifi_w1_init_header(packet, wire_length, MT6797_WIFI_W1_PQ_PDA,
		0, 0);
	memcpy(packet + MT6797_WIFI_W1_HIF_HEADER_SIZE, image, image_length);
	return mt6797_wifi_w1_cmd53_write(portal, MT6797_WIFI_W1_MCR_WTDR1,
		packet, transfer);
}

int mt6797_wifi_ahb_w1_idlm_transfer_account(
		const struct mt6797_wifi_ahb_w1_image *image,
		u32 *frame_count, size_t *transfer_bytes)
{
	u32 frames = 0;
	size_t bytes = 0;
	unsigned int i;

	if (!image || !frame_count || !transfer_bytes)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		u32 sent = 0;

		if (!image->section[i].length)
			return -EINVAL;
		while (sent < image->section[i].length) {
			size_t chunk = min_t(u32, MT6797_WIFI_W1_IMAGE_PACKET_SIZE,
				image->section[i].length - sent);

			if (check_add_overflow(bytes,
				mt6797_wifi_w1_tx_transfer_size(
					MT6797_WIFI_W1_HIF_HEADER_SIZE + chunk), &bytes) ||
			    frames == U32_MAX)
				return -EOVERFLOW;
			frames++;
			sent += chunk;
		}
	}
	*frame_count = frames;
	*transfer_bytes = bytes;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_idlm_transfer_account);

static int mt6797_wifi_w1_send_idlm(void __iomem *portal, const u8 *image,
				    const struct mt6797_wifi_ahb_w1_section *section,
				    u8 *packet)
{
	u32 sent = 0;
	int ret;

	if (!image || !section)
		return -EINVAL;
	while (sent < section->length) {
		size_t chunk = min_t(u32, MT6797_WIFI_W1_IMAGE_PACKET_SIZE,
			section->length - sent);

		ret = mt6797_wifi_w1_send_image_packet(portal, packet,
			image + section->offset + sent, chunk);
		if (ret)
			return ret;
		sent += chunk;
	}
	return 0;
}

static int mt6797_wifi_w1_copy_emi(struct device *owner, const u8 *image,
				    const struct mt6797_wifi_ahb_w1_image *parsed)
{
	struct resource window;
	void __iomem *emi;
	unsigned int i;
	int close_ret;
	int ret;

	if (!owner || !image || !parsed)
		return -EINVAL;
	/* The persistent region-19 owner serializes this temporary region-18 grant. */
	ret = mt6797_consys_emi_mpu_wifi_window_open(owner, &window);
	if (ret)
		return ret;
	if (resource_size(&window) != MT6797_WIFI_W1_EMI_SIZE) {
		ret = -EINVAL;
		goto close_window;
	}
	emi = ioremap(window.start, resource_size(&window));
	if (!emi) {
		ret = -ENOMEM;
		goto close_window;
	}
	for (i = 2; i < ARRAY_SIZE(parsed->section); i++) {
		const struct mt6797_wifi_ahb_w1_section *section =
			&parsed->section[i];
		u32 offset = section->destination & GENMASK(19, 0);

		if (offset > MT6797_WIFI_W1_EMI_SIZE - section->length) {
			ret = -EFBIG;
			break;
		}
		memcpy_toio(emi + offset, image + section->offset, section->length);
	}
	/* Complete all mapped writes before region 18 is restored. */
	wmb();
	iounmap(emi);
close_window:
	close_ret = mt6797_consys_emi_mpu_wifi_window_close(owner);
	if (!ret)
		ret = close_ret;
	return ret;
}

static int mt6797_wifi_w1_send_start(void __iomem *portal, u8 sequence)
{
	u8 packet[16] = { };

	/* wlan_lib.c:2821-2887 sends enable=0, address=0 for the default entry. */
	mt6797_wifi_w1_init_header(packet, sizeof(packet), MT6797_WIFI_W1_PQ_CMD,
		MT6797_WIFI_W1_CMD_WIFI_START, sequence);
	return mt6797_wifi_w1_cmd53_write(portal, MT6797_WIFI_W1_MCR_WTDR1,
		packet, sizeof(packet));
}

static int mt6797_wifi_w1_wait_ready(void __iomem *portal)
{
	struct mt6797_wifi_ahb_wcir wcir;
	u32 raw;
	unsigned int i;
	int ret;

	for (i = 0; i < MT6797_WIFI_W1_READY_POLLS; i++) {
		ret = mt6797_wifi_w1_reg_read(portal,
			MT6797_WIFI_AHB_WCIR_OFFSET, &raw);
		if (ret)
			return ret;
		ret = mt6797_wifi_ahb_wcir_decode(raw, &wcir);
		if (ret)
			return ret;
		if (wcir.wlan_ready)
			return 0;
		usleep_range(10000, 11000);
	}
	return -ETIMEDOUT;
}

static int mt6797_wifi_w1_clear_post_ready_status(void __iomem *portal)
{
	u32 status;
	u32 released;
	unsigned int i;
	int ret;

	/* nic_pwr_mgt.c:379-386: clear WHISR and consume TX release counts. */
	ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WHISR, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0)))
		return 0;
	for (i = 0; i < 8; i++) {
		ret = mt6797_wifi_w1_reg_read(portal,
			MT6797_WIFI_W1_MCR_WTQCR0 + i * sizeof(u32), &released);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6797_wifi_w1_stream(struct mt6797_wifi_w1_power *power)
{
	struct device_node *wifi_np;
	struct resource hif;
	void __iomem *portal = NULL;
	u8 *image_packet;
	u8 ioex_before, block_low_before, block_high_before, sequence = 1;
	bool function_enabled = false, block_size_set = false;
	int cleanup_ret;
	int ret;
	unsigned int i;
	u32 frame_count;
	size_t transfer_bytes;

	if (!power || !power->owner || !power->lease || !power->firmware ||
	    !mt6797_btif_stp_bootstrap_full_wmt_lease_active(power->lease))
		return -EPERM;
	ret = mt6797_wifi_ahb_w1_idlm_transfer_account(&power->image,
		&frame_count, &transfer_bytes);
	if (ret)
		return ret;
	if (frame_count != MT6797_WIFI_W1_IDLM_FRAME_COUNT ||
	    transfer_bytes != MT6797_WIFI_W1_IDLM_TRANSFER_BYTES)
		return -EPROTO;
	/* Allocate before the first portal mutation; it is reused for every frame. */
	image_packet = kmalloc(MT6797_WIFI_W1_HIF_TRANSFER_MAX, GFP_KERNEL);
	if (!image_packet)
		return -ENOMEM;

	wifi_np = of_find_compatible_node(NULL, NULL, "mediatek,wifi");
	if (!wifi_np) {
		ret = -ENODEV;
		goto out_free_image_packet;
	}
	ret = of_address_to_resource(wifi_np, 0, &hif);
	of_node_put(wifi_np);
	if (ret)
		goto out_free_image_packet;
	if (hif.start != MT6797_WIFI_AHB_HIF_BASE ||
	    resource_size(&hif) < MT6797_WIFI_AHB_HIF_PORTAL_SIZE) {
		ret = -EINVAL;
		goto out_free_image_packet;
	}
	if (!request_mem_region(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE,
		"mt6797-wifi-w1")) {
		ret = -EBUSY;
		goto out_free_image_packet;
	}
	portal = ioremap(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
	if (!portal) {
		ret = -ENOMEM;
		goto out_release_region;
	}
	mutex_lock(&mt6797_wifi_w1_portal_lock);
	ret = mt6797_wifi_w1_enable_function(portal, &ioex_before,
		&function_enabled);
	if (ret)
		goto out_unlock;
	ret = mt6797_wifi_w1_set_block_size(portal, &block_low_before,
		&block_high_before, &block_size_set);
	if (ret)
		goto out_unlock;
	ret = mt6797_wifi_w1_driver_own(portal);
	if (ret)
		goto out_unlock;

	/* nicInitializeAdapter() sets WHIER, then nicDisableInterrupt() gates it. */
	ret = mt6797_wifi_w1_reg_write(portal, MT6797_WIFI_W1_MCR_WHIER,
		MT6797_WIFI_W1_WHIER_DEFAULT);
	if (!ret)
		ret = mt6797_wifi_w1_reg_write(portal, MT6797_WIFI_W1_MCR_WHLPCR,
			MT6797_WIFI_W1_WHLPCR_INT_CLR);
	for (i = 0; !ret && i < 2; i++) {
		ret = mt6797_wifi_w1_send_config(portal, &power->image.section[i],
			!i, sequence++);
		if (!ret)
			ret = mt6797_wifi_w1_send_idlm(portal, power->firmware->data,
				&power->image.section[i], image_packet);
	}
	if (!ret)
		ret = mt6797_wifi_w1_copy_emi(power->owner, power->firmware->data,
			&power->image);
	if (!ret)
		ret = mt6797_wifi_w1_send_start(portal, sequence);
	if (!ret)
		ret = mt6797_wifi_w1_wait_ready(portal);
	/* Do not enable IRQ: vendor has no isolated post-enable event source. */
	if (!ret)
		ret = mt6797_wifi_w1_clear_post_ready_status(portal);
out_unlock:
	/* Every attempted mutable CCCR operation is read back after restoration. */
	cleanup_ret = 0;
	if (block_size_set)
		cleanup_ret = mt6797_wifi_w1_restore_block_size(portal,
			block_low_before, block_high_before);
	if (function_enabled)
		cleanup_ret = cleanup_ret ?: mt6797_wifi_w1_restore_function(portal,
			ioex_before);
	mutex_unlock(&mt6797_wifi_w1_portal_lock);
	if (cleanup_ret) {
		dev_err(power->owner,
			"W1 portal cleanup could not be verified: %d; reset/fence required\n",
			cleanup_ret);
		if (!ret)
			ret = cleanup_ret;
	}
	iounmap(portal);
out_release_region:
	release_mem_region(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
out_free_image_packet:
	kfree(image_packet);
	return ret;
}

static int mt6797_wifi_w1_power_get_voltage(void *context)
{
	return regulator_get_voltage(((struct mt6797_wifi_w1_power *)context)->wifi_paldo);
}

static int mt6797_wifi_w1_power_is_enabled(void *context, bool wifi)
{
	struct mt6797_wifi_w1_power *power = context;

	return regulator_is_enabled(wifi ? power->wifi_paldo : power->bt_paldo);
}

static int mt6797_wifi_w1_power_set_voltage(void *context, int microvolts)
{
	return regulator_set_voltage(((struct mt6797_wifi_w1_power *)context)->wifi_paldo,
		microvolts, microvolts);
}

static int mt6797_wifi_w1_power_enable(void *context)
{
	return regulator_enable(((struct mt6797_wifi_w1_power *)context)->wifi_paldo);
}

static int mt6797_wifi_w1_power_disable(void *context)
{
	return regulator_disable(((struct mt6797_wifi_w1_power *)context)->wifi_paldo);
}

static int mt6797_wifi_w1_power_stream(void *context)
{
	return mt6797_wifi_w1_stream(context);
}

static void mt6797_wifi_w1_power_fatal(void *context, int causal, int cleanup)
{
	struct mt6797_wifi_w1_power *power = context;

	dev_err(power->owner,
		"W1 PALDO state dirty (causal=%d cleanup=%d); reboot required\n",
		causal, cleanup);
}

int mt6797_wifi_ahb_w1_firmware_preflight(struct device *owner,
		  const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		  struct regulator *bt_paldo, struct regulator *wifi_paldo)
{
	struct mt6797_wifi_w1_power power = {
		.owner = owner,
		.lease = lease,
		.bt_paldo = bt_paldo,
		.wifi_paldo = wifi_paldo,
	};
	struct mt6797_wifi_ahb_w0_ops power_ops = {
		.get_voltage = mt6797_wifi_w1_power_get_voltage,
		.is_enabled = mt6797_wifi_w1_power_is_enabled,
		.set_voltage = mt6797_wifi_w1_power_set_voltage,
		.enable = mt6797_wifi_w1_power_enable,
		.disable = mt6797_wifi_w1_power_disable,
		.observe = mt6797_wifi_w1_power_stream,
		.fatal = mt6797_wifi_w1_power_fatal,
		.context = &power,
	};
	struct mt6797_wifi_ahb_w0_result power_result;
	int ret;

	if (!owner || !lease || !bt_paldo || !wifi_paldo)
		return -EINVAL;
	if (!mt6797_btif_stp_bootstrap_full_wmt_lease_active(lease))
		return -ESHUTDOWN;
	if (!mt6797_wifi_ahb_w1_firmware_enabled(owner))
		return -EPERM;
	ret = request_firmware(&power.firmware, MT6797_WIFI_W1_FIRMWARE_NAME, owner);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_w1_firmware_validate(power.firmware->data,
		power.firmware->size, MT6797_WIFI_W1_FIRMWARE_SIZE,
		mt6797_wifi_w1_firmware_sha256);
	if (!ret)
		ret = mt6797_wifi_ahb_w1_firmware_parse(power.firmware->data,
			power.firmware->size, &power.image);
	if (!ret)
		ret = mt6797_wifi_ahb_w0_run(&power_ops, &power_result, 3300000);
	if (ret)
		dev_err(owner, "W1 firmware proof failed: %d; reset/fence required\n", ret);
	else
		dev_info(owner,
			"W1 complete HIF/EMI/start/READY proof passed; IRQ/netdev fenced\n");
	release_firmware(power.firmware);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_preflight);

MODULE_DESCRIPTION("MT6797 WLAN GEN3 AHB W1 pinned firmware proof");
MODULE_LICENSE("GPL");
