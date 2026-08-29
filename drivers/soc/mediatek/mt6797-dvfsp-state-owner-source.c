// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bind source-backed calibration and live state to one protected snapshot.
 *
 * The source callbacks are deliberately explicit: a caller cannot provide a
 * calibration table without naming the silicon identity, and live state must
 * echo the same provenance and owner handles. The borrowed device tuple is
 * read through the already-reviewed transport bridge; this file does not
 * register a platform driver or perform a transition.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-owner-source.h>

static bool mt6797_dvfsp_owner_provenance_equal(
	const struct mt6797_dvfsp_state_provenance *left,
	const struct mt6797_dvfsp_state_provenance *right)
{
	return left->abi == right->abi &&
		left->source_mask == right->source_mask &&
		left->variant_id == right->variant_id &&
		left->table_epoch == right->table_epoch &&
		left->calibration_handle == right->calibration_handle;
}

static int mt6797_dvfsp_owner_identity_check(
	const struct mt6797_dvfsp_state_owner_source_identity *identity)
{
	if (!identity || identity->abi !=
	    MT6797_DVFSP_STATE_OWNER_SOURCE_ABI ||
	    !identity->variant_id || !identity->table_epoch ||
	    !identity->calibration_handle)
		return -EPROTO;

	return 0;
}

static void mt6797_dvfsp_owner_identity_provenance(
	const struct mt6797_dvfsp_state_owner_source_identity *identity,
	struct mt6797_dvfsp_state_provenance *provenance)
{
	memset(provenance, 0, sizeof(*provenance));
	provenance->abi = MT6797_DVFSP_STATE_PROVENANCE_ABI;
	provenance->source_mask = MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL;
	provenance->variant_id = identity->variant_id;
	provenance->table_epoch = identity->table_epoch;
	provenance->calibration_handle = identity->calibration_handle;
}

static int mt6797_dvfsp_owner_read_clock(
	void *context, struct mt6797_dvfsp_clock_readback *readback)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;

	if (!owner)
		return -EINVAL;
	return mt6797_dvfsp_state_source_backend_read_clock(
		&owner->devices, readback);
}

static int mt6797_dvfsp_owner_read_big(
	void *context, struct mt6797_bigidvfs_readback *readback)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;

	if (!owner)
		return -EINVAL;
	return mt6797_dvfsp_state_source_backend_read_big(
		&owner->devices, readback);
}

static int mt6797_dvfsp_owner_read_eem(
	void *context, struct mt6797_eem_readback *readback)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;

	if (!owner)
		return -EINVAL;
	return mt6797_dvfsp_state_source_backend_read_eem(
		&owner->devices, readback);
}

static int mt6797_dvfsp_owner_read_ptp(
	void *context, struct mt6797_dvfsp_ptp_handoff *handoff)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;

	if (!owner)
		return -EINVAL;
	return mt6797_dvfsp_state_source_backend_read_ptp(
		&owner->devices, handoff);
}

static int mt6797_dvfsp_owner_read_ppm(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_ppm_snapshot *snapshot)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;
	int ret;

	if (!owner || !ptp_state || !snapshot || !owner->ppm_owner_ops ||
	    owner->ppm_owner_ops->abi != MT6797_DVFSP_PPM_OWNER_ABI ||
	    !owner->ppm_owner_ops->lock || !owner->ppm_owner_ops->unlock ||
	    !owner->ppm_owner_ops->snapshot)
		return -EINVAL;

	owner->ppm_policy_valid = false;
	memset(snapshot, 0, sizeof(*snapshot));
	memset(&owner->ppm_policy, 0, sizeof(owner->ppm_policy));
	ret = owner->ppm_owner_ops->lock(owner->ppm_owner_context);
	if (ret)
		return ret;
	ret = owner->ppm_owner_ops->snapshot(owner->ppm_owner_context,
						ptp_state, snapshot,
						&owner->ppm_policy);
	owner->ppm_owner_ops->unlock(owner->ppm_owner_context);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_ppm_snapshot_validate(snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_ppm_policy_validate(&owner->ppm_policy);
	if (ret)
		return ret;
	if (snapshot->table_epoch != owner->ppm_policy.table_epoch)
		return -EAGAIN;
	owner->ppm_policy_valid = true;
	return 0;
}

