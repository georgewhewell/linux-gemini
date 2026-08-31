// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded MT6797 GEN3 WLAN firmware download/start proof.
 *
 * The vendor sequence is deliberately followed rather than approximated:
 * nic/nic_pwr_mgt.c:262-290 obtains driver ownership and disables global
 * interrupt delivery; common/wlan_lib.c:2577-2612 configures and sends every
 * divided section; :827-887 sends sections 0-1 over HIF and copies later
 * sections through the first 512 KiB of CONSYS EMI; and nic_pwr_mgt.c:348-389
 * starts firmware and waits for WCIR.READY. W1 clears the same status and
 * leaves global IRQ delivery off. The separately gated W2 extension then
 * mirrors the vendor NIC-capability transaction: it sends one normal query
 * and polls only the new port-1 response before any device IRQ enable. W1_DIAG
 * and W2_DIAG deliberately retain the causal register snapshots for an
 * enabled attempt. They make a safe failure useful: the next build must
 * distinguish CCCR function readiness, ownership, download acknowledgement,
 * each transfer, EMI placement, start, READY, IRQ/event, and portal cleanup
 * before anyone considers another hardware cycle.
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
#include <linux/jiffies.h>
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
#define MT6797_WIFI_W1_MCR_WRDR1		0x0054U
#define MT6797_WIFI_W1_MCR_WRPLR		0x0090U
#define MT6797_WIFI_W1_MCR_WTQCR0		0x0130U

#define MT6797_WIFI_W1_WHLPCR_DRIVER_OWN	BIT(8)
#define MT6797_WIFI_W1_WHLPCR_OWN_CLR		BIT(9)
#define MT6797_WIFI_W1_WHLPCR_INT_CLR		BIT(1)
#define MT6797_WIFI_W1_WHLPCR_INT_SET		BIT(0)
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
#define MT6797_WIFI_W1_OWN_TIMEOUT_MS		2048U
#define MT6797_WIFI_W1_OWN_REISSUE_POLLS	256U
#define MT6797_WIFI_W1_RESPONSE_POLLS		20000U
#define MT6797_WIFI_W1_READY_POLLS		100U
#define MT6797_WIFI_W1_IDLM_FRAME_COUNT	8U
#define MT6797_WIFI_W1_IDLM_TRANSFER_BYTES	18432U

/*
 * wlan_lib.c:3726-3791 uses this ordinary post-READY query.  It allocates
 * CMD_HDR_SIZE + EVENT_NIC_CAPABILITY_T, so both command and event are 124
 * bytes (the 8-byte HIF header plus the 116-byte capability payload).
 */
#define MT6797_WIFI_W2_CMD_NIC_CAPABILITY	0x80U
#define MT6797_WIFI_W2_EVENT_NIC_CAPABILITY	0x01U
#define MT6797_WIFI_W2_CAPABILITY_SEQUENCE	1U
#define MT6797_WIFI_W2_CAPABILITY_LENGTH	124U
#define MT6797_WIFI_W2_RESPONSE_TIMEOUT_MS	1000U
/*
 * config.h:333-346 bounds a single non-aggregate receive packet at 2352
 * bytes.  nicRxWaitResponse() reads four appended HIF bytes, and ahb.c:
 * 810-837 rounds a block-mode transfer to the 512-byte function block size.
 */
#define MT6797_WIFI_W2_RX_PACKET_MAX		2352U
#define MT6797_WIFI_W2_RX_DRAIN_MAX		2560U
#define MT6797_WIFI_W2_RX_DRAIN_MAX_PACKETS	32U

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

static int mt6797_wifi_w1_reg_read(void __iomem *portal, u32 address, u32 *value);

static DEFINE_MUTEX(mt6797_wifi_w1_portal_lock);
static bool enable_experimental_wifi_w1_firmware_preflight;
static bool enable_experimental_wifi_w2_nic_capability;
module_param_named(enable_experimental_wifi_w1_firmware_preflight,
			   enable_experimental_wifi_w1_firmware_preflight, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w1_firmware_preflight,
			 "allow one MT6797 WLAN firmware download/start/READY proof");
module_param_named(enable_experimental_wifi_w2_nic_capability,
			   enable_experimental_wifi_w2_nic_capability, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w2_nic_capability,
			 "allow one vendor-polled MT6797 WLAN NIC capability query after READY");

struct mt6797_wifi_w1_power {
	struct device *owner;
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease;
	struct regulator *bt_paldo;
	struct regulator *wifi_paldo;
	const struct firmware *firmware;
	struct mt6797_wifi_ahb_w1_image image;
	/* W4 owns the resulting live function and drops PALDO on its stop path. */
	bool retain_hif;
};

struct mt6797_wifi_w1_config_ack {
	u32 wrplr;
	u16 length;
	unsigned int polls;
	u8 header[9];
};

struct mt6797_wifi_w2_capability_poll {
	void __iomem *portal;
	u8 sequence;
	u8 event[MT6797_WIFI_W2_CAPABILITY_LENGTH];
	u8 response[MT6797_WIFI_W2_CAPABILITY_LENGTH + sizeof(u32)];
	u8 drain[MT6797_WIFI_W2_RX_DRAIN_MAX];
	u32 pre_whisr;
	u32 pre_wrplr;
	u32 post_whisr;
	u32 post_wrplr;
	unsigned int polls;
	unsigned int rx0_drained;
	unsigned int rx1_drained;
};

