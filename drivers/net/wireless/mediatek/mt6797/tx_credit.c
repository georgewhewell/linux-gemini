// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 nicTxGetPageCount/nicTxCalculateResource with delayed TX-done tracking. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "tx_credit.h"

void mt6797_wifi_tx_credit_init(struct mt6797_wifi_tx_credit *credit)
{
	memset(credit, 0, sizeof(*credit));
	credit->limit[MT6797_WIFI_TX_CREDIT_DATA] = 13;
	credit->limit[MT6797_WIFI_TX_CREDIT_CPU] = 13;
	memcpy(credit->free, credit->limit, sizeof(credit->free));
}

int mt6797_wifi_tx_credit_acquire(struct mt6797_wifi_tx_credit *credit,
	unsigned int queue, size_t packet_length)
{
	size_t pages;
	int ret;

	if (!credit || queue >= MT6797_WIFI_TX_CREDIT_QUEUES || !packet_length ||
	    !credit->limit[queue])
		return -EINVAL;
	pages = (packet_length - 1) / MT6797_WIFI_TX_PAGE_SIZE + 1;
	if (pages > credit->limit[queue])
		return -EMSGSIZE;
	if (pages > credit->free[queue]) {
		const u16 released[MT6797_WIFI_TX_CREDIT_QUEUES] = {};

		/* An earlier IRQ may have returned FFA while this queue was idle. */
		ret = mt6797_wifi_tx_credit_release(credit, released);
		if (ret)
			return ret;
		if (pages > credit->free[queue])
			return -EAGAIN;
	}
	credit->free[queue] -= pages;
	return 0;
}

int mt6797_wifi_tx_credit_release(struct mt6797_wifi_tx_credit *credit,
	const u16 released[MT6797_WIFI_TX_CREDIT_QUEUES])
{
	struct mt6797_wifi_tx_credit next;
	unsigned int queue, offset, start;
	u64 available;

	if (!credit || !released)
		return -EINVAL;
	next = *credit;
	if (check_add_overflow(credit->available,
		(u64)released[MT6797_WIFI_TX_CREDIT_FFA], &available))
		return -EOVERFLOW;
	for (queue = 0; queue < MT6797_WIFI_TX_CREDIT_QUEUES; queue++) {
		u16 debt = credit->limit[queue] - credit->free[queue];
		u16 delayed = min_t(u64, next.preused[queue], released[queue]);
		u16 reported = released[queue] - delayed;

		next.preused[queue] -= delayed;
		/* Never retain a queue report for a reservation not yet made. */
		next.done[queue] = min_t(unsigned int, debt,
			(unsigned int)next.done[queue] + reported);
	}
	/* First pair known queue completions with returned memory. */
	start = next.next_queue;
	for (offset = 0; offset < MT6797_WIFI_TX_CREDIT_QUEUES; offset++) {
		u16 refund;

		queue = (start + offset) % MT6797_WIFI_TX_CREDIT_QUEUES;
		refund = min_t(u64, available, next.done[queue]);
		next.done[queue] -= refund;
		next.free[queue] += refund;
		available -= refund;
		if (refund)
			next.next_queue = (queue + 1) % MT6797_WIFI_TX_CREDIT_QUEUES;
	}
	/* FFA may precede batched LMAC TX-done. Reuse only observed free pages,
	 * and remember their queue debt so a later report cannot refund twice.
	 */
	start = next.next_queue;
	for (offset = 0; offset < MT6797_WIFI_TX_CREDIT_QUEUES; offset++) {
		u16 refund;

		queue = (start + offset) % MT6797_WIFI_TX_CREDIT_QUEUES;
		refund = min_t(u64, available, next.limit[queue] - next.free[queue]);
		if (check_add_overflow(next.preused[queue], (u64)refund,
			&next.preused[queue]))
			return -EOVERFLOW;
		next.free[queue] += refund;
		available -= refund;
		if (refund)
			next.next_queue = (queue + 1) % MT6797_WIFI_TX_CREDIT_QUEUES;
	}
	next.available = available;
	*credit = next;
	return 0;
}
