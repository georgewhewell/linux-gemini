/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_CAPABILITY_H
#define __MT6797_WIFI_CAPABILITY_H

#include <linux/completion.h>
#include <linux/types.h>

#define MT6797_WIFI_CAPABILITY_LENGTH	124U

struct device;
struct mt6797_wifi_ahb_hif;
struct mt6797_wifi_boot;
struct mt6797_wifi_hif_irq;

struct mt6797_wifi_capability {
	u16 product_id;
	u16 firmware_version;
	u16 driver_version;
	u8 hw_5ghz_disabled;
	u8 eeprom_used;
	u8 mac_address[6];
	u8 rf_version;
	u8 phy_version;
	u8 rf_cal_failed;
	u8 bb_cal_failed;
	u32 feature_flag0;
	u32 feature_flag1;
	u32 compile_flag0;
	u32 compile_flag1;
};

struct mt6797_wifi_capability_session {
	struct mt6797_wifi_hif_irq *owner;
	struct mt6797_wifi_boot *boot;
	struct completion completion;
	struct mt6797_wifi_capability capability;
	u8 response[MT6797_WIFI_CAPABILITY_LENGTH + sizeof(u32)];
	u32 tx_released[8];
	u32 whcr_before;
	u32 whisr;
	u32 wrplr;
	int result;
	unsigned int irq_count;
	u8 sequence;
	bool armed;
	bool done;
	bool whcr_changed;
};

struct mt6797_wifi_capability_ops {
	int (*precheck)(void *context);
	int (*arm)(void *context);
	void (*enable_linux_irq)(void *context);
	int (*enable_device_irq)(void *context);
	void (*unmask_hif_irq)(void *context);
	int (*send)(void *context);
	int (*wait)(void *context);
	void (*disarm)(void *context);
	void (*mask_hif_irq)(void *context);
	int (*disable_device_irq)(void *context);
	void (*disable_linux_irq)(void *context);
	void (*fence)(void *context);
	void *context;
};

int mt6797_wifi_capability_command_build(u8 *command, size_t length,
					 u8 sequence);
int mt6797_wifi_capability_event_parse(const u8 *event, size_t length,
				       u8 sequence,
				       struct mt6797_wifi_capability *capability);
int
mt6797_wifi_capability_transaction(const struct mt6797_wifi_capability_ops *ops);

int mt6797_wifi_capability_session_init(
	struct mt6797_wifi_capability_session *session,
	struct mt6797_wifi_hif_irq *owner);
int mt6797_wifi_capability_query(struct mt6797_wifi_capability_session *session,
				 struct mt6797_wifi_boot *boot,
				 struct mt6797_wifi_capability *capability);

#endif /* __MT6797_WIFI_CAPABILITY_H */
