/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6797 WLAN GEN3 AHB SDIO-like portal contract. */

#ifndef __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_H
#define __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb-codec.h>

#define MT6797_WIFI_AHB_HIF_BASE		0x180f0000ULL
#define MT6797_WIFI_AHB_HIF_PORTAL_SIZE	0x1100ULL
#define MT6797_WIFI_AHB_CMD_SETUP		0x0000
#define MT6797_WIFI_AHB_CMD52_DATA		0x0004
#define MT6797_WIFI_AHB_CMD53_DATA		0x1000
#define MT6797_WIFI_AHB_WCIR_OFFSET		0x0000

#define MT6797_WIFI_AHB_SDIO_FUNC_WIFI	1
#define MT6797_WIFI_AHB_SDIO_BLOCK_SIZE	512
#define MT6797_WIFI_AHB_SDIO_CCCR_IOEX	0x0002
#define MT6797_WIFI_AHB_SDIO_CCCR_IORX	0x0003
#define MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE	0x0110

struct mt6797_wifi_ahb_wcir_guard {
	bool full_postpatch_session;
	bool wifi_paldo_held;
	bool hif_owner;
	bool hif_mapping;
};

struct device;
struct regulator;
struct mt6797_btif_stp_bootstrap_full_wmt_lease;

struct mt6797_wifi_ahb_wcir {
	u32 raw;
	u16 chip_id;
	u8 revision;
	bool por_indicator;
	bool wlan_ready;
};

int mt6797_wifi_ahb_wcir_decode(u32 raw, struct mt6797_wifi_ahb_wcir *wcir);
bool mt6797_wifi_ahb_wcir_read_permitted(const struct mt6797_wifi_ahb_wcir_guard *guard);
bool mt6797_wifi_ahb_wcir_enabled(struct device *owner);
int mt6797_wifi_ahb_wcir_observe(struct device *owner,
				 const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
				 struct regulator *wifi_paldo,
				 struct mt6797_wifi_ahb_wcir *wcir);

/* W1 is an explicitly gated, one-shot firmware download/start/READY proof. */
bool mt6797_wifi_ahb_w1_firmware_enabled(struct device *owner);
int mt6797_wifi_ahb_w1_firmware_validate(const u8 *data, size_t size,
					 size_t expected_size,
					 const u8 *expected_digest);
struct mt6797_wifi_ahb_w1_section {
	u32 offset;
	u32 length;
	u32 destination;
	u8 key_index;
	bool encrypted;
};

#define MT6797_WIFI_AHB_W1_SECTION_COUNT 4

struct mt6797_wifi_ahb_w1_image {
	struct mt6797_wifi_ahb_w1_section section[MT6797_WIFI_AHB_W1_SECTION_COUNT];
};

int mt6797_wifi_ahb_w1_firmware_parse(const u8 *data, size_t size,
				       struct mt6797_wifi_ahb_w1_image *image);
int mt6797_wifi_ahb_w1_idlm_transfer_account(
		const struct mt6797_wifi_ahb_w1_image *image,
		u32 *frame_count, size_t *transfer_bytes);

/* W2 is the exact normal NIC-capability command/event exchange after W1. */
#define MT6797_WIFI_AHB_W2_CAPABILITY_LENGTH	124U

struct mt6797_wifi_ahb_w2_capability {
	u16 product_id;
	u16 firmware_version;
	u16 driver_version;
	u8 mac_address[6];
	u32 feature_flag0;
	u32 feature_flag1;
	u32 compile_flag0;
	u32 compile_flag1;
};

int mt6797_wifi_ahb_w2_capability_command_build(u8 *command, size_t length,
					 u8 sequence);
int mt6797_wifi_ahb_w2_capability_event_parse(const u8 *event, size_t length,
					 u8 sequence,
					 struct mt6797_wifi_ahb_w2_capability *capability);
int mt6797_wifi_ahb_w1_firmware_preflight(struct device *owner,
		  const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
		  struct regulator *bt_paldo, struct regulator *wifi_paldo);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_H */
