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
	u16 limit[MT6797_WIFI_TX_CREDIT_QUEUES];
	u16 free[MT6797_WIFI_TX_CREDIT_QUEUES];
	u16 done[MT6797_WIFI_TX_CREDIT_QUEUES];
};

void mt6797_wifi_tx_credit_init(struct mt6797_wifi_tx_credit *credit);
int mt6797_wifi_tx_credit_acquire(struct mt6797_wifi_tx_credit *credit,
	unsigned int queue, size_t packet_length);
/* Per-queue TX done and FFA may arrive in different IRQs. Neither alone refunds
 * an outstanding reservation. Unknown/pre-runtime completions cannot inflate
 * a queue beyond its initial admission limit.
 */
int mt6797_wifi_tx_credit_release(struct mt6797_wifi_tx_credit *credit,
	const u16 released[MT6797_WIFI_TX_CREDIT_QUEUES]);

#endif
