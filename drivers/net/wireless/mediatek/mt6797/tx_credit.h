/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_TX_CREDIT_H
#define __MT6797_WIFI_TX_CREDIT_H

#include <linux/types.h>

#define MT6797_WIFI_TX_CREDIT_QUEUES 16U
#define MT6797_WIFI_TX_CREDIT_DATA 4U /* Non-QoS TC5 -> LMAC AC4. */
#define MT6797_WIFI_TX_CREDIT_CPU 15U /* TC4 -> MCU Q1. */
#define MT6797_WIFI_TX_CREDIT_FFA 14U
#define MT6797_WIFI_TX_PAGE_SIZE 128U

/* Caller serializes acquire/release. Conservative initial admission: one
 * maximum Ethernet frame (13 pages) on each used queue, below GEN3 runtime
 * default TC5=13/TC4=26 pages. This does not program hardware allocation.
 */
struct mt6797_wifi_tx_credit {
	u64 available;
	/* FFA pages reused before their delayed per-queue TX-done report. */
	u64 preused[MT6797_WIFI_TX_CREDIT_QUEUES];
	u16 limit[MT6797_WIFI_TX_CREDIT_QUEUES];
	u16 free[MT6797_WIFI_TX_CREDIT_QUEUES];
	u16 done[MT6797_WIFI_TX_CREDIT_QUEUES];
	unsigned int next_queue;
};

void mt6797_wifi_tx_credit_init(struct mt6797_wifi_tx_credit *credit);
int mt6797_wifi_tx_credit_acquire(struct mt6797_wifi_tx_credit *credit,
	unsigned int queue, size_t packet_length);
/* GEN3 nicTxCalculateResource with extra-TX-done: FFA proves returned pages
 * even when a per-queue count is delayed. Track each early reuse in preused,
 * then consume the delayed queue report before considering another refund.
 * Queue reports alone never supply memory; admission remains bounded by limit.
 */
int mt6797_wifi_tx_credit_release(struct mt6797_wifi_tx_credit *credit,
	const u16 released[MT6797_WIFI_TX_CREDIT_QUEUES]);

#endif
