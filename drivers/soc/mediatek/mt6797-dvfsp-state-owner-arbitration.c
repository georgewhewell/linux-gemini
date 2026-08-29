// SPDX-License-Identifier: GPL-2.0-only
/*
 * Serialize the calibrated state source with the real transition owner.
 *
 * The caller supplies the external clock/rail lock and generation source.
 * This wrapper performs no hardware operation and never registers a provider;
 * it only rejects source snapshots that overlap a transition or regress in
 * generation.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-state-owner-source.h>

static int mt6797_dvfsp_arbitration_check(
	const struct mt6797_dvfsp_state_owner_arbitration *arbitration)
{
	if (!arbitration || !arbitration->initialized ||
	    arbitration->faulted ||
	    !arbitration->source || !arbitration->source->initialized ||
	    !arbitration->ops || arbitration->ops->abi !=
	    MT6797_DVFSP_STATE_OWNER_ARBITRATION_ABI ||
	    !arbitration->ops->lock || !arbitration->ops->unlock ||
	    !arbitration->ops->read_generation)
		return -EINVAL;

	return 0;
}

static int mt6797_dvfsp_arbitration_fault(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	enum mt6797_dvfsp_state_invalidation_reason reason, int error)
{
	if (arbitration) {
		arbitration->faulted = true;
		if (arbitration->source)
			mt6797_dvfsp_state_owner_source_invalidate(
				arbitration->source, reason);
	}

	return error;
}

static int mt6797_dvfsp_arbitration_read_generation(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	u64 *generation)
{
	int ret;

	ret = arbitration->ops->read_generation(arbitration->context,
						 generation);
	if (ret)
		return mt6797_dvfsp_arbitration_fault(arbitration,
			MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION, ret);
	if (!*generation)
		return mt6797_dvfsp_arbitration_fault(arbitration,
			MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION, -EPROTO);
	if (arbitration->last_generation &&
	    *generation < arbitration->last_generation)
		return mt6797_dvfsp_arbitration_fault(arbitration,
			MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION, -EPROTO);

	return 0;
}

static int mt6797_dvfsp_arbitration_begin(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	u64 *generation)
{
	int ret;

	ret = mt6797_dvfsp_arbitration_check(arbitration);
	if (ret)
		return ret;
	ret = arbitration->ops->lock(arbitration->context);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_arbitration_read_generation(arbitration,
							generation);
	if (ret)
		arbitration->ops->unlock(arbitration->context);

	return ret;
}

static int mt6797_dvfsp_arbitration_end(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	u64 generation)
{
	u64 after;
	int ret;

	ret = mt6797_dvfsp_arbitration_read_generation(arbitration, &after);
	if (!ret && after != generation)
		ret = mt6797_dvfsp_arbitration_fault(arbitration,
			MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION, -EAGAIN);
	if (!ret)
		arbitration->last_generation = after;
	arbitration->ops->unlock(arbitration->context);

	return ret;
}

int mt6797_dvfsp_state_owner_arbitration_init(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_source *source,
	const struct mt6797_dvfsp_state_owner_arbitration_ops *ops,
	void *context)
{
	if (!arbitration || !source || !source->initialized || !ops ||
	    ops->abi != MT6797_DVFSP_STATE_OWNER_ARBITRATION_ABI ||
	    !ops->lock || !ops->unlock || !ops->read_generation)
		return -EINVAL;

	memset(arbitration, 0, sizeof(*arbitration));
	arbitration->source = source;
	arbitration->ops = ops;
	arbitration->context = context;
	mutex_init(&arbitration->lifecycle_lock);
	arbitration->initialized = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_init);

int mt6797_dvfsp_state_owner_arbitration_snapshot(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	u64 generation;
	int ret;
	int finish_ret;

	if (!snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_arbitration_begin(arbitration, &generation);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_owner_source_snapshot(arbitration->source,
							snapshot);
	if (!ret && snapshot->generation != generation)
		ret = mt6797_dvfsp_arbitration_fault(arbitration,
			MT6797_DVFSP_STATE_INVALID_CLOCK_TRANSITION, -EAGAIN);
	finish_ret = mt6797_dvfsp_arbitration_end(arbitration, generation);
	if (!ret)
		ret = finish_ret;

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_snapshot);

int mt6797_dvfsp_state_owner_arbitration_validate(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	struct mt6797_dvfsp_state_snapshot fresh;
	int ret;

	if (!snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_state_owner_arbitration_snapshot(arbitration,
							&fresh);
	if (ret)
		return ret;

	return memcmp(&fresh, snapshot, sizeof(fresh)) ? -EAGAIN : 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_validate);

int mt6797_dvfsp_state_owner_arbitration_identify(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	u64 generation;
	int ret;
	int finish_ret;

	if (!identity)
		return -EINVAL;
	ret = mt6797_dvfsp_arbitration_begin(arbitration, &generation);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_owner_source_identify(arbitration->source,
							identity);
	finish_ret = mt6797_dvfsp_arbitration_end(arbitration, generation);

	if (!ret)
		ret = finish_ret;
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_identify);

void mt6797_dvfsp_state_owner_arbitration_invalidate(
	struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	 enum mt6797_dvfsp_state_invalidation_reason reason)
{
	if (!arbitration || !arbitration->source)
		return;
	arbitration->faulted = true;
	mt6797_dvfsp_state_owner_source_invalidate(arbitration->source, reason);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_invalidate);

static int mt6797_dvfsp_state_owner_arbitration_identify_cb(
	void *context, struct mt6797_dvfsp_state_owner_identity *identity)
{
	return mt6797_dvfsp_state_owner_arbitration_identify(context, identity);
}

static int mt6797_dvfsp_state_owner_arbitration_snapshot_cb(
	void *context, struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return mt6797_dvfsp_state_owner_arbitration_snapshot(context, snapshot);
}

static int mt6797_dvfsp_state_owner_arbitration_validate_cb(
	void *context, const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return mt6797_dvfsp_state_owner_arbitration_validate(context, snapshot);
}

static void mt6797_dvfsp_state_owner_arbitration_invalidate_cb(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	mt6797_dvfsp_state_owner_arbitration_invalidate(context, reason);
}

static int mt6797_dvfsp_state_owner_arbitration_hold_cb(
	void *context, const struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_hold *hold)
{
	struct mt6797_dvfsp_state_owner_arbitration *arbitration = context;
	u64 generation;
	int ret;

	if (!arbitration || !snapshot || !hold ||
	    !arbitration->initialized)
		return -EINVAL;

	mutex_lock(&arbitration->lifecycle_lock);
	ret = mt6797_dvfsp_arbitration_check(arbitration);
	if (ret)
		goto out_unlock;
	if (arbitration->hold_active) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = mt6797_dvfsp_arbitration_begin(arbitration, &generation);
	if (ret)
		goto out_unlock;
	if (!snapshot->generation || snapshot->generation != generation ||
	    !snapshot->cluster_mask || !arbitration->source->owner_handle) {
		arbitration->ops->unlock(arbitration->context);
		ret = -EAGAIN;
		goto out_unlock;
	}

	memset(hold, 0, sizeof(*hold));
	hold->generation = generation;
	hold->cluster_mask = snapshot->cluster_mask;
	hold->owner_handle = arbitration->source->owner_handle;
	arbitration->hold_active = true;
	arbitration->hold_generation = generation;
	arbitration->hold_cluster_mask = hold->cluster_mask;
	arbitration->hold_owner_handle = hold->owner_handle;

out_unlock:
	mutex_unlock(&arbitration->lifecycle_lock);
	return ret;
}

static int mt6797_dvfsp_state_owner_arbitration_release_cb(
	void *context, const struct mt6797_dvfsp_state_hold *hold)
{
	struct mt6797_dvfsp_state_owner_arbitration *arbitration = context;
	u64 generation;
	int ret;

	if (!arbitration || !hold || !arbitration->initialized)
		return -EINVAL;

	mutex_lock(&arbitration->lifecycle_lock);
	if (!arbitration->hold_active) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (hold->generation != arbitration->hold_generation ||
	    hold->cluster_mask != arbitration->hold_cluster_mask ||
	    hold->reserved || hold->owner_handle !=
	    arbitration->hold_owner_handle) {
		ret = -EPERM;
		goto out_unlock;
	}
	generation = arbitration->hold_generation;
	arbitration->hold_active = false;
	arbitration->hold_generation = 0;
	arbitration->hold_cluster_mask = 0;
	arbitration->hold_owner_handle = 0;
	mutex_unlock(&arbitration->lifecycle_lock);

	ret = mt6797_dvfsp_arbitration_end(arbitration, generation);
	if (!ret && arbitration->faulted)
		ret = -EHOSTDOWN;
	return ret;

out_unlock:
	mutex_unlock(&arbitration->lifecycle_lock);
	return ret;
}

int mt6797_dvfsp_state_owner_arbitration_owner_ops_init(
	const struct mt6797_dvfsp_state_owner_arbitration *arbitration,
	struct mt6797_dvfsp_state_owner_ops *ops)
{
	if (mt6797_dvfsp_arbitration_check(arbitration) || !ops)
		return -EINVAL;

	memset(ops, 0, sizeof(*ops));
	ops->identify = mt6797_dvfsp_state_owner_arbitration_identify_cb;
	ops->snapshot = mt6797_dvfsp_state_owner_arbitration_snapshot_cb;
	ops->validate = mt6797_dvfsp_state_owner_arbitration_validate_cb;
	ops->hold = mt6797_dvfsp_state_owner_arbitration_hold_cb;
	ops->release = mt6797_dvfsp_state_owner_arbitration_release_cb;
	ops->invalidate = mt6797_dvfsp_state_owner_arbitration_invalidate_cb;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_owner_ops_init);

int mt6797_dvfsp_state_owner_arbitration_register(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_arbitration *arbitration)
{
	struct mt6797_dvfsp_state_snapshot snapshot;
	int ret;

	if (!handoff || !arbitration || !arbitration->initialized)
		return -EINVAL;

	mutex_lock(&arbitration->lifecycle_lock);
	ret = mt6797_dvfsp_arbitration_check(arbitration);
	if (ret)
		goto out_unlock;
	if (arbitration->registered) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = mt6797_dvfsp_state_owner_arbitration_owner_ops_init(
		arbitration, &arbitration->owner_ops);
	if (ret)
		goto out_unlock;
	memset(&snapshot, 0, sizeof(snapshot));
	ret = mt6797_dvfsp_state_owner_arbitration_snapshot(
		arbitration, &snapshot);
	if (ret)
		goto out_clear_ops;
	ret = mt6797_dvfsp_state_owner_arbitration_validate(
		arbitration, &snapshot);
	if (ret)
		goto out_clear_ops;
	ret = mt6797_dvfsp_handoff_state_owner_register(
		handoff, &arbitration->owner_ops, arbitration);
	if (ret)
		goto out_clear_ops;

	arbitration->handoff = handoff;
	arbitration->registered = true;
	goto out_unlock;

out_clear_ops:
	memset(&arbitration->owner_ops, 0, sizeof(arbitration->owner_ops));

out_unlock:
	mutex_unlock(&arbitration->lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_register);

int mt6797_dvfsp_state_owner_arbitration_unregister(
	struct mt6797_dvfsp_handoff *handoff,
	struct mt6797_dvfsp_state_owner_arbitration *arbitration)
{
	int ret;

	if (!handoff || !arbitration || !arbitration->initialized)
		return -EINVAL;

	mutex_lock(&arbitration->lifecycle_lock);
	if (!arbitration->registered || arbitration->handoff != handoff) {
		ret = -ENOENT;
		goto out_unlock;
	}
	ret = mt6797_dvfsp_handoff_state_owner_unregister(
		handoff, &arbitration->owner_ops, arbitration);
	if (!ret) {
		arbitration->handoff = NULL;
		arbitration->registered = false;
		memset(&arbitration->owner_ops, 0,
		       sizeof(arbitration->owner_ops));
	}

out_unlock:
	mutex_unlock(&arbitration->lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_arbitration_unregister);

MODULE_DESCRIPTION("MT6797 calibrated state-owner transition arbitration");
MODULE_LICENSE("GPL");
