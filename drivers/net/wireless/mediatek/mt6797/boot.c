// SPDX-License-Identifier: GPL-2.0-only
/* Bounded MT6797 GEN3 WLAN firmware download and start transaction. */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mt6797-consys-wifi.h>

#include "boot.h"
#include "firmware.h"
#include "hif.h"

#define MT6797_WIFI_CCCR_IOEX		0x0002
#define MT6797_WIFI_CCCR_IORX		0x0003
#define MT6797_WIFI_FBR1_BLOCK_SIZE	0x0110
#define MT6797_WIFI_FUNCTION		1

#define MT6797_WIFI_MCR_WCIR		0x0000
#define MT6797_WIFI_MCR_WHLPCR		0x0004
#define MT6797_WIFI_MCR_WHISR		0x0010
#define MT6797_WIFI_MCR_WHIER		0x0014
#define MT6797_WIFI_MCR_WTDR1		0x0034
#define MT6797_WIFI_MCR_WRDR0		0x0050
#define MT6797_WIFI_MCR_WRPLR		0x0090
#define MT6797_WIFI_MCR_WTQCR0		0x0130

#define MT6797_WIFI_WHLPCR_DRIVER_OWN	BIT(8)
#define MT6797_WIFI_WHLPCR_OWN_CLR	BIT(9)
#define MT6797_WIFI_WHLPCR_INT_CLR	BIT(1)
#define MT6797_WIFI_WHIER_DEFAULT	0xffffff47U

#define MT6797_WIFI_PQ_CMD		0x8000U
#define MT6797_WIFI_PQ_PDA		0xc000U
#define MT6797_WIFI_CMD_PACKET_TYPE	0xa0U
#define MT6797_WIFI_EVENT_PACKET_TYPE	0xe000U
#define MT6797_WIFI_CMD_DOWNLOAD_CONFIG	1U
#define MT6797_WIFI_CMD_WIFI_START	2U
#define MT6797_WIFI_EVENT_CMD_RESULT	1U
#define MT6797_WIFI_DOWNLOAD_ENCRYPT	BIT(0)
#define MT6797_WIFI_DOWNLOAD_RESET	BIT(3)
#define MT6797_WIFI_DOWNLOAD_ACK		BIT(31)

#define MT6797_WIFI_HIF_HEADER_SIZE	8U
#define MT6797_WIFI_IMAGE_PACKET_SIZE	2048U
#define MT6797_WIFI_RESPONSE_SIZE	32U
#define MT6797_WIFI_OWN_TIMEOUT_MS	2048U
#define MT6797_WIFI_OWN_REISSUE_POLLS	256U
#define MT6797_WIFI_RESPONSE_POLLS	20000U
#define MT6797_WIFI_READY_POLLS		100U
#define MT6797_WIFI_EMI_SIZE		SZ_512K
#define MT6797_WIFI_CHIP_ID		0x0279

static int mt6797_wifi_boot_cleanup(const struct mt6797_wifi_boot_ops *ops,
				    struct mt6797_wifi_boot_transaction *tx)
{
	int cleanup = 0;
	int ret;

	if (tx->block_size_touched) {
		ret = ops->restore_block_size(ops->context);
		if (ret)
			cleanup = ret;
	}
	if (tx->function_touched) {
		ret = ops->restore_function(ops->context);
		if (!cleanup && ret)
			cleanup = ret;
	}
	return cleanup;
}

static bool mt6797_wifi_boot_ops_valid(const struct mt6797_wifi_boot_ops *ops)
{
	return ops && ops->enable_function && ops->set_block_size &&
		ops->driver_own && ops->disable_interrupts &&
		ops->download_config && ops->download_idlm && ops->download_emi &&
		ops->start && ops->wait_ready && ops->clear_status &&
		ops->restore_block_size && ops->restore_function && ops->fence;
}

int mt6797_wifi_boot_transaction_run(const struct mt6797_wifi_boot_ops *ops,
				     struct mt6797_wifi_boot_transaction *tx)
{
	bool dirty;
	int cleanup;
	int ret;

	if (!mt6797_wifi_boot_ops_valid(ops) || !tx)
		return -EINVAL;
	memset(tx, 0, sizeof(*tx));

	ret = ops->enable_function(ops->context, &tx->function_touched);
	if (ret)
		goto fail;
	ret = ops->set_block_size(ops->context, &tx->block_size_touched);
	if (ret)
		goto fail;
	ret = ops->driver_own(ops->context);
	if (ret)
		goto fail;
	ret = ops->disable_interrupts(ops->context);
	if (ret)
		goto fail;

