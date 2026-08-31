// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime invalidation conversion boundary for the MT6797 DVFSP owner.
 *
 * The future owner supplies serialized lifecycle events. This helper only
 * validates their identity and maps them to the existing state-owner fault
 * reasons; it does not subscribe to a notifier or access hardware.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-runtime.h>

static int mt6797_dvfsp_runtime_reason(u32 type,
		enum mt6797_dvfsp_state_invalidation_reason *reason)
{
	if (!reason)
		return -EINVAL;

	switch (type) {
	case MT6797_DVFSP_RUNTIME_CPU_ONLINE:
	case MT6797_DVFSP_RUNTIME_CPU_DOWN_PREPARE:
	case MT6797_DVFSP_RUNTIME_CPU_DOWN_FAILED:
	case MT6797_DVFSP_RUNTIME_CLOCK_TRANSITION:
		*reason = MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION;
		return 0;
	case MT6797_DVFSP_RUNTIME_PM_SUSPEND_PREPARE:
	case MT6797_DVFSP_RUNTIME_PM_POST_SUSPEND:
		*reason = MT6797_DVFSP_STATE_INVALID_SUSPEND_RESUME;
		return 0;
	case MT6797_DVFSP_RUNTIME_RAIL_TRANSITION:
		*reason = MT6797_DVFSP_STATE_INVALID_RAIL_TRANSITION;
		return 0;
	case MT6797_DVFSP_RUNTIME_PCM_FAULT:
		*reason = MT6797_DVFSP_STATE_INVALID_PCM_FAULT;
		return 0;
	default:
		return -EPROTO;
	}
}

static int mt6797_dvfsp_runtime_event_check(
	const struct mt6797_dvfsp_runtime_event *event)
{
	if (!event || event->abi != MT6797_DVFSP_RUNTIME_ABI ||
		!event->sequence || !event->generation ||
		event->type >= MT6797_DVFSP_RUNTIME_EVENT_COUNT)
		return -EPROTO;

	return 0;
}

int mt6797_dvfsp_runtime_event_apply(
	struct mt6797_dvfsp_handoff *handoff,
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger)
{
	enum mt6797_dvfsp_state_invalidation_reason reason;
	int ret;

	if (!handoff || !ledger)
		return -EINVAL;
	ret = mt6797_dvfsp_runtime_event_check(event);
	if (ret)
		return ret;
	if ((ledger->abi && ledger->abi != MT6797_DVFSP_RUNTIME_ABI) ||
		ledger->reserved)
		return -EPROTO;
	if (ledger->last_sequence >= event->sequence ||
		ledger->invalidated_generation >= event->generation)
		return -EALREADY;
	ret = mt6797_dvfsp_runtime_reason(event->type, &reason);
	if (ret)
		return ret;

	/* The caller serializes this ledger with the owner transition lock. */
	mt6797_dvfsp_handoff_state_invalidate(handoff, reason);
	ledger->abi = MT6797_DVFSP_RUNTIME_ABI;
	ledger->last_sequence = event->sequence;
	ledger->invalidated_generation = event->generation;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_runtime_event_apply);

int mt6797_dvfsp_runtime_event_apply_invalidator(
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger,
	void (*invalidate)(void *context,
				enum mt6797_dvfsp_state_invalidation_reason reason),
	void *context)
{
	enum mt6797_dvfsp_state_invalidation_reason reason;
	int ret;

	if (!ledger || !invalidate)
		return -EINVAL;
	ret = mt6797_dvfsp_runtime_event_check(event);
	if (ret)
		return ret;
	if ((ledger->abi && ledger->abi != MT6797_DVFSP_RUNTIME_ABI) ||
	    ledger->reserved)
		return -EPROTO;
	if (ledger->last_sequence >= event->sequence ||
	    ledger->invalidated_generation >= event->generation)
		return -EALREADY;
	ret = mt6797_dvfsp_runtime_reason(event->type, &reason);
	if (ret)
		return ret;

	invalidate(context, reason);
	ledger->abi = MT6797_DVFSP_RUNTIME_ABI;
	ledger->last_sequence = event->sequence;
	ledger->invalidated_generation = event->generation;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_runtime_event_apply_invalidator);

MODULE_DESCRIPTION("MT6797 DVFSP runtime invalidation conversion");
MODULE_LICENSE("GPL");
