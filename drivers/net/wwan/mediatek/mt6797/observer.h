/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_OBSERVER_H
#define __MT6797_MD1_OBSERVER_H

#include <linux/string.h>
#include <linux/types.h>

#define MT6797_MD1_CLDMA_QUEUES	8

struct mt6797_md1_snapshot {
	u32 tx_start[MT6797_MD1_CLDMA_QUEUES];
	u32 rx_start[MT6797_MD1_CLDMA_QUEUES];
	u32 tx_active;
	u32 rx_active;
	u32 ccif_busy;
	u32 ccif_pending;
};

enum mt6797_md1_inherited_state {
	MT6797_MD1_INHERITED_POWER_OFF,
	MT6797_MD1_INHERITED_CLOCK_GATED,
	MT6797_MD1_INHERITED_QUIESCENT,
	MT6797_MD1_INHERITED_CCIF_PENDING,
	MT6797_MD1_INHERITED_FOREIGN_RINGS,
	MT6797_MD1_INHERITED_UNSTABLE,
};

static inline bool
mt6797_md1_snapshot_equal(const struct mt6797_md1_snapshot *first,
			  const struct mt6797_md1_snapshot *second)
{
	return !memcmp(first, second, sizeof(*first));
}

static inline enum mt6797_md1_inherited_state
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

#endif /* __MT6797_MD1_OBSERVER_H */