	/* A DOWNLOAD_CONFIG write can leave firmware state partially changed. */
	tx->firmware_dirty = true;
	ret = ops->download_config(ops->context, 0, true, 1);
	if (ret)
		goto fail;
	ret = ops->download_idlm(ops->context, 0);
	if (ret)
		goto fail;
	ret = ops->download_config(ops->context, 1, false, 2);
	if (ret)
		goto fail;
	ret = ops->download_idlm(ops->context, 1);
	if (ret)
		goto fail;
	ret = ops->download_emi(ops->context);
	if (ret)
		goto fail;
	ret = ops->start(ops->context, 3);
	if (ret)
		goto fail;
	ret = ops->wait_ready(ops->context);
	if (ret)
		goto fail;
	ret = ops->clear_status(ops->context);
	if (ret)
		goto fail;
	tx->ready = true;
	return 0;

fail:
	dirty = tx->firmware_dirty;
	cleanup = mt6797_wifi_boot_cleanup(ops, tx);
	memset(tx, 0, sizeof(*tx));
	if (dirty || cleanup || ret == -EUCLEAN)
		ops->fence(ops->context);
	return ret ?: cleanup;
}

int mt6797_wifi_boot_transaction_stop(const struct mt6797_wifi_boot_ops *ops,
				      struct mt6797_wifi_boot_transaction *tx)
{
	int ret;

	if (!mt6797_wifi_boot_ops_valid(ops) || !tx)
		return -EINVAL;
	ret = mt6797_wifi_boot_cleanup(ops, tx);
	memset(tx, 0, sizeof(*tx));
	if (ret)
		ops->fence(ops->context);
	return ret;
}

static int mt6797_wifi_reg_read(struct mt6797_wifi_boot *boot, u32 address,
				u32 *value)
{
	__le32 data;
	int ret;

	ret = mt6797_wifi_ahb_cmd53_read(boot->hif, address, &data,
					 sizeof(data));
	if (!ret)
		*value = le32_to_cpu(data);
	return ret;
}

static int mt6797_wifi_reg_write(struct mt6797_wifi_boot *boot, u32 address,
				 u32 value)
{
	__le32 data = cpu_to_le32(value);

	return mt6797_wifi_ahb_cmd53_write(boot->hif, address, &data,
					  sizeof(data));
}

static int mt6797_wifi_enable_function(void *context, bool *touched)
{
	struct mt6797_wifi_boot *boot = context;
	u8 ioex;
	u8 iorx;
	int ret;

	*touched = false;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0, MT6797_WIFI_CCCR_IOEX,
					 &boot->ioex_before);
	if (ret)
		return ret;
	if (boot->ioex_before & BIT(MT6797_WIFI_FUNCTION))
		return -EUCLEAN;
	*touched = true;
	ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0, MT6797_WIFI_CCCR_IOEX,
					  boot->ioex_before |
					  BIT(MT6797_WIFI_FUNCTION));
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0, MT6797_WIFI_CCCR_IOEX,
					 &ioex);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0, MT6797_WIFI_CCCR_IORX,
					 &iorx);
	if (ret)
		return ret;
	return (ioex & BIT(MT6797_WIFI_FUNCTION)) &&
		(iorx & BIT(MT6797_WIFI_FUNCTION)) ? 0 : -ETIMEDOUT;
}

static int mt6797_wifi_set_block_size(void *context, bool *touched)
{
	struct mt6797_wifi_boot *boot = context;
	u8 low;
	u8 high;
	int ret;

	*touched = false;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE,
					 &boot->block_low_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE + 1,
					 &boot->block_high_before);
	if (ret)
		return ret;
	if (boot->block_low_before) {
		*touched = true;
		ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0,
						  MT6797_WIFI_FBR1_BLOCK_SIZE, 0);
		if (ret)
			return ret;
	}
	if (boot->block_high_before != MT6797_WIFI_AHB_BLOCK_SIZE >> 8) {
		*touched = true;
		ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0,
						  MT6797_WIFI_FBR1_BLOCK_SIZE + 1,
						  MT6797_WIFI_AHB_BLOCK_SIZE >> 8);
		if (ret)
			return ret;
	}
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE, &low);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE + 1, &high);
	if (ret)
		return ret;
	return !low && high == MT6797_WIFI_AHB_BLOCK_SIZE >> 8 ? 0 : -EIO;
}