bool mt6797_wifi_ahb_w1_firmware_enabled(struct device *owner)
{
	return owner &&
		((READ_ONCE(enable_experimental_wifi_w1_firmware_preflight) &&
		  device_property_read_bool(owner,
			"mediatek,experimental-wifi-w1-firmware-preflight")) ||
		 (IS_ENABLED(CONFIG_MTK_MT6797_WIFI_AHB_W2_NIC_CAPABILITY) &&
		  READ_ONCE(enable_experimental_wifi_w2_nic_capability) &&
		  device_property_read_bool(owner,
			"mediatek,experimental-wifi-w2-nic-capability")));
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_enabled);

static bool mt6797_wifi_w2_capability_enabled(struct device *owner)
{
	return IS_ENABLED(CONFIG_MTK_MT6797_WIFI_AHB_W2_NIC_CAPABILITY) && owner &&
		READ_ONCE(enable_experimental_wifi_w2_nic_capability) &&
		device_property_read_bool(owner,
			"mediatek,experimental-wifi-w2-nic-capability");
}

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
	u32 blocks;
	size_t i;
	int ret;

	if (!data || !length || length & 3)
		return -EINVAL;
	if (length < MT6797_WIFI_AHB_SDIO_BLOCK_SIZE) {
		ret = mt6797_wifi_ahb_cmd53_read_pack(address, length, &command);
		goto out_command;
	}
	if (length % MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return -EINVAL;
	blocks = length / MT6797_WIFI_AHB_SDIO_BLOCK_SIZE;
	if (blocks > MT6797_WIFI_AHB_CMD53_COUNT_MAX)
		return -EINVAL;
	/* Vendor sdio.h:76-87: fixed port, block mode, function 1, read. */
	command = blocks | (address << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) |
		BIT(27) | BIT(MT6797_WIFI_AHB_CMD_FUNC_SHIFT);
	ret = 0;
out_command:
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	for (i = 0; i < length; i += sizeof(u32))
		put_unaligned_le32(readl(portal + MT6797_WIFI_AHB_CMD53_DATA),
				   data + i);
	return 0;
}

static size_t mt6797_wifi_w2_rx_transfer_size(u16 packet_length)
{
	size_t transfer;

	transfer = ALIGN((size_t)packet_length + sizeof(u32), sizeof(u32));
	if (transfer >= MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		transfer = ALIGN(transfer, MT6797_WIFI_AHB_SDIO_BLOCK_SIZE);
	return transfer;
}

static int mt6797_wifi_w2_drain_port(struct mt6797_wifi_w2_capability_poll *capability,
				     unsigned int port, u16 packet_length)
{
	size_t transfer;

	if (!capability || port > 1 || !packet_length ||
	    packet_length > MT6797_WIFI_W2_RX_PACKET_MAX)
		return -EMSGSIZE;
	transfer = mt6797_wifi_w2_rx_transfer_size(packet_length);
	if (transfer > sizeof(capability->drain))
		return -EOVERFLOW;
	return mt6797_wifi_w1_cmd53_read(capability->portal,
		port ? MT6797_WIFI_W1_MCR_WRDR1 : MT6797_WIFI_W1_MCR_WRDR0,
		capability->drain, transfer);
}

static int
mt6797_wifi_w2_drain_pending(struct mt6797_wifi_w2_capability_poll *capability,
			     u32 *last_wrplr)
{
	unsigned int i;
	int ret;

	if (!capability || !last_wrplr)
		return -EINVAL;
	for (i = 0; i < MT6797_WIFI_W2_RX_DRAIN_MAX_PACKETS; i++) {
		u16 rx0_length;
		u16 rx1_length;

		ret = mt6797_wifi_w1_reg_read(capability->portal,
					      MT6797_WIFI_W1_MCR_WRPLR, last_wrplr);
		if (ret)
			return ret;
		rx0_length = *last_wrplr & U16_MAX;
		rx1_length = *last_wrplr >> 16;
		if (!rx0_length && !rx1_length)
			return 0;
		/* Match nicRxWaitResponse(): port 0 first, then port 1. */
		if (rx0_length) {
			ret = mt6797_wifi_w2_drain_port(capability, 0, rx0_length);
			if (ret)
				return ret;
			capability->rx0_drained++;
		}
		if (rx1_length) {
			ret = mt6797_wifi_w2_drain_port(capability, 1, rx1_length);
			if (ret)
				return ret;
			capability->rx1_drained++;
		}
	}
	return -EOVERFLOW;
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
					   u8 *ioex_after, u8 *iorx_after,
					   bool *enabled)
{
	int ret;

	if (!ioex_before || !ioex_after || !iorx_after || !enabled)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_before);
	if (ret)
		return ret;
	if (*ioex_before & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI))
		return -EBUSY;
	/* A failed CMD52 write may still have reached the function-enable latch. */
	*enabled = true;
	ret = mt6797_wifi_w1_cmd52_write(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX,
		*ioex_before | BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI));
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_after);
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IORX, iorx_after);
	if (ret)
		return ret;
	return *iorx_after & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI) ? 0 : -ETIMEDOUT;
}

