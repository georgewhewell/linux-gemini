/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RUNTIME_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RUNTIME_H

#include <linux/errno.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

#define MT6797_DVFSP_RUNTIME_ABI		1
#define MT6797_DVFSP_RUNTIME_EVENT_COUNT	8

/* These lifecycle points are recovered from the vendor DVFS owner. */
enum mt6797_dvfsp_runtime_event_type {
	MT6797_DVFSP_RUNTIME_CPU_ONLINE,
	MT6797_DVFSP_RUNTIME_CPU_DOWN_PREPARE,
	MT6797_DVFSP_RUNTIME_CPU_DOWN_FAILED,
	MT6797_DVFSP_RUNTIME_PM_SUSPEND_PREPARE,
	MT6797_DVFSP_RUNTIME_PM_POST_SUSPEND,
	MT6797_DVFSP_RUNTIME_CLOCK_TRANSITION,
	MT6797_DVFSP_RUNTIME_RAIL_TRANSITION,
	MT6797_DVFSP_RUNTIME_PCM_FAULT,
};

struct mt6797_dvfsp_runtime_event {
	u32 abi;
	u32 type;
	u64 sequence;
	u64 generation;
};

struct mt6797_dvfsp_runtime_ledger {
	u32 abi;
	u32 reserved;
	u64 last_sequence;
	u64 invalidated_generation;
};

int mt6797_dvfsp_runtime_event_apply(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger);

#define MT6797_DVFSP_RUNTIME_BINDING_ABI	1
#define MT6797_DVFSP_RUNTIME_CPU_NONE		(~0U)

struct mt6797_dvfsp_runtime_source_ops {
	u32 abi;
	int (*event)(void *context, u32 type, unsigned int cpu,
			     struct mt6797_dvfsp_runtime_event *event);
};

struct mt6797_dvfsp_runtime_binding {
	u32 abi;
	void *private_data;
};

void mt6797_dvfsp_runtime_binding_init(
	struct mt6797_dvfsp_runtime_binding *binding);
int mt6797_dvfsp_runtime_binding_register(
	struct mt6797_dvfsp_runtime_binding *binding,
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_runtime_source_ops *source_ops,
	void *source_context);
int mt6797_dvfsp_runtime_binding_unregister(
	struct mt6797_dvfsp_runtime_binding *binding);

int mt6797_dvfsp_runtime_event_apply_invalidator(
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger,
	void (*invalidate)(void *context,
				enum mt6797_dvfsp_state_invalidation_reason reason),
	void *context);
#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_RUNTIME_H */