static int mt6797_wifi_driver_own(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	unsigned long deadline;
	unsigned int poll;
	u32 whlpcr;
	int ret;

	deadline = jiffies + msecs_to_jiffies(MT6797_WIFI_OWN_TIMEOUT_MS);
	for (poll = 0; ; poll++) {
		ret = mt6797_wifi_reg_read(boot, MT6797_WIFI_MCR_WHLPCR,
					   &whlpcr);
		if (ret)
			return ret;
		if (whlpcr & MT6797_WIFI_WHLPCR_DRIVER_OWN)
			return 0;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		if (!(poll & (MT6797_WIFI_OWN_REISSUE_POLLS - 1))) {
			ret = mt6797_wifi_reg_write(boot, MT6797_WIFI_MCR_WHLPCR,
						    MT6797_WIFI_WHLPCR_OWN_CLR);
			if (ret)
				return ret;
		}
		usleep_range(900, 1000);
	}
}

static int mt6797_wifi_disable_interrupts(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	int ret;

	ret = mt6797_wifi_reg_write(boot, MT6797_WIFI_MCR_WHIER,
				    MT6797_WIFI_WHIER_DEFAULT);
	if (ret)
		return ret;
	return mt6797_wifi_reg_write(boot, MT6797_WIFI_MCR_WHLPCR,
				     MT6797_WIFI_WHLPCR_INT_CLR);
}

static void mt6797_wifi_init_header(u8 *packet, u16 length, u16 pq,
				    u8 command, u8 sequence)
{
	put_unaligned_le16(length, packet);
	put_unaligned_le16(pq, packet + 2);
	packet[4] = command;
	packet[5] = MT6797_WIFI_CMD_PACKET_TYPE;
	packet[7] = sequence;
}

static int mt6797_wifi_wait_config_ack(struct mt6797_wifi_boot *boot,
				       u8 sequence)
{
	u8 event[MT6797_WIFI_RESPONSE_SIZE] = { };
	unsigned int poll;
	u32 length = 0;
	int ret;

	for (poll = 0; poll < MT6797_WIFI_RESPONSE_POLLS; poll++) {
		ret = mt6797_wifi_reg_read(boot, MT6797_WIFI_MCR_WRPLR, &length);
		if (ret)
			return ret;
		length &= U16_MAX;
		if (length)
			break;
		usleep_range(50, 100);
	}
	if (!length)
		return -ETIMEDOUT;
	/* Eight-byte init header plus the 20-byte INIT_EVENT_CMD_RESULT. */
	if (length != 28 || length + 4 > sizeof(event))
		return -EPROTO;
	ret = mt6797_wifi_ahb_cmd53_read(boot->hif, MT6797_WIFI_MCR_WRDR0,
					 event, ALIGN(length + 4, sizeof(u32)));
	if (ret)
		return ret;
	if (get_unaligned_le16(event) != length ||
	    get_unaligned_le16(event + 2) != MT6797_WIFI_EVENT_PACKET_TYPE ||
	    event[4] != MT6797_WIFI_EVENT_CMD_RESULT || event[5] != sequence ||
	    event[8])
		return -EPROTO;
	return 0;
}

static int mt6797_wifi_download_config(void *context, unsigned int index,
				       bool reset, u8 sequence)
{
	struct mt6797_wifi_boot *boot = context;
	const struct mt6797_wifi_section *section;
	u8 packet[20] = { };
	u32 mode = MT6797_WIFI_DOWNLOAD_ACK;
	int ret;

	if (index >= 2)
		return -EINVAL;
	section = &boot->firmware->section[index];
	if (section->encrypted)
		mode |= MT6797_WIFI_DOWNLOAD_ENCRYPT |
			(section->key_index & 0x3) << 1;
	if (reset)
		mode |= MT6797_WIFI_DOWNLOAD_RESET;
	mt6797_wifi_init_header(packet, sizeof(packet), MT6797_WIFI_PQ_CMD,
				MT6797_WIFI_CMD_DOWNLOAD_CONFIG, sequence);
	put_unaligned_le32(section->destination, packet + 8);
	put_unaligned_le32(section->length, packet + 12);
	put_unaligned_le32(mode, packet + 16);
	ret = mt6797_wifi_ahb_cmd53_write(boot->hif, MT6797_WIFI_MCR_WTDR1,
					  packet, sizeof(packet));
	return ret ?: mt6797_wifi_wait_config_ack(boot, sequence);
}