static int mt6797_dvfsp_owner_fill_calibration(
	void *context, const struct mt6797_dvfsp_ptp_handoff *handoff,
	const struct mt6797_dvfsp_ptp_state *ptp_state,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy,
	const struct mt6797_eem_readback *readback,
	struct mt6797_eem_calibration_input *input)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;
	struct mt6797_dvfsp_state_owner_source_identity identity;
	struct mt6797_dvfsp_state_provenance provenance;
	int ret;

	if (!owner || !handoff || !ptp_state || !ppm || !policy ||
	    !readback || !input ||
	    !owner->source_ops || !owner->source_ops->read_identity ||
	    !owner->source_ops->fill_calibration || !owner->ppm_policy_valid)
		return -EINVAL;

	memset(&identity, 0, sizeof(identity));
	ret = owner->source_ops->read_identity(owner->source_context,
					       ptp_state, &identity);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_owner_identity_check(&identity);
	if (ret)
		return ret;

	ret = owner->source_ops->fill_calibration(owner->source_context,
			handoff, ptp_state, ppm, policy, readback, &identity, input);
	if (ret)
		return ret;

	/* The binding owns provenance; the row callback may not replace it. */
	if (input->provenance.abi || input->provenance.source_mask ||
	    input->provenance.variant_id || input->provenance.table_epoch ||
	    input->provenance.calibration_handle)
		return -EPROTO;
	ret = mt6797_dvfsp_ppm_policy_validate(policy);
	if (ret)
		return ret;
	if (memcmp(policy, &owner->ppm_policy, sizeof(*policy)))
		return -EAGAIN;

	mt6797_dvfsp_owner_identity_provenance(&identity, &provenance);
	if (owner->provenance.abi &&
	    !mt6797_dvfsp_owner_provenance_equal(&owner->provenance,
						 &provenance))
		return -EAGAIN;
	owner->provenance = provenance;
	input->provenance = provenance;

	return 0;
}

static int mt6797_dvfsp_owner_fill_live(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_state_source_live *live)
{
	struct mt6797_dvfsp_state_owner_source *owner = context;
	int ret;

	if (!owner || !clock || !calibration || !cspm || !ppm || !live ||
	    !owner->source_ops ||
	    !owner->source_ops->fill_live)
		return -EINVAL;

	ret = owner->source_ops->fill_live(owner->source_context, clock,
					   calibration, cspm, ppm, live);
	if (ret)
		return ret;
	if (!live->generation || live->owner_handle != owner->owner_handle ||
	    !calibration->source_generation ||
	    live->generation != calibration->source_generation ||
	    live->transition_handle != owner->transition_handle ||
	    !mt6797_dvfsp_owner_provenance_equal(&live->provenance,
						 &owner->provenance))
		return -EPROTO;

	return 0;
}

static int mt6797_dvfsp_owner_source_snapshot_locked(
	struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret;

	if (!owner->initialized || owner->invalidated)
		return -EAGAIN;

	ret = mt6797_dvfsp_state_source_snapshot(&owner->adapter_ops, owner,
						 snapshot);
	if (ret)
		return ret;
	if (snapshot->owner_handle != owner->owner_handle ||
	    snapshot->transition_handle != owner->transition_handle ||
	    !mt6797_dvfsp_owner_provenance_equal(&snapshot->provenance,
						 &owner->provenance))
		return -EAGAIN;

	return 0;
}

int mt6797_dvfsp_state_owner_source_init(
	struct mt6797_dvfsp_state_owner_source *owner,
	const struct mt6797_dvfsp_state_source_devices *devices,
	const struct mt6797_dvfsp_state_owner_source_ops *source_ops,
	void *source_context,
	const struct mt6797_dvfsp_ppm_owner_ops *ppm_owner_ops,
	void *ppm_owner_context, u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_state_source_ops backend_ops;
	int ret;

	if (!owner || !devices || !source_ops || source_ops->abi !=
	    MT6797_DVFSP_STATE_OWNER_SOURCE_ABI ||
	    !source_ops->read_identity || !ppm_owner_ops ||
	    ppm_owner_ops->abi != MT6797_DVFSP_PPM_OWNER_ABI ||
	    !ppm_owner_ops->lock || !ppm_owner_ops->unlock ||
	    !ppm_owner_ops->snapshot ||
	    !source_ops->fill_calibration ||
	    !source_ops->fill_live || !source_ops->invalidate ||
	    !owner_handle || !transition_handle)
		return -EINVAL;

	ret = mt6797_dvfsp_state_source_backend_ops_init(devices,
							&backend_ops);
	if (ret)
		return ret;

	memset(owner, 0, sizeof(*owner));
	mutex_init(&owner->transition_lock);
	owner->devices = *devices;
	owner->source_ops = source_ops;
	owner->source_context = source_context;
	owner->ppm_owner_ops = ppm_owner_ops;
	owner->ppm_owner_context = ppm_owner_context;
	owner->owner_handle = owner_handle;
	owner->transition_handle = transition_handle;
	owner->adapter_ops = backend_ops;
	owner->adapter_ops.read_clock = mt6797_dvfsp_owner_read_clock;
	owner->adapter_ops.read_big = mt6797_dvfsp_owner_read_big;
	owner->adapter_ops.read_eem = mt6797_dvfsp_owner_read_eem;
	owner->adapter_ops.read_ptp = mt6797_dvfsp_owner_read_ptp;
	owner->adapter_ops.read_ppm = mt6797_dvfsp_owner_read_ppm;
	owner->adapter_ops.fill_calibration =
		mt6797_dvfsp_owner_fill_calibration;
	owner->adapter_ops.fill_live = mt6797_dvfsp_owner_fill_live;
	owner->initialized = true;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_init);

