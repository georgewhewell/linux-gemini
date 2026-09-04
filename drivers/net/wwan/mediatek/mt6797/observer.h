/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_OBSERVER_H
#define __MT6797_MD1_OBSERVER_H

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

bool mt6797_md1_snapshot_equal(const struct mt6797_md1_snapshot *first,
			       const struct mt6797_md1_snapshot *second);
enum mt6797_md1_inherited_state
mt6797_md1_snapshot_classify(const struct mt6797_md1_snapshot *snapshot);

#endif /* __MT6797_MD1_OBSERVER_H */
