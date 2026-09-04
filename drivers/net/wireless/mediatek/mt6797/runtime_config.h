/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_RUNTIME_CONFIG_H
#define __MT6797_WIFI_RUNTIME_CONFIG_H

#include <linux/types.h>

struct mt6797_wifi_ahb_hif;
struct mt6797_wifi_boot;

struct mt6797_wifi_config_transaction {
	bool dirty;
	bool configured;
};

struct mt6797_wifi_config_ops {
	int (*precheck)(void *context);
	int (*send)(void *context);
	int (*teardown)(void *context);
	void (*fence)(void *context);
	void *context;
};

struct mt6797_wifi_runtime_config {
	struct mt6797_wifi_boot *boot;
	struct mt6797_wifi_ahb_hif *hif;
	struct mt6797_wifi_config_transaction transaction;
};

int
mt6797_wifi_config_transaction_run(const struct mt6797_wifi_config_ops *ops,
				   struct mt6797_wifi_config_transaction *transaction);
int mt6797_wifi_runtime_configure(struct mt6797_wifi_runtime_config *config,
				  struct mt6797_wifi_boot *boot);

#endif /* __MT6797_WIFI_RUNTIME_CONFIG_H */
