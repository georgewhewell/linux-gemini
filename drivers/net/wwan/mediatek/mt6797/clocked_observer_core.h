/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_CLOCKED_OBSERVER_CORE_H
#define __MT6797_MD1_CLOCKED_OBSERVER_CORE_H

#include <linux/errno.h>
#include <linux/kernel.h>

#include "observer.h"

#define MT6797_MD1_OBSERVER_CLOCKS	3

struct mt6797_md1_clocked_ops {
	int (*enable_clock)(void *context, unsigned int index);
	void (*disable_clock)(void *context, unsigned int index);
	int (*take_snapshot)(void *context, unsigned int index,
			     struct mt6797_md1_snapshot *snapshot);
	int (*verify_unwind)(void *context);
};

static inline int
mt6797_md1_clocked_observe(const struct mt6797_md1_clocked_ops *ops,
			   void *context,
			   enum mt6797_md1_inherited_state *state)
{
	struct mt6797_md1_snapshot snapshots[2] = {};
	enum mt6797_md1_inherited_state observed_state;
	unsigned int enabled = 0;
	unsigned int i;
	int unwind_ret;
	int ret = 0;

	if (!ops || !context || !state || !ops->enable_clock ||
	    !ops->disable_clock || !ops->take_snapshot ||
	    !ops->verify_unwind)
		return -EINVAL;

	for (i = 0; i < MT6797_MD1_OBSERVER_CLOCKS; i++) {
		ret = ops->enable_clock(context, i);
		if (ret)
			goto unwind;
		enabled++;
	}

	for (i = 0; i < ARRAY_SIZE(snapshots); i++) {
		ret = ops->take_snapshot(context, i, &snapshots[i]);
		if (ret)
			goto unwind;
	}

	if (!mt6797_md1_snapshot_equal(&snapshots[0], &snapshots[1])) {
		ret = -EAGAIN;
		goto unwind;
	}
	observed_state = mt6797_md1_snapshot_classify(&snapshots[1]);

unwind:
	while (enabled)
		ops->disable_clock(context, --enabled);
	unwind_ret = ops->verify_unwind(context);
	if (unwind_ret)
		return unwind_ret;
	if (!ret)
		*state = observed_state;

	return ret;
}

static inline const char *
mt6797_md1_clocked_state_name(enum mt6797_md1_inherited_state state)
{
	switch (state) {
	case MT6797_MD1_INHERITED_QUIESCENT:
		return "quiescent-unconfigured";
	case MT6797_MD1_INHERITED_CCIF_PENDING:
		return "ccif-pending";
	case MT6797_MD1_INHERITED_FOREIGN_RINGS:
		return "foreign-rings";
	default:
		return "invalid";
	}
}

#endif /* __MT6797_MD1_CLOCKED_OBSERVER_CORE_H */
