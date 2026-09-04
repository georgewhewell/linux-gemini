/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_BOOT_H
#define __MT6797_WIFI_BOOT_H

#include <linux/types.h>

struct device;
struct mt6797_consys_wifi_lease;
struct mt6797_wifi_ahb_hif;
struct mt6797_wifi_firmware;

struct mt6797_wifi_boot_transaction {
	bool function_touched;
	bool block_size_touched;
	bool firmware_dirty;
	bool ready;
};

struct mt6797_wifi_boot_ops {
	int (*enable_function)(void *context, bool *touched);
	int (*set_block_size)(void *context, bool *touched);
	int (*driver_own)(void *context);
	int (*disable_interrupts)(void *context);
	int (*download_config)(void *context, unsigned int section,
			       bool reset, u8 sequence);
	int (*download_idlm)(void *context, unsigned int section);
	int (*download_emi)(void *context);
	int (*start)(void *context, u8 sequence);
	int (*wait_ready)(void *context);
	int (*clear_status)(void *context);
	int (*restore_block_size)(void *context);
	int (*restore_function)(void *context);
	void (*fence)(void *context);
	void *context;
};

struct mt6797_wifi_boot {
	struct device *dev;
	struct mt6797_wifi_ahb_hif *hif;
	const struct mt6797_wifi_firmware *firmware;
	const struct mt6797_consys_wifi_lease *lease;
	struct mt6797_wifi_boot_transaction transaction;
	u8 ioex_before;
	u8 block_low_before;
	u8 block_high_before;
	u8 *image_packet;
	bool online;
};

int mt6797_wifi_boot_transaction_run(const struct mt6797_wifi_boot_ops *ops,
				     struct mt6797_wifi_boot_transaction *transaction);
int mt6797_wifi_boot_transaction_stop(const struct mt6797_wifi_boot_ops *ops,
				      struct mt6797_wifi_boot_transaction *transaction);

int mt6797_wifi_boot_start(struct mt6797_wifi_boot *boot, struct device *dev,
			   struct mt6797_wifi_ahb_hif *hif,
			   const struct mt6797_wifi_firmware *firmware);
int mt6797_wifi_boot_stop(struct mt6797_wifi_boot *boot);

#endif /* __MT6797_WIFI_BOOT_H */
