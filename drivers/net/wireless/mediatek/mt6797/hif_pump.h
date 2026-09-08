/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_HIF_PUMP_H
#define __MT6797_WIFI_HIF_PUMP_H

#include <linux/types.h>

#include "protocol.h"

#define MT6797_WIFI_HIF_RX_BUDGET	32U
#define MT6797_WIFI_HIF_TX_QUEUES	16U

struct mt6797_wifi_hif_pump_stats {
	u64 interrupts;
	u64 tx_released[MT6797_WIFI_HIF_TX_QUEUES];
	u64 rx_packets;
	u64 rx_dropped;
	u64 events;
	u64 ieee80211_frames;
	u64 ethernet_frames;
	u8 last_event_id;
	u8 last_event_sequence;
};

struct mt6797_wifi_hif_pump_ops {
	int (*read_status)(void *context, u32 *status);
	int (*read_tx_released)(void *context,
				u16 released[MT6797_WIFI_HIF_TX_QUEUES]);
	int (*read_lengths)(void *context, u16 lengths[2]);
	int (*read_packet)(void *context, unsigned int port, u16 length,
			   const u8 **packet);
	/* Called synchronously; event and its payload must not be retained. */
	int (*deliver_event)(void *context,
			     const struct mt6797_wifi_event_view *event);
	/* Called synchronously; frame and its payload must not be retained. */
	int (*deliver_frame)(void *context,
			     const struct mt6797_wifi_rx_view *frame);
	/* Diagnostic only; never log payloads or hardware report address fields. */
	void (*reject_packet)(void *context, const u8 *packet, size_t length, int error);
	bool (*expired)(void *context);
	/* Only startup, before opening interrupt gates, may find no status. */
	bool allow_empty_status;
	void *context;
};

int mt6797_wifi_hif_pump_account_tx(
	struct mt6797_wifi_hif_pump_stats *stats,
	const u16 released[MT6797_WIFI_HIF_TX_QUEUES]);
int mt6797_wifi_hif_pump_dispatch(
	const struct mt6797_wifi_hif_pump_ops *ops,
	struct mt6797_wifi_hif_pump_stats *stats,
	const struct mt6797_wifi_rx_view *frame);
int mt6797_wifi_hif_pump_run(const struct mt6797_wifi_hif_pump_ops *ops,
			     struct mt6797_wifi_hif_pump_stats *stats,
			     u32 *acknowledged_status);

#endif /* __MT6797_WIFI_HIF_PUMP_H */
