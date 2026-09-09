/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_TX_DIAG_H
#define __MT6797_WIFI_TX_DIAG_H

#include <linux/types.h>

#define MT6797_RADIO_TX_DIAG_BUCKETS 10

struct mt6797_radio_tx_diag_span {
	u64 count;
	u64 total_ns;
	u64 max_ns;
	u64 buckets[MT6797_RADIO_TX_DIAG_BUCKETS];
};

/* All fields are protected by radio->lock; lifetime is one netdev registration. */
struct mt6797_radio_tx_diag {
	u64 lifetime_start_ns;
	u64 connection_generation;
	u64 enqueued;
	u64 dequeued;
	u64 purged;
	u64 rejected;
	u64 busy;
	u64 hif_submitted;
	u64 send_failed;
	u64 send_cancelled;
	u64 send_disconnected;
	u64 send_timed_out;
	u64 credit_wait_attempts;
	u64 credit_wait_packets;
	u64 credit_wait_wakeups;
	u64 credit_wait_timeouts;
	u64 worker_started_ns;
	u64 send_started_ns;
	u64 credit_wait_started_ns;
	u32 queue_depth;
	u32 queue_high_water;
	bool worker_active;
	bool send_active;
	bool credit_wait_active;
	struct mt6797_radio_tx_diag_span queue_wait;
	struct mt6797_radio_tx_diag_span send_duration;
	struct mt6797_radio_tx_diag_span credit_wait;
	struct mt6797_radio_tx_diag_span worker_duration;
};

#endif
