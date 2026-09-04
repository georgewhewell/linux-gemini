// SPDX-License-Identifier: GPL-2.0-only
/* Pure classification helpers for inherited MT6797 MD1 transport state. */

#include <linux/string.h>

#include "observer.h"

bool mt6797_md1_snapshot_equal(const struct mt6797_md1_snapshot *first,
			       const struct mt6797_md1_snapshot *second)
{
	return !memcmp(first, second, sizeof(*first));
}

enum mt6797_md1_inherited_state
mt6797_md1_snapshot_classify(const struct mt6797_md1_snapshot *snapshot)
{
	unsigned int i;

	if (snapshot->ccif_busy || snapshot->ccif_pending)
		return MT6797_MD1_INHERITED_CCIF_PENDING;
	if (snapshot->tx_active || snapshot->rx_active)
		return MT6797_MD1_INHERITED_FOREIGN_RINGS;

	for (i = 0; i < MT6797_MD1_CLDMA_QUEUES; i++) {
		if (snapshot->tx_start[i] || snapshot->rx_start[i])
			return MT6797_MD1_INHERITED_FOREIGN_RINGS;
	}

	return MT6797_MD1_INHERITED_QUIESCENT;
}
