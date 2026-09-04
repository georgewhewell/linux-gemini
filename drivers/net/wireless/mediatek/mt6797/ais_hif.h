/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_HIF_H
#define __MT6797_WIFI_AIS_HIF_H

#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/types.h>

#include "ais_runtime.h"

struct mt6797_wifi_capability;
struct mt6797_wifi_hif_irq;
struct mt6797_wifi_runtime_config;
struct mt6797_wifi_scan_target;
struct mt6797_wifi_scan_target_owner;

/* Driver-private backend boundary; a NULL backend selects the real AHB HIF. */
struct mt6797_wifi_ais_hif_backend {
	int (*send)(void *context, u32 address, const u8 *packet,
		    size_t length);
	ktime_t (*now)(void *context);
	unsigned long (*wait)(void *context, struct completion *completion,
			      unsigned long timeout);
	void (*terminal)(void *context, int reason);
	void *context;
};

struct mt6797_wifi_ais_hif_request {
	struct mt6797_wifi_scan_target_owner *target_owner;
	const struct mt6797_wifi_scan_target *target;
	u64 now_ms;
	unsigned int response_timeout_ms;
	u8 bss_index;
	u8 own_mac_index;
};

/*
 * This object is explicitly invoked and never started from probe. The caller
 * owns its lifetime and the scan-target owner for the entire transaction.
 */
struct mt6797_wifi_ais_hif {
	struct mt6797_wifi_hif_irq *irq;
	struct mt6797_wifi_runtime_config *config;
	const struct mt6797_wifi_capability *capability;
	struct mt6797_wifi_ais_hif_backend backend;
	struct mt6797_wifi_ais_runtime runtime;
	u64 target_generation;
	u64 target_cookie;
	bool initialized;
};

int mt6797_wifi_ais_hif_init(
	struct mt6797_wifi_ais_hif *ais, struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability,
	unsigned int first_command_sequence,
	unsigned int first_channel_token,
	const struct mt6797_wifi_ais_hif_backend *backend);
int mt6797_wifi_ais_hif_start(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_request *request);
int mt6797_wifi_ais_hif_stop(struct mt6797_wifi_ais_hif *ais);

#endif /* __MT6797_WIFI_AIS_HIF_H */