static int mt6797_wifi_w1_set_block_size(void __iomem *portal, u8 *low_before,
					  u8 *high_before, u8 *low_after,
					  u8 *high_after, bool *changed)
{
	int ret;

	if (!low_before || !high_before || !low_after || !high_after || !changed)
		return -EINVAL;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
	if (ret)
		return ret;
	*changed = false;
	if (*low_before) {
		/* A CMD52 error does not prove that the writable CCCR was unchanged. */
		*changed = true;
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, 0);
		if (ret)
			return ret;
	}
	if (*high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8) {
		*changed = true;
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1,
			MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_after);
	if (ret)
		return ret;
	return mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_after);
}

static int mt6797_wifi_w1_restore_block_size(void __iomem *portal,
					      u8 low_before, u8 high_before,
					      u8 *low_after, u8 *high_after)
{
	int cleanup = 0;
	int ret;

	if (!low_after || !high_after)
		return -EINVAL;

	if (high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
		if (ret)
			cleanup = ret;
	}
	if (low_before) {
		ret = mt6797_wifi_w1_cmd52_write(portal,
			MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
		if (!cleanup && ret)
			cleanup = ret;
	}
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_after);
	if (!cleanup && ret)
		cleanup = ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_after);
	if (!cleanup && ret)
		cleanup = ret;
	if (!cleanup && (*low_after != low_before || *high_after != high_before))
		cleanup = -EUCLEAN;
	return cleanup;
}

static int mt6797_wifi_w1_restore_function(void __iomem *portal,
					     u8 ioex_before, u8 *ioex_after)
{
	int cleanup = 0;
	int ret;

	if (!ioex_after)
		return -EINVAL;

	ret = mt6797_wifi_w1_cmd52_write(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_before);
	if (ret)
		cleanup = ret;
	ret = mt6797_wifi_w1_cmd52_read(portal,
		MT6797_WIFI_AHB_SDIO_CCCR_IOEX, ioex_after);
	if (!cleanup && ret)
		cleanup = ret;
	if (!cleanup && *ioex_after != ioex_before)
		cleanup = -EUCLEAN;
	return cleanup;
}

static int mt6797_wifi_w1_driver_own(void __iomem *portal, u32 *before,
					     u32 *first, u32 *last,
					     unsigned int *polls, unsigned int *requests)
{
	unsigned int i;
	unsigned long deadline;
	int ret;

	if (!before || !first || !last || !polls || !requests)
		return -EINVAL;
	*before = 0;
	*first = 0;
	*last = 0;
	*polls = 0;
	*requests = 0;

	/*
	 * nic_pwr_mgt.c:147-223 nicpmSetDriverOwn(): read ownership first,
	 * issue OWN_CLR on iterations 0, 256, ... and sleep one millisecond.
	 * This has the vendor's 2048 ms time bound, but lacks an adapter state
	 * through which to inspect its card/bus/no-ack early-exit predicates.
	 */
	deadline = jiffies + msecs_to_jiffies(MT6797_WIFI_W1_OWN_TIMEOUT_MS);
	for (i = 0; ; i++) {
		ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WHLPCR,
			last);
		if (ret)
			return ret;
		*polls = i + 1;
		if (!i) {
			*before = *last;
			*first = *last;
		}
		if (*last & MT6797_WIFI_W1_WHLPCR_DRIVER_OWN)
			return 0;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		if (!(i & (MT6797_WIFI_W1_OWN_REISSUE_POLLS - 1))) {
			ret = mt6797_wifi_w1_reg_write(portal,
				MT6797_WIFI_W1_MCR_WHLPCR,
				MT6797_WIFI_W1_WHLPCR_OWN_CLR);
			if (ret)
				return ret;
			(*requests)++;
		}
		usleep_range(900, 1000);
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

int mt6797_wifi_ahb_w2_capability_command_build(u8 *command, size_t length,
						 u8 sequence)
{
	if (!command)
		return -EINVAL;
	if (length != MT6797_WIFI_W2_CAPABILITY_LENGTH)
		return -EMSGSIZE;
	/* nic.c:1025-1040 starts the normal command sequence at zero. */
	if (sequence != MT6797_WIFI_W2_CAPABILITY_SEQUENCE)
		return -EINVAL;

	memset(command, 0, length);
	/* wlan_lib.c:3748-3770, CMD_PQ_ID/CMD_PACKET_TYPE_ID. */
	mt6797_wifi_w1_init_header(command, length, MT6797_WIFI_W1_PQ_CMD,
		MT6797_WIFI_W2_CMD_NIC_CAPABILITY, sequence);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w2_capability_command_build);

int mt6797_wifi_ahb_w2_capability_event_parse(const u8 *event, size_t length,
					       u8 sequence,
					       struct mt6797_wifi_ahb_w2_capability *capability)
{
	if (!event || !capability)
		return -EINVAL;
	if (length != MT6797_WIFI_W2_CAPABILITY_LENGTH ||
	    get_unaligned_le16(event) != MT6797_WIFI_W2_CAPABILITY_LENGTH)
		return -EMSGSIZE;
	if (get_unaligned_le16(event + 2) != MT6797_WIFI_W1_EVENT_PACKET_TYPE ||
	    event[4] != MT6797_WIFI_W2_EVENT_NIC_CAPABILITY ||
	    event[5] != sequence)
		return -EPROTO;

	/* nic_cmd_event.h:495-516, after the 8-byte WIFI_EVENT_T header. */
	memset(capability, 0, sizeof(*capability));
	capability->product_id = get_unaligned_le16(event + 8);
	capability->firmware_version = get_unaligned_le16(event + 10);
	capability->driver_version = get_unaligned_le16(event + 12);
	memcpy(capability->mac_address, event + 16,
	       sizeof(capability->mac_address));
	capability->feature_flag0 = get_unaligned_le32(event + 44);
	capability->feature_flag1 = get_unaligned_le32(event + 48);
	capability->compile_flag0 = get_unaligned_le32(event + 52);
	capability->compile_flag1 = get_unaligned_le32(event + 56);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w2_capability_event_parse);

static int mt6797_wifi_w1_wait_config_ack(void __iomem *portal, u8 sequence,
					  struct mt6797_wifi_w1_config_ack *ack)
{
	u8 event[MT6797_WIFI_W1_RESPONSE_MAX] = { };
	u32 response_length;
	unsigned int i;
	int ret;

	if (!ack)
		return -EINVAL;
	ack->wrplr = 0;
	ack->length = 0;
	ack->polls = 0;
	memset(ack->header, 0, sizeof(ack->header));
	for (i = 0; i < MT6797_WIFI_W1_RESPONSE_POLLS; i++) {
		ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WRPLR,
			&response_length);
		if (ret)
			return ret;
		ack->wrplr = response_length;
		response_length &= U16_MAX;
		ack->length = response_length;
		ack->polls = i + 1;
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
	memcpy(ack->header, event, sizeof(ack->header));
	if (get_unaligned_le16(event) != response_length ||
	    get_unaligned_le16(event + 2) != MT6797_WIFI_W1_EVENT_PACKET_TYPE ||
	    event[4] != MT6797_WIFI_W1_EVENT_CMD_RESULT || event[5] != sequence ||
	    event[8])
		return -EPROTO;
	return 0;
}

static int mt6797_wifi_w1_send_config(void __iomem *portal,
				       const struct mt6797_wifi_ahb_w1_section *section,
				       bool reset, u8 sequence,
				       struct mt6797_wifi_w1_config_ack *ack)
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
	return mt6797_wifi_w1_wait_config_ack(portal, sequence, ack);
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
				    u8 *packet, u32 *frames, u32 *bytes)
{
	u32 sent = 0;
	int ret;

	if (!image || !section || !frames || !bytes)
		return -EINVAL;
	*frames = 0;
	*bytes = 0;
	while (sent < section->length) {
		size_t chunk = min_t(u32, MT6797_WIFI_W1_IMAGE_PACKET_SIZE,
			section->length - sent);

		ret = mt6797_wifi_w1_send_image_packet(portal, packet,
			image + section->offset + sent, chunk);
		if (ret)
			return ret;
		sent += chunk;
		(*frames)++;
		*bytes = sent;
	}
	return 0;
}