static int mt6797_wifi_download_idlm(void *context, unsigned int index)
{
	struct mt6797_wifi_boot *boot = context;
	const struct mt6797_wifi_section *section;
	const u8 *image;
	u32 sent = 0;

	if (index >= 2 || !boot->image_packet)
		return -EINVAL;
	section = &boot->firmware->section[index];
	image = boot->firmware->blob->data + section->offset;
	while (sent < section->length) {
		const u8 *packet;
		size_t chunk = min_t(u32, MT6797_WIFI_IMAGE_PACKET_SIZE,
				     section->length - sent);
		size_t length = MT6797_WIFI_HIF_HEADER_SIZE + chunk;
		size_t transfer = length < MT6797_WIFI_AHB_BLOCK_SIZE ?
			ALIGN(length, sizeof(u32)) :
			ALIGN(length, MT6797_WIFI_AHB_BLOCK_SIZE);
		int ret;

		if (transfer > MT6797_WIFI_AHB_MAX_TRANSFER_SIZE)
			return -EOVERFLOW;
		memset(boot->image_packet, 0, transfer);
		mt6797_wifi_init_header(boot->image_packet, length,
					MT6797_WIFI_PQ_PDA, 0, 0);
		memcpy(boot->image_packet + MT6797_WIFI_HIF_HEADER_SIZE,
		       image + sent, chunk);
		packet = boot->image_packet;
		ret = mt6797_wifi_ahb_cmd53_write(boot->hif,
						  MT6797_WIFI_MCR_WTDR1, packet,
						  transfer);
		if (ret)
			return ret;
		sent += chunk;
	}
	return 0;
}

static int mt6797_wifi_download_emi(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	struct resource window;
	u8 __iomem *emi;
	unsigned int index;
	int close_ret;
	int ret;

	ret = mt6797_consys_wifi_emi_window_open(boot->lease, &window);
	if (ret)
		return ret;
	if (resource_size(&window) != MT6797_WIFI_EMI_SIZE) {
		ret = -EINVAL;
		goto close;
	}
	emi = ioremap(window.start, resource_size(&window));
	if (!emi) {
		ret = -ENOMEM;
		goto close;
	}
	for (index = 2; index < MT6797_WIFI_SECTION_COUNT; index++) {
		const struct mt6797_wifi_section *section =
			&boot->firmware->section[index];
		u32 offset = section->destination & GENMASK(19, 0);

		if (section->length > MT6797_WIFI_EMI_SIZE ||
		    offset > MT6797_WIFI_EMI_SIZE - section->length) {
			ret = -EFBIG;
			break;
		}
		memcpy_toio(emi + offset,
			    boot->firmware->blob->data + section->offset,
			    section->length);
	}
	/* Complete every image write before revoking AP access to region 18. */
	wmb();
	iounmap(emi);
close:
	close_ret = mt6797_consys_wifi_emi_window_close(boot->lease);
	return ret ?: close_ret;
}

static int mt6797_wifi_start_firmware(void *context, u8 sequence)
{
	struct mt6797_wifi_boot *boot = context;
	u8 packet[16] = { };

	mt6797_wifi_init_header(packet, sizeof(packet), MT6797_WIFI_PQ_CMD,
				MT6797_WIFI_CMD_WIFI_START, sequence);
	return mt6797_wifi_ahb_cmd53_write(boot->hif, MT6797_WIFI_MCR_WTDR1,
					  packet, sizeof(packet));
}

static int mt6797_wifi_wait_ready(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	unsigned int poll;
	u32 wcir;
	int ret;

	for (poll = 0; poll < MT6797_WIFI_READY_POLLS; poll++) {
		ret = mt6797_wifi_reg_read(boot, MT6797_WIFI_MCR_WCIR, &wcir);
		if (ret)
			return ret;
		if (FIELD_GET(GENMASK(15, 0), wcir) != MT6797_WIFI_CHIP_ID ||
		    FIELD_GET(GENMASK(19, 16), wcir))
			return -EPROTO;
		if (wcir & BIT(21)) {
			dev_info(boot->dev, "WLAN firmware ready (WCIR %#08x)\n",
				 wcir);
			return 0;
		}
		usleep_range(10000, 11000);
	}
	return -ETIMEDOUT;
}

