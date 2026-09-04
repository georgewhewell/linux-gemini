/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_SCAN_HIF_H
#define __MT6797_WIFI_SCAN_HIF_H

#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "scan.h"
#include "scan_target.h"

struct mt6797_wifi_hif_irq;

/*
 * Driver-private backend boundary; a NULL backend selects the real AHB HIF.
 * now() is also called from synchronous RX delivery and must not sleep.
 */
struct mt6797_wifi_scan_hif_backend {
	int (*send)(void *context, u32 address, const u8 *packet,
		    size_t length);
	ktime_t (*now)(void *context);
	unsigned long (*wait)(void *context, struct completion *completion,
			      unsigned long timeout);
	void (*terminal)(void *context, int reason);
	void *context;
};

struct mt6797_wifi_scan_hif_request {
	struct mt6797_wifi_scan_target_key key;
	u64 max_age_ms;
	unsigned int timeout_ms;
	u8 command_sequence;
	u8 cancel_sequence;
	u8 scan_sequence;
	u8 band;
	u8 channel;
};

struct mt6797_wifi_scan_hif_result {
	/* Copy remains valid only while target_owner accepts its generation/cookie. */
	struct mt6797_wifi_scan_target target;
	bool scan_done;
	bool target_seen;
	bool cancel_sent;
};

/*
 * One caller-owned passive-scan lifetime. The lifecycle owner serializes
 * run/close. IRQ callbacks nest this lock below the runtime sink lock and
 * retain no borrowed event, frame, or complete-packet pointer.
 */
struct mt6797_wifi_scan_hif {
	struct mt6797_wifi_hif_irq *irq;
	struct mt6797_wifi_scan_hif_backend backend;
	struct mt6797_wifi_scan_transaction transaction;
	struct mt6797_wifi_scan_target_owner target_owner;
	spinlock_t lock; /* Serializes IRQ delivery against timeout and disarm. */
	struct completion completion;
	int terminal_reason;
	u8 expected_band;
	u8 expected_channel;
	bool initialized;
	bool attempted;
	bool armed;
	bool dirty;
	bool sent;
	bool scan_done;
	bool target_seen;
	bool cancel_dirty;
	bool cancel_sent;
	bool target_valid;
};

int mt6797_wifi_scan_hif_init(
	struct mt6797_wifi_scan_hif *scan, struct mt6797_wifi_hif_irq *irq,
	const struct mt6797_wifi_scan_hif_backend *backend);
int mt6797_wifi_scan_hif_run(
	struct mt6797_wifi_scan_hif *scan,
	const struct mt6797_wifi_scan_hif_request *request,
	struct mt6797_wifi_scan_hif_result *result);
int mt6797_wifi_scan_hif_close(struct mt6797_wifi_scan_hif *scan);

#endif /* __MT6797_WIFI_SCAN_HIF_H */
