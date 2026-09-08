// SPDX-License-Identifier: GPL-2.0-only
/* GEN3 nicTxGetPageCount/nicTxCalculateResource without speculative extra-done. */
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

	if (!credit || queue >= MT6797_WIFI_TX_CREDIT_QUEUES || !packet_length ||
	    !credit->limit[queue])
		return -EINVAL;
	pages = (packet_length - 1) / MT6797_WIFI_TX_PAGE_SIZE + 1;
	if (pages > credit->limit[queue])
		return -EMSGSIZE;
	if (pages > credit->free[queue])
		return -EAGAIN;
	credit->free[queue] -= pages;
	return 0;
}

int mt6797_wifi_tx_credit_release(struct mt6797_wifi_tx_credit *credit,
	const u16 released[MT6797_WIFI_TX_CREDIT_QUEUES])
{
	u64 available;
	unsigned int queue;

	if (!credit || !released)
		return -EINVAL;
	if (check_add_overflow(credit->available,
		(u64)released[MT6797_WIFI_TX_CREDIT_FFA], &available))
		return -EOVERFLOW;
	for (queue = 0; queue < MT6797_WIFI_TX_CREDIT_QUEUES; queue++) {
		u16 debt = credit->limit[queue] - credit->free[queue];
		u16 refund;

		/* Never carry unrelated completion counts into a later reservation. */
		credit->done[queue] = min_t(unsigned int, debt,
			(unsigned int)credit->done[queue] + released[queue]);
		refund = min_t(u64, available, credit->done[queue]);
		credit->done[queue] -= refund;
		credit->free[queue] += refund;
		available -= refund;
	}
	credit->available = available;
	return 0;
}
