// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bind the calibrated state source to the resource-only MT6797 owner.
 *
 * All silicon, PPM, calibration, live-rail, and clock-generation behavior
 * remains an explicit external callback contract. This object only composes
 * the existing fail-closed source and arbitration lifecycles.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-calibrated-provider.h>

static int mt6797_dvfsp_calibrated_provider_read_identity(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_state_owner_source_identity *identity)
{
	struct mt6797_dvfsp_calibrated_provider *provider = context;

	if (!provider)
		return -EINVAL;
	return mt6797_dvfsp_identity_source_read(&provider->identity_source,
						ptp_state, identity);
}

static int mt6797_dvfsp_calibrated_provider_fill_calibration(
	void *context, const struct mt6797_dvfsp_ptp_handoff *handoff,
	const struct mt6797_dvfsp_ptp_state *ptp_state,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy,
	const struct mt6797_eem_readback *readback,
	const struct mt6797_dvfsp_state_owner_source_identity *identity,
	struct mt6797_eem_calibration_input *input)
{
	struct mt6797_dvfsp_calibrated_provider *provider = context;

	if (!provider || !provider->source_ops ||
	    provider->source_ops->abi != MT6797_DVFSP_CALIBRATED_SOURCE_ABI ||
    !provider->source_ops->fill_calibration)
		return -EINVAL;
	return provider->source_ops->fill_calibration(provider->source_context,
			handoff, ptp_state, ppm, policy, readback, identity, input);
}

static int mt6797_dvfsp_calibrated_provider_fill_live(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_state_source_live *live)
{
	struct mt6797_dvfsp_calibrated_provider *provider = context;

	if (!provider || !clock || !calibration || !cspm || !ppm || !live)
		return -EINVAL;
	return mt6797_dvfsp_live_source_fill(&provider->live_source, clock,
						calibration, cspm, ppm, live);
}

static void mt6797_dvfsp_calibrated_provider_invalidate_cb(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	struct mt6797_dvfsp_calibrated_provider *provider = context;

	if (provider && provider->source_ops && provider->source_ops->invalidate)
		provider->source_ops->invalidate(provider->source_context, reason);
}

static int mt6797_dvfsp_calibrated_provider_check(
	const struct mt6797_dvfsp_calibrated_provider *provider)
{
	if (!provider || !provider->initialized ||
	    !provider->resource_owner)
		return -EINVAL;

	return 0;
}

int mt6797_dvfsp_calibrated_provider_init(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_state_owner_source_ops *source_ops,
	void *source_context,
	const struct mt6797_dvfsp_ppm_owner_ops *ppm_owner_ops,
	void *ppm_owner_context, u64 owner_handle, u64 transition_handle)
{
	int ret;

	if (!provider || !resource_owner || !resource_owner->initialized ||
	    !resource_owner->attached || !source_ops || !ppm_owner_ops)
		return -EINVAL;
	if (provider->initialized)
		return -EALREADY;

	ret = mt6797_dvfsp_resource_owner_bind_source(resource_owner);
	if (ret)
		return ret;

	ret = mt6797_dvfsp_state_owner_source_init(
		&provider->source, &resource_owner->devices, source_ops,
		source_context, ppm_owner_ops, ppm_owner_context,
		owner_handle, transition_handle);
	if (ret)
		goto err_unbind;

	ret = mt6797_dvfsp_resource_owner_arbitration_ops_init(
		resource_owner, &provider->arbitration_ops);
	if (ret)
		goto err_unbind;

	ret = mt6797_dvfsp_state_owner_arbitration_init(
		&provider->arbitration, &provider->source,
		&provider->arbitration_ops, resource_owner);
	if (ret)
		goto err_unbind;

	provider->resource_owner = resource_owner;
	provider->initialized = true;
	return 0;

err_unbind:
	mt6797_dvfsp_resource_owner_unbind_source(resource_owner);
	memset(provider, 0, sizeof(*provider));
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_init);

int mt6797_dvfsp_calibrated_provider_exit(
	struct mt6797_dvfsp_calibrated_provider *provider)
{
	int ret;

	ret = mt6797_dvfsp_calibrated_provider_check(provider);
	if (ret)
		return ret;

	mt6797_dvfsp_state_owner_arbitration_invalidate(
		&provider->arbitration, MT6797_DVFSP_STATE_INVALID_OWNER_REMOVED);
	ret = mt6797_dvfsp_resource_owner_unbind_source(
		provider->resource_owner);
	if (ret)
		return ret;

	memset(provider, 0, sizeof(*provider));
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_exit);

