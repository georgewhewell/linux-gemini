/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_A72_PLATFORM_PROVIDER_CLOCK_OBSERVER_INTERNAL_H
#define __MT6797_A72_PLATFORM_PROVIDER_CLOCK_OBSERVER_INTERNAL_H

#include <linux/mt6797-a72-provider.h>
#include <linux/soc/mediatek/mt6797-a72-platform-state.h>
#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

struct device;

enum mt6797_a72_ppc_failure_stage {
	MT6797_A72_PPC_FAILURE_NONE,
	MT6797_A72_PPC_FAILURE_DEPENDENCY,
	MT6797_A72_PPC_FAILURE_PLATFORM,
	MT6797_A72_PPC_FAILURE_PROVIDER,
	MT6797_A72_PPC_FAILURE_BEFORE_CLOCK,
};

struct mt6797_a72_platform_provider_clock_snapshot {
	struct mt6797_a72_platform_state platform;
	struct mt6797_a72_provider_snapshot provider;
	struct mt6797_dvfsp_clock_readback clock;
	int clock_ret;
	bool clock_returned;
	bool after_checkpoint;
	bool valid;
};

struct mt6797_a72_platform_provider_clock_ops {
	int (*platform)(void *context, struct device *dev,
			struct mt6797_a72_platform_state *snapshot,
			struct mt6797_a72_platform_state_failure *failure);
	int (*provider)(void *context,
			struct mt6797_a72_provider_snapshot *snapshot);
	bool (*checkpoint)(void *context, unsigned int checkpoint);
	int (*clock)(void *context, struct device *dev,
		     struct mt6797_dvfsp_clock_readback *snapshot);
};

int mt6797_a72_ppc_capture(struct device *platform, struct device *provider,
			   struct device *clock,
	const struct mt6797_a72_platform_provider_clock_ops *ops, void *context,
	struct mt6797_a72_platform_provider_clock_snapshot *snapshot,
	enum mt6797_a72_ppc_failure_stage *failure_stage,
	struct mt6797_a72_platform_state_failure *platform_failure);

#endif /* __MT6797_A72_PLATFORM_PROVIDER_CLOCK_OBSERVER_INTERNAL_H */