int mt6797_dvfsp_state_owner_source_snapshot(
	struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret;

	if (!owner || !snapshot)
		return -EINVAL;
	ret = mutex_lock_interruptible(&owner->transition_lock);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_owner_source_snapshot_locked(owner, snapshot);
	mutex_unlock(&owner->transition_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_snapshot);

int mt6797_dvfsp_state_owner_source_validate(
	struct mt6797_dvfsp_state_owner_source *owner,
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	struct mt6797_dvfsp_state_snapshot fresh;
	int ret;

	if (!owner || !snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_state_owner_source_snapshot(owner, &fresh);
	if (ret)
		return ret;
	return memcmp(&fresh, snapshot, sizeof(fresh)) ? -EAGAIN : 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_validate);

int mt6797_dvfsp_state_owner_source_identify(
	struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	struct mt6797_dvfsp_ptp_handoff handoff;
	struct mt6797_dvfsp_ptp_state ptp_state;
	struct mt6797_dvfsp_state_owner_source_identity source_identity;
	int ret;

	if (!owner || !identity)
		return -EINVAL;
	ret = mutex_lock_interruptible(&owner->transition_lock);
	if (ret)
		return ret;
	if (!owner->initialized || owner->invalidated) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	memset(&handoff, 0, sizeof(handoff));
	ret = mt6797_dvfsp_owner_read_ptp(owner, &handoff);
	if (ret)
		goto out_unlock;
	ret = mt6797_dvfsp_ptp_decode(&handoff, &ptp_state);
	if (ret)
		goto out_unlock;
	memset(&source_identity, 0, sizeof(source_identity));
	ret = owner->source_ops->read_identity(owner->source_context,
						&ptp_state, &source_identity);
	if (ret)
		goto out_unlock;
	ret = mt6797_dvfsp_owner_identity_check(&source_identity);
	if (ret)
		goto out_unlock;

	{
		struct mt6797_dvfsp_state_provenance provenance;

		mt6797_dvfsp_owner_identity_provenance(&source_identity,
						&provenance);
		if (owner->provenance.abi &&
		    !mt6797_dvfsp_owner_provenance_equal(&owner->provenance,
							 &provenance)) {
			ret = -EAGAIN;
			goto out_unlock;
		}
		owner->provenance = provenance;
	}
	memset(identity, 0, sizeof(*identity));
	identity->abi = MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI;
	identity->resource_mask = MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL;
	identity->cpu_pll_backend =
		MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP;
	identity->big_cluster_backend =
		MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC;
	identity->owner_handle = owner->owner_handle;
	identity->transition_handle = owner->transition_handle;
	identity->provenance = owner->provenance;

out_unlock:
	mutex_unlock(&owner->transition_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_identify);

void mt6797_dvfsp_state_owner_source_invalidate(
	struct mt6797_dvfsp_state_owner_source *owner,
	enum mt6797_dvfsp_state_invalidation_reason reason)
{
	bool notify = false;

	if (!owner)
		return;
	mutex_lock(&owner->transition_lock);
	if (!owner->invalidated) {
		owner->invalidated = true;
		notify = true;
	}
	mutex_unlock(&owner->transition_lock);
	if (notify && owner->source_ops && owner->source_ops->invalidate)
		owner->source_ops->invalidate(owner->source_context, reason);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_invalidate);

static int mt6797_dvfsp_state_owner_source_identify_cb(
	void *context, struct mt6797_dvfsp_state_owner_identity *identity)
{
	return mt6797_dvfsp_state_owner_source_identify(context, identity);
}

static int mt6797_dvfsp_state_owner_source_snapshot_cb(
	void *context, struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return mt6797_dvfsp_state_owner_source_snapshot(context, snapshot);
}

static int mt6797_dvfsp_state_owner_source_validate_cb(
	void *context, const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	return mt6797_dvfsp_state_owner_source_validate(context, snapshot);
}

static void mt6797_dvfsp_state_owner_source_invalidate_cb(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	mt6797_dvfsp_state_owner_source_invalidate(context, reason);
}

int mt6797_dvfsp_state_owner_source_owner_ops_init(
	const struct mt6797_dvfsp_state_owner_source *owner,
	struct mt6797_dvfsp_state_owner_ops *ops)
{
	if (!owner || !ops || !owner->initialized)
		return -EINVAL;

	memset(ops, 0, sizeof(*ops));
	ops->identify = mt6797_dvfsp_state_owner_source_identify_cb;
	ops->snapshot = mt6797_dvfsp_state_owner_source_snapshot_cb;
	ops->validate = mt6797_dvfsp_state_owner_source_validate_cb;
	ops->invalidate = mt6797_dvfsp_state_owner_source_invalidate_cb;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_owner_source_owner_ops_init);

MODULE_DESCRIPTION("MT6797 calibrated state-owner source binding");
MODULE_LICENSE("GPL");