int mt6797_dvfsp_calibrated_provider_snapshot(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret = mt6797_dvfsp_calibrated_provider_check(provider);

	if (ret)
		return ret;
	return mt6797_dvfsp_state_owner_arbitration_snapshot(
		&provider->arbitration, snapshot);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_snapshot);

int mt6797_dvfsp_calibrated_provider_validate(
	struct mt6797_dvfsp_calibrated_provider *provider,
	const struct mt6797_dvfsp_state_snapshot *snapshot)
{
	int ret = mt6797_dvfsp_calibrated_provider_check(provider);

	if (ret)
		return ret;
	return mt6797_dvfsp_state_owner_arbitration_validate(
		&provider->arbitration, snapshot);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_validate);

int mt6797_dvfsp_calibrated_provider_identify(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	int ret = mt6797_dvfsp_calibrated_provider_check(provider);

	if (ret)
		return ret;
	return mt6797_dvfsp_state_owner_arbitration_identify(
		&provider->arbitration, identity);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_identify);

int mt6797_dvfsp_calibrated_provider_owner_ops_init(
	const struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_state_owner_ops *ops)
{
	if (mt6797_dvfsp_calibrated_provider_check(provider) || !ops)
		return -EINVAL;
	return mt6797_dvfsp_state_owner_arbitration_owner_ops_init(
		&provider->arbitration, ops);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_owner_ops_init);

void mt6797_dvfsp_calibrated_provider_invalidate(
	struct mt6797_dvfsp_calibrated_provider *provider,
	enum mt6797_dvfsp_state_invalidation_reason reason)
{
	if (mt6797_dvfsp_calibrated_provider_check(provider))
		return;
	mt6797_dvfsp_state_owner_arbitration_invalidate(
		&provider->arbitration, reason);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_invalidate);

static void mt6797_dvfsp_calibrated_provider_runtime_invalidate(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	mt6797_dvfsp_calibrated_provider_invalidate(context, reason);
}

int mt6797_dvfsp_calibrated_provider_apply_runtime_event(
	struct mt6797_dvfsp_calibrated_provider *provider,
	const struct mt6797_dvfsp_runtime_event *event,
	struct mt6797_dvfsp_runtime_ledger *ledger)
{
	if (!provider)
		return -EINVAL;
	return mt6797_dvfsp_runtime_event_apply_invalidator(event, ledger,
			mt6797_dvfsp_calibrated_provider_runtime_invalidate, provider);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_apply_runtime_event);

int mt6797_dvfsp_calibrated_provider_bind_sources(
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_identity_source *identity_source,
	const struct mt6797_dvfsp_live_source *live_source,
	const struct mt6797_dvfsp_ppm_source *ppm_source,
	const struct mt6797_dvfsp_calibrated_provider_source_ops *source_ops,
	void *source_context, u64 owner_handle, u64 transition_handle)
{
	int ret;

	if (!provider || provider->initialized || !identity_source ||
	    !live_source || !ppm_source || !source_ops ||
	    source_ops->abi != MT6797_DVFSP_CALIBRATED_SOURCE_ABI ||
    !source_ops->fill_calibration || !source_ops->invalidate)
		return -EINVAL;

	provider->identity_source = *identity_source;
	provider->live_source = *live_source;
	provider->ppm_source = *ppm_source;
	provider->source_ops = source_ops;
	provider->source_context = source_context;

	ret = mt6797_dvfsp_ppm_source_owner_ops_init(&provider->ppm_source,
						&provider->bound_ppm_ops);
	if (ret)
		goto err_clear;

	memset(&provider->bound_source_ops, 0,
		   sizeof(provider->bound_source_ops));
	provider->bound_source_ops.abi = MT6797_DVFSP_STATE_OWNER_SOURCE_ABI;
	provider->bound_source_ops.read_identity =
		mt6797_dvfsp_calibrated_provider_read_identity;
	provider->bound_source_ops.fill_calibration =
		mt6797_dvfsp_calibrated_provider_fill_calibration;
	provider->bound_source_ops.fill_live =
		mt6797_dvfsp_calibrated_provider_fill_live;
	provider->bound_source_ops.invalidate =
		mt6797_dvfsp_calibrated_provider_invalidate_cb;

	ret = mt6797_dvfsp_calibrated_provider_init(provider, resource_owner,
			&provider->bound_source_ops, provider,
			&provider->bound_ppm_ops, &provider->ppm_source,
			owner_handle, transition_handle);
	if (ret)
		return ret;

	return 0;

err_clear:
	memset(provider, 0, sizeof(*provider));
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_calibrated_provider_bind_sources);

MODULE_DESCRIPTION("MT6797 DVFSP calibrated provider binding");
MODULE_LICENSE("GPL");