static int mt6797_wifi_w1_copy_emi(struct device *owner, const u8 *image,
				    const struct mt6797_wifi_ahb_w1_image *parsed)
{
	struct resource window;
	void __iomem *emi;
	resource_size_t window_size;
	unsigned int i;
	int close_ret;
	int ret;

	if (!owner || !image || !parsed)
		return -EINVAL;
	/* The persistent region-19 owner serializes this temporary region-18 grant. */
	ret = mt6797_consys_emi_mpu_wifi_window_open(owner, &window);
	if (ret) {
		dev_info(owner, "W1_DIAG stage=emi-open ret=%d\n", ret);
		return ret;
	}
	window_size = resource_size(&window);
	dev_info(owner, "W1_DIAG stage=emi-open ret=0 start=%pa size=%pa\n",
		 &window.start, &window_size);
	if (window_size != MT6797_WIFI_W1_EMI_SIZE) {
		ret = -EINVAL;
		goto close_window;
	}
	emi = ioremap(window.start, window_size);
	if (!emi) {
		ret = -ENOMEM;
		dev_info(owner, "W1_DIAG stage=emi-map ret=%d\n", ret);
		goto close_window;
	}
	for (i = 2; i < ARRAY_SIZE(parsed->section); i++) {
		const struct mt6797_wifi_ahb_w1_section *section =
			&parsed->section[i];
		u32 offset = section->destination & GENMASK(19, 0);

		if (offset > MT6797_WIFI_W1_EMI_SIZE - section->length) {
			ret = -EFBIG;
			dev_info(owner,
				 "W1_DIAG stage=emi-copy section=%u ret=%d image=%#x dest=%#x length=%#x offset=%#x\n",
				 i, ret, section->offset, section->destination,
				 section->length, offset);
			break;
		}
		dev_info(owner,
			 "W1_DIAG stage=emi-copy section=%u ret=0 image=%#x dest=%#x length=%#x offset=%#x\n",
			 i, section->offset, section->destination, section->length, offset);
		memcpy_toio(emi + offset, image + section->offset, section->length);
	}
	/* Complete all mapped writes before region 18 is restored. */
	wmb();
	iounmap(emi);
close_window:
	close_ret = mt6797_consys_emi_mpu_wifi_window_close(owner);
	dev_info(owner, "W1_DIAG stage=emi-close causal=%d cleanup=%d\n",
		 ret, close_ret);
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

static int mt6797_wifi_w1_wait_ready(void __iomem *portal, u32 *last_raw,
				     unsigned int *polls)
{
	struct mt6797_wifi_ahb_wcir wcir;
	unsigned int i;
	int ret;

	if (!last_raw || !polls)
		return -EINVAL;
	*last_raw = 0;
	*polls = 0;
	for (i = 0; i < MT6797_WIFI_W1_READY_POLLS; i++) {
		ret = mt6797_wifi_w1_reg_read(portal,
			MT6797_WIFI_AHB_WCIR_OFFSET, last_raw);
		if (ret)
			return ret;
		*polls = i + 1;
		ret = mt6797_wifi_ahb_wcir_decode(*last_raw, &wcir);
		if (ret)
			return ret;
		if (wcir.wlan_ready)
			return 0;
		usleep_range(10000, 11000);
	}
	return -ETIMEDOUT;
}

static int mt6797_wifi_w1_clear_post_ready_status(void __iomem *portal,
					   u32 *status_out, u32 released_out[8])
{
	unsigned int i;
	int ret;

	if (!status_out || !released_out)
		return -EINVAL;
	*status_out = 0;
	memset(released_out, 0, 8 * sizeof(*released_out));
	/* nic_pwr_mgt.c:379-386: clear WHISR and consume TX release counts. */
	ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WHISR, status_out);
	if (ret)
		return ret;
	if (!(*status_out & BIT(0)))
		return 0;
	for (i = 0; i < 8; i++) {
		ret = mt6797_wifi_w1_reg_read(portal,
			MT6797_WIFI_W1_MCR_WTQCR0 + i * sizeof(u32), &released_out[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6797_wifi_w2_poll_capability(
		struct mt6797_wifi_w2_capability_poll *capability)
{
	unsigned long deadline;
	u16 response_length;
	int ret;

	if (!capability)
		return -EINVAL;
	/* Vendor nicRxWaitResponse(): 50-us port-1 polls, bounded to 1000 ms. */
	deadline = jiffies + msecs_to_jiffies(MT6797_WIFI_W2_RESPONSE_TIMEOUT_MS);
	for (;;) {
		ret = mt6797_wifi_w1_reg_read(capability->portal,
			MT6797_WIFI_W1_MCR_WRPLR, &capability->post_wrplr);
		if (ret)
			return ret;
		response_length = capability->post_wrplr >> 16;
		if (response_length) {
			if (response_length != MT6797_WIFI_W2_CAPABILITY_LENGTH)
				return -EMSGSIZE;
			/* nic_rx.c:3680-3692 reads the exact four extra portal bytes. */
			ret = mt6797_wifi_w1_cmd53_read(capability->portal,
				MT6797_WIFI_W1_MCR_WRDR1, capability->response,
				sizeof(capability->response));
			if (ret)
				return ret;
			memcpy(capability->event, capability->response,
			       sizeof(capability->event));
			return 0;
		}
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		capability->polls++;
		udelay(50);
	}
}

static int mt6797_wifi_w2_query_capability(struct device *owner,
					    void __iomem *portal, u32 post_ready_whisr)
{
	struct mt6797_wifi_w2_capability_poll capability = {
		.portal = portal,
		.sequence = MT6797_WIFI_W2_CAPABILITY_SEQUENCE,
		.pre_whisr = post_ready_whisr,
	};
	struct mt6797_wifi_ahb_w2_capability parsed;
	u8 command[MT6797_WIFI_W2_CAPABILITY_LENGTH];
	u32 drained_wrplr;
	int snapshot_ret;
	int ret;

	if (!owner || !portal)
		return -EINVAL;

	/* A pre-existing port-1 packet could impersonate the capability response. */
	ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WRPLR,
		&capability.pre_wrplr);
	if (ret)
		goto out_summary;
	if (capability.pre_wrplr >> 16) {
		ret = -EUCLEAN;
		goto out_summary;
	}
	/* RX0 is independent of nicRxWaitResponse()'s RX1 transaction. */
	if (capability.pre_wrplr & U16_MAX) {
		ret = mt6797_wifi_w2_drain_port(&capability, 0,
						capability.pre_wrplr & U16_MAX);
		if (ret)
			goto out_summary;
		capability.rx0_drained++;
	}
	/*
	 * nicRxWaitResponse() polls only the selected RX1 port.  RX0 carries
	 * asynchronous firmware traffic (the post-READY 0x59-byte frame observed
	 * on MT6797 is one such packet), so it cannot impersonate this response.
	 * A stale RX1 packet still can, and must fail the retained handoff.
	 */
	ret = mt6797_wifi_w1_reg_read(portal, MT6797_WIFI_W1_MCR_WRPLR,
				      &drained_wrplr);
	if (ret)
		goto out_summary;
	if (drained_wrplr >> 16) {
		ret = -EUCLEAN;
		goto out_summary;
	}
	ret = mt6797_wifi_ahb_w2_capability_command_build(command,
		sizeof(command), capability.sequence);
	if (ret)
		goto out_summary;

	/* wlan_lib.c:3772-3779 sends, then polls WRPLR/WRDR1 before nicEnableInterrupt. */
	ret = mt6797_wifi_w1_cmd53_write(portal, MT6797_WIFI_W1_MCR_WTDR1,
		command, sizeof(command));
	if (!ret)
		ret = mt6797_wifi_w2_poll_capability(&capability);
	if (!ret)
		capability.rx1_drained++;
	if (!ret)
		ret = mt6797_wifi_w2_drain_pending(&capability, &capability.post_wrplr);

	/* Terminal instrumentation only: it never controls acceptance or acks. */
	snapshot_ret = mt6797_wifi_w1_reg_read(portal,
		MT6797_WIFI_W1_MCR_WHISR, &capability.post_whisr);
	if (!ret && snapshot_ret)
		ret = snapshot_ret;
	if (!ret) {
		ret = mt6797_wifi_ahb_w2_capability_event_parse(capability.event,
			sizeof(capability.event), capability.sequence, &parsed);
		if (!ret)
			dev_info(owner,
				 "W2_DIAG stage=event ret=0 product=%#04x firmware=%#04x driver=%#04x mac=%pM feature=%#08x/%#08x compile=%#08x/%#08x\n",
				 parsed.product_id, parsed.firmware_version,
				 parsed.driver_version, parsed.mac_address,
				 parsed.feature_flag0, parsed.feature_flag1,
				 parsed.compile_flag0, parsed.compile_flag1);
	}
out_summary:
	dev_info(owner,
		 "W2_DIAG stage=capability-poll causal=%d pre-whisr=%#08x pre-wrplr=%#08x post-whisr=%#08x post-wrplr=%#08x polls=%u rx0-drained=%u rx1-drained=%u sequence=%u\n",
		 ret, capability.pre_whisr, capability.pre_wrplr,
		 capability.post_whisr, capability.post_wrplr, capability.polls,
		 capability.rx0_drained, capability.rx1_drained, capability.sequence);
	return ret;
}

static int mt6797_wifi_w1_stream(struct mt6797_wifi_w1_power *power)
{
	struct device_node *wifi_np;
	struct resource hif;
	struct mt6797_wifi_w1_config_ack ack;
	void __iomem *portal = NULL;
	u8 *image_packet;
	u8 ioex_before = 0;
	u8 ioex_after = 0;
	u8 iorx_after = 0;
	u8 block_low_before = 0;
	u8 block_high_before = 0;
	u8 block_low_after = 0;
	u8 block_high_after = 0;
	u8 block_low_restored = 0;
	u8 block_high_restored = 0;
	u8 ioex_restored = 0;
	u8 sequence = 1;
	bool function_enabled = false, block_size_set = false;
	bool w2_capability;
	int block_cleanup_ret = 0;
	int cleanup_ret = 0;
	int function_cleanup_ret = 0;
	int ret;
	unsigned int i;
	unsigned int own_polls = 0;
	unsigned int own_requests = 0;
	unsigned int ready_polls = 0;
	u32 expected_frames;
	u32 idlm_frames = 0;
	u32 idlm_payload = 0;
	u32 own_before = 0;
	u32 own_first = 0;
	u32 own_last = 0;
	u32 ready_raw = 0;
	u32 status = 0;
	u32 released[8] = { };
	size_t expected_transfer_bytes;

	if (!power || !power->owner || !power->lease || !power->firmware ||
	    !mt6797_btif_stp_bootstrap_full_wmt_lease_active(power->lease))
		return -EPERM;
	w2_capability = mt6797_wifi_w2_capability_enabled(power->owner);
	ret = mt6797_wifi_ahb_w1_idlm_transfer_account(&power->image,
		&expected_frames, &expected_transfer_bytes);
	if (ret)
		return ret;
	if (expected_frames != MT6797_WIFI_W1_IDLM_FRAME_COUNT ||
	    expected_transfer_bytes != MT6797_WIFI_W1_IDLM_TRANSFER_BYTES)
		return -EPROTO;
	/* Allocate before the first portal mutation; it is reused for every frame. */
	image_packet = kmalloc(MT6797_WIFI_W1_HIF_TRANSFER_MAX, GFP_KERNEL);
	if (!image_packet)
		return -ENOMEM;

	wifi_np = of_find_compatible_node(NULL, NULL, "mediatek,wifi");
	if (!wifi_np) {
		ret = -ENODEV;
		dev_info(power->owner, "W1_DIAG stage=portal-node ret=%d\n", ret);
		goto out_free_image_packet;
	}
	ret = of_address_to_resource(wifi_np, 0, &hif);
	of_node_put(wifi_np);
	if (ret)
		dev_info(power->owner, "W1_DIAG stage=portal-resource ret=%d\n", ret);
	if (ret)
		goto out_free_image_packet;
	if (hif.start != MT6797_WIFI_AHB_HIF_BASE ||
	    resource_size(&hif) < MT6797_WIFI_AHB_HIF_PORTAL_SIZE) {
		ret = -EINVAL;
		dev_info(power->owner, "W1_DIAG stage=portal-resource ret=%d\n", ret);
		goto out_free_image_packet;
	}
	if (!request_mem_region(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE,
		"mt6797-wifi-w1")) {
		ret = -EBUSY;
		dev_info(power->owner, "W1_DIAG stage=portal-request ret=%d\n", ret);
		goto out_free_image_packet;
	}
	portal = ioremap(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
	if (!portal) {
		ret = -ENOMEM;
		dev_info(power->owner, "W1_DIAG stage=portal-map ret=%d\n", ret);
		goto out_release_region;
	}
	mutex_lock(&mt6797_wifi_w1_portal_lock);
	ret = mt6797_wifi_w1_enable_function(portal, &ioex_before,
		&ioex_after, &iorx_after, &function_enabled);
	dev_info(power->owner,
		 "W1_DIAG stage=function-enable ret=%d ioex-before=%#02x ioex-after=%#02x iorx=%#02x mutation=%u\n",
		 ret, ioex_before, ioex_after, iorx_after, function_enabled);
	if (ret)
		goto out_unlock;
	ret = mt6797_wifi_w1_set_block_size(portal, &block_low_before,
		&block_high_before, &block_low_after, &block_high_after,
		&block_size_set);
	dev_info(power->owner,
		 "W1_DIAG stage=block-size ret=%d before=%#02x/%#02x after=%#02x/%#02x mutation=%u\n",
		 ret, block_low_before, block_high_before, block_low_after,
		 block_high_after, block_size_set);
	if (ret)
		goto out_unlock;
	ret = mt6797_wifi_w1_driver_own(portal, &own_before, &own_first,
		&own_last, &own_polls, &own_requests);
	dev_info(power->owner,
		 "W1_DIAG stage=driver-own ret=%d whlpcr-before=%#08x first=%#08x last=%#08x polls=%u requests=%u\n",
		 ret, own_before, own_first, own_last, own_polls, own_requests);
	if (ret)
		goto out_unlock;

	/* nicInitializeAdapter() sets WHIER, then nicDisableInterrupt() gates it. */
	ret = mt6797_wifi_w1_reg_write(portal, MT6797_WIFI_W1_MCR_WHIER,
		MT6797_WIFI_W1_WHIER_DEFAULT);
	dev_info(power->owner, "W1_DIAG stage=whier ret=%d value=%#08x\n",
		 ret, MT6797_WIFI_W1_WHIER_DEFAULT);
	if (!ret) {
		ret = mt6797_wifi_w1_reg_write(portal, MT6797_WIFI_W1_MCR_WHLPCR,
			MT6797_WIFI_W1_WHLPCR_INT_CLR);
		dev_info(power->owner,
			 "W1_DIAG stage=interrupt-clear ret=%d value=%#08x\n", ret,
			 (u32)MT6797_WIFI_W1_WHLPCR_INT_CLR);
	}
	for (i = 0; !ret && i < 2; i++) {
		memset(&ack, 0, sizeof(ack));
		ret = mt6797_wifi_w1_send_config(portal, &power->image.section[i],
			!i, sequence, &ack);
		dev_info(power->owner,
			 "W1_DIAG stage=config section=%u ret=%d sequence=%u reset=%u encrypted=%u wrplr=%#08x length=%u polls=%u event-length=%u event-type=%#04x event-id=%u event-sequence=%u status=%u\n",
			 i, ret, sequence, !i, power->image.section[i].encrypted,
			 ack.wrplr, ack.length, ack.polls,
			 get_unaligned_le16(ack.header),
			 get_unaligned_le16(ack.header + 2), ack.header[4], ack.header[5],
			 ack.header[8]);
		sequence++;
		if (!ret) {
			ret = mt6797_wifi_w1_send_idlm(portal, power->firmware->data,
				&power->image.section[i], image_packet, &idlm_frames,
				&idlm_payload);
			dev_info(power->owner,
				 "W1_DIAG stage=idlm section=%u ret=%d frames=%u payload=%u expected=%#x\n",
				 i, ret, idlm_frames, idlm_payload,
				 power->image.section[i].length);
		}
	}
	if (!ret) {
		ret = mt6797_wifi_w1_copy_emi(power->owner, power->firmware->data,
			&power->image);
		dev_info(power->owner, "W1_DIAG stage=emi ret=%d\n", ret);
	}
	if (!ret) {
		ret = mt6797_wifi_w1_send_start(portal, sequence);
		dev_info(power->owner,
			 "W1_DIAG stage=wifi-start ret=%d sequence=%u\n", ret, sequence);
	}
	if (!ret) {
		ret = mt6797_wifi_w1_wait_ready(portal, &ready_raw, &ready_polls);
		dev_info(power->owner,
			 "W1_DIAG stage=ready ret=%d raw=%#08x polls=%u\n", ret,
			 ready_raw, ready_polls);
	}
	if (!ret) {
		ret = mt6797_wifi_w1_clear_post_ready_status(portal, &status, released);
		dev_info(power->owner,
			 "W1_DIAG stage=post-ready ret=%d whisr=%#08x released=%#08x/%#08x/%#08x/%#08x/%#08x/%#08x/%#08x/%#08x\n",
			 ret, status, released[0], released[1], released[2], released[3],
			 released[4], released[5], released[6], released[7]);
	}
	if (!ret && w2_capability)
		ret = mt6797_wifi_w2_query_capability(power->owner, portal, status);
out_unlock:
	/* W1 proof restores CCCR; W4 deliberately retains this live HIF state. */
	if (block_size_set && !power->retain_hif) {
		block_cleanup_ret = mt6797_wifi_w1_restore_block_size(portal,
			block_low_before, block_high_before, &block_low_restored,
			&block_high_restored);
		dev_info(power->owner,
			 "W1_DIAG stage=cleanup-block ret=%d restored=%#02x/%#02x expected=%#02x/%#02x\n",
			 block_cleanup_ret, block_low_restored, block_high_restored,
			 block_low_before, block_high_before);
	}
	if (function_enabled && !power->retain_hif) {
		function_cleanup_ret = mt6797_wifi_w1_restore_function(portal,
			ioex_before, &ioex_restored);
		dev_info(power->owner,
			 "W1_DIAG stage=cleanup-function ret=%d ioex=%#02x expected=%#02x\n",
			 function_cleanup_ret, ioex_restored, ioex_before);
	}
	cleanup_ret = block_cleanup_ret ?: function_cleanup_ret;
	dev_info(power->owner,
		 "W1_DIAG stage=stream-summary causal=%d cleanup=%d function-mutation=%u block-mutation=%u\n",
		 ret, cleanup_ret, function_enabled, block_size_set);
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
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = regulator_get_voltage(power->wifi_paldo);
	dev_info(power->owner, "W1_DIAG stage=paldo-get-voltage ret=%d\n", ret);
	return ret;
}

static int mt6797_wifi_w1_power_is_enabled(void *context, bool wifi)
{
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = regulator_is_enabled(wifi ? power->wifi_paldo : power->bt_paldo);
	dev_info(power->owner, "W1_DIAG stage=paldo-is-enabled rail=%s ret=%d\n",
		 wifi ? "wifi" : "bt", ret);
	return ret;
}

static int mt6797_wifi_w1_power_set_voltage(void *context, int microvolts)
{
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = regulator_set_voltage(power->wifi_paldo, microvolts, microvolts);
	dev_info(power->owner,
		 "W1_DIAG stage=paldo-set-voltage ret=%d target=%d\n", ret,
		 microvolts);
	return ret;
}

static int mt6797_wifi_w1_power_enable(void *context)
{
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = regulator_enable(power->wifi_paldo);
	dev_info(power->owner, "W1_DIAG stage=paldo-enable ret=%d\n", ret);
	return ret;
}

static int mt6797_wifi_w1_power_disable(void *context)
{
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = regulator_disable(power->wifi_paldo);
	dev_info(power->owner, "W1_DIAG stage=paldo-disable ret=%d\n", ret);
	return ret;
}

static int mt6797_wifi_w1_power_stream(void *context)
{
	struct mt6797_wifi_w1_power *power = context;
	int ret;

	ret = mt6797_wifi_w1_stream(power);
	dev_info(power->owner, "W1_DIAG stage=paldo-observe ret=%d\n", ret);
	return ret;
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
	struct mt6797_wifi_ahb_w0_result power_result = { };
	bool power_ran = false;
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
	if (!ret) {
		power_ran = true;
		ret = mt6797_wifi_ahb_w0_run(&power_ops, &power_result, 3300000);
		dev_info(owner,
			 "W1_DIAG stage=paldo-summary ret=%d causal=%d cleanup=%d fatal=%u selector-dirty=%u\n",
			 ret, power_result.causal, power_result.cleanup, power_result.fatal,
			 power_result.selector_dirty);
	}
	if (!power_ran && ret)
		dev_info(owner, "W1_DIAG stage=image-preflight ret=%d\n", ret);
	if (ret)
		dev_err(owner, "W1 firmware proof failed: %d; reset/fence required\n", ret);
	else
		dev_info(owner,
			"W1 complete HIF/EMI/start/READY proof passed; IRQ/netdev fenced\n");
	release_firmware(power.firmware);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_preflight);

/*
 * The original W1 entry point is intentionally a proof and always unwinds.
 * A radio cannot be built on that transaction.  Keep this sibling narrow: it
 * is the same image validation and vendor stream, but leaves exactly the
 * PALDO/function state established by the stream for W4 to own and tear down.
 */
int mt6797_wifi_ahb_w1_firmware_start_retained(struct device *owner,
		  const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		  struct regulator *bt_paldo, struct regulator *wifi_paldo)
{
	struct mt6797_wifi_w1_power power = {
		.owner = owner, .lease = lease, .bt_paldo = bt_paldo,
		.wifi_paldo = wifi_paldo, .retain_hif = true,
	};
	int ret;

	if (!owner || !lease || !bt_paldo || !wifi_paldo)
		return -EINVAL;
	if (!mt6797_btif_stp_bootstrap_full_wmt_lease_active(lease) ||
	    !mt6797_wifi_ahb_w1_firmware_enabled(owner))
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
		ret = mt6797_wifi_w1_power_set_voltage(&power, 3300000);
	if (!ret)
		ret = mt6797_wifi_w1_power_enable(&power);
	if (!ret)
		ret = mt6797_wifi_w1_stream(&power);
	if (ret) {
		mt6797_wifi_w1_power_disable(&power);
		dev_err(owner, "W1 retained start failed: %d\n", ret);
	} else {
		dev_info(owner, "W1 retained firmware/FUNC_CTRL handoff complete\n");
	}
	release_firmware(power.firmware);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_start_retained);

int mt6797_wifi_ahb_w1_firmware_stop_retained(struct regulator *wifi_paldo)
{
	if (!wifi_paldo)
		return -EINVAL;
	return regulator_disable(wifi_paldo);
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_stop_retained);

MODULE_DESCRIPTION("MT6797 WLAN GEN3 AHB W1 pinned firmware proof");
MODULE_LICENSE("GPL");