static int mt6797_wifi_clear_status(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	unsigned int queue;
	u32 status;
	u32 released;
	int ret;

	ret = mt6797_wifi_reg_read(boot, MT6797_WIFI_MCR_WHISR, &status);
	if (ret || !(status & BIT(0)))
		return ret;
	for (queue = 0; queue < 8; queue++) {
		u32 address = MT6797_WIFI_MCR_WTQCR0 + queue * sizeof(u32);

		ret = mt6797_wifi_reg_read(boot, address, &released);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6797_wifi_restore_block_size(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	u8 low = 0;
	u8 high = 0;
	int cleanup = 0;
	int ret;

	ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0,
					  MT6797_WIFI_FBR1_BLOCK_SIZE + 1,
					  boot->block_high_before);
	if (ret)
		cleanup = ret;
	ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0,
					  MT6797_WIFI_FBR1_BLOCK_SIZE,
					  boot->block_low_before);
	if (!cleanup && ret)
		cleanup = ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE, &low);
	if (!cleanup && ret)
		cleanup = ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0,
					 MT6797_WIFI_FBR1_BLOCK_SIZE + 1, &high);
	if (!cleanup && ret)
		cleanup = ret;
	if (!cleanup && (low != boot->block_low_before ||
			 high != boot->block_high_before))
		cleanup = -EUCLEAN;
	return cleanup;
}

static int mt6797_wifi_restore_function(void *context)
{
	struct mt6797_wifi_boot *boot = context;
	u8 ioex = 0;
	int ret;

	ret = mt6797_wifi_ahb_cmd52_write(boot->hif, 0,
					  MT6797_WIFI_CCCR_IOEX,
					  boot->ioex_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(boot->hif, 0, MT6797_WIFI_CCCR_IOEX,
					 &ioex);
	if (ret)
		return ret;
	return ioex == boot->ioex_before ? 0 : -EUCLEAN;
}

static void mt6797_wifi_fence(void *context)
{
	struct mt6797_wifi_boot *boot = context;

	mt6797_consys_wifi_fence(boot->lease);
}

static void mt6797_wifi_boot_init_ops(struct mt6797_wifi_boot *boot,
				      struct mt6797_wifi_boot_ops *ops)
{
	*ops = (struct mt6797_wifi_boot_ops) {
		.enable_function = mt6797_wifi_enable_function,
		.set_block_size = mt6797_wifi_set_block_size,
		.driver_own = mt6797_wifi_driver_own,
		.disable_interrupts = mt6797_wifi_disable_interrupts,
		.download_config = mt6797_wifi_download_config,
		.download_idlm = mt6797_wifi_download_idlm,
		.download_emi = mt6797_wifi_download_emi,
		.start = mt6797_wifi_start_firmware,
		.wait_ready = mt6797_wifi_wait_ready,
		.clear_status = mt6797_wifi_clear_status,
		.restore_block_size = mt6797_wifi_restore_block_size,
		.restore_function = mt6797_wifi_restore_function,
		.fence = mt6797_wifi_fence,
		.context = boot,
	};
}

int mt6797_wifi_boot_start(struct mt6797_wifi_boot *boot, struct device *dev,
			   struct mt6797_wifi_ahb_hif *hif,
			   const struct mt6797_wifi_firmware *firmware)
{
	struct mt6797_wifi_boot_ops ops;
	int release_ret;
	int ret;

	if (!boot || !dev || !hif || !firmware || !firmware->blob)
		return -EINVAL;
	memset(boot, 0, sizeof(*boot));
	boot->dev = dev;
	boot->hif = hif;
	boot->firmware = firmware;
	boot->image_packet = kmalloc(MT6797_WIFI_AHB_MAX_TRANSFER_SIZE,
				     GFP_KERNEL);
	if (!boot->image_packet)
		return -ENOMEM;
	boot->lease = mt6797_consys_wifi_acquire(dev);
	if (IS_ERR(boot->lease)) {
		ret = PTR_ERR(boot->lease);
		boot->lease = NULL;
		goto free_packet;
	}

	mt6797_wifi_boot_init_ops(boot, &ops);
	ret = mt6797_wifi_boot_transaction_run(&ops, &boot->transaction);
	kfree(boot->image_packet);
	boot->image_packet = NULL;
	if (ret) {
		release_ret = mt6797_consys_wifi_release(boot->lease);
		boot->lease = NULL;
		return ret ?: release_ret;
	}
	boot->online = true;
	return 0;

free_packet:
	kfree(boot->image_packet);
	boot->image_packet = NULL;
	return ret;
}

int mt6797_wifi_boot_stop(struct mt6797_wifi_boot *boot)
{
	struct mt6797_wifi_boot_ops ops;
	int release_ret;
	int ret;

	if (!boot || !boot->online || !boot->lease)
		return -EINVAL;
	mt6797_wifi_boot_init_ops(boot, &ops);
	ret = mt6797_wifi_boot_transaction_stop(&ops, &boot->transaction);
	boot->online = false;
	release_ret = mt6797_consys_wifi_release(boot->lease);
	boot->lease = NULL;
	return ret ?: release_ret;
}
