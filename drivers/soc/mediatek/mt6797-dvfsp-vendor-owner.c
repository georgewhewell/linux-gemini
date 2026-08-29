// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compose explicit vendor callbacks around the dormant MT6797 DVFSP owner.
 *
 * The vendor PTP-table, voltage, and PPM registration APIs are intentionally
 * absent here: each is a single callback slot in the legacy driver. A real
 * integration must chain those callbacks at their writer and then provide
 * one coherent generation and transition owner through this contract.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-owner.h>

static int mt6797_dvfsp_vendor_owner_ops_check(
	const struct mt6797_dvfsp_vendor_owner_ops *ops)
{
	if (!ops || ops->abi != MT6797_DVFSP_VENDOR_OWNER_ABI ||
	    !ops->identity_ops ||
	    ops->identity_ops->abi != MT6797_DVFSP_IDENTITY_SOURCE_ABI ||
	    !ops->identity_ops->read_table_epoch ||
	    !ops->identity_ops->read_calibration_handle ||
	    !ops->ppm_ops ||
	    ops->ppm_ops->abi != MT6797_DVFSP_PPM_SOURCE_ADAPTER_ABI ||
	    !ops->ppm_ops->lock || !ops->ppm_ops->unlock ||
	    !ops->ppm_ops->read_ppm_clusters ||
	    !ops->ppm_ops->read_cci_frequency ||
	    !ops->ppm_ops->read_policy_limits ||
	    !ops->ppm_ops->read_table_epoch ||
	    !ops->live_ops ||
	    ops->live_ops->abi != MT6797_DVFSP_LIVE_SOURCE_ADAPTER_ABI ||
	    !ops->live_ops->read_generation ||
	    !ops->live_ops->read_cluster ||
	    !ops->provider_ops ||
	    ops->provider_ops->abi != MT6797_DVFSP_CALIBRATED_SOURCE_ABI ||
	    !ops->provider_ops->fill_calibration ||
	    !ops->provider_ops->invalidate ||
	    !ops->writer_ops ||
	    ops->writer_ops->abi != MT6797_DVFSP_VENDOR_WRITER_ABI ||
	    !ops->writer_ops->read_provenance)
		return -EINVAL;

	return 0;
}

static int mt6797_dvfsp_vendor_owner_registration_ops_check(
	const struct mt6797_dvfsp_vendor_writer_registration_ops *ops)
{
	if (!ops)
		return 0;
	if (ops->abi != MT6797_DVFSP_VENDOR_WRITER_REGISTRATION_ABI ||
	    !ops->register_owner || !ops->unregister_owner)
		return -EINVAL;
	return 0;
}

static int mt6797_dvfsp_vendor_owner_runtime_ops_check(
	const struct mt6797_dvfsp_vendor_runtime_ops *ops)
{
	if (!ops)
		return -EOPNOTSUPP;
	if (ops->abi != MT6797_DVFSP_VENDOR_RUNTIME_EVENT_ABI ||
	    !ops->event)
		return -EINVAL;
	return 0;
}

static int mt6797_dvfsp_vendor_owner_integration_ops_check(
	const struct mt6797_dvfsp_vendor_integration_ops *ops)
{
	if (!ops || ops->abi != MT6797_DVFSP_VENDOR_INTEGRATION_ABI ||
	    !ops->bind_lifecycle || !ops->unbind_lifecycle ||
	    !ops->register_mainline_owner ||
	    !ops->unregister_mainline_owner || !ops->source_registration)
		return -EINVAL;
	return 0;
}

static const struct mt6797_dvfsp_vendor_writer_registration_ops
	mt6797_dvfsp_vendor_owner_integration_registration_ops;
static const struct mt6797_dvfsp_vendor_owner_lifecycle_ops
	mt6797_dvfsp_vendor_owner_integration_lifecycle_ops;

int mt6797_dvfsp_vendor_owner_forward_runtime_event(
	struct mt6797_dvfsp_vendor_owner *owner, unsigned int type,
	unsigned int cpu)
{

	if (!owner || !owner->writer_integration_bound ||
	    !owner->source.initialized || !owner->ops.writer_runtime_ops ||
	    !owner->ops.writer_runtime_ops->event)
		return -ENODEV;
	if (type >= MT6797_DVFSP_VENDOR_RUNTIME_EVENT_COUNT ||
	    (cpu != MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE && cpu >= 10))
		return -EINVAL;
	mt6797_dvfsp_vendor_source_invalidate(&owner->source);
	return owner->ops.writer_runtime_ops->event(
		owner->ops.writer_runtime_context, type, cpu);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_forward_runtime_event);

static int mt6797_dvfsp_vendor_owner_runtime_event(
	void *context, unsigned int type, unsigned int cpu)
{
	return mt6797_dvfsp_vendor_owner_forward_runtime_event(context, type,
		cpu);
}

static const struct mt6797_dvfsp_vendor_runtime_ops
mt6797_dvfsp_vendor_owner_runtime_adapter_ops = {
	.abi = MT6797_DVFSP_VENDOR_RUNTIME_EVENT_ABI,
	.event = mt6797_dvfsp_vendor_owner_runtime_event,
};

static int mt6797_dvfsp_vendor_owner_integration_register_owner(
	void *context,
	const struct mt6797_dvfsp_vendor_writer_bridge_ops *bridge_ops,
	void *bridge_context,
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
	void *runtime_context, u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_owner *owner = context;

	if (!owner || !owner->bound || !owner->writer_integration_bound ||
	    owner->ops.writer_registration_ops !=
		&mt6797_dvfsp_vendor_owner_integration_registration_ops ||
	    owner->ops.writer_registration_context != owner ||
	    !owner->writer_integration_ops ||
	    bridge_ops != mt6797_dvfsp_vendor_writer_bridge_ops() ||
	    bridge_context != &owner->writer_bridge ||
	    runtime_ops != &mt6797_dvfsp_vendor_owner_runtime_adapter_ops ||
	    runtime_context != owner ||
	    owner_handle != owner->owner_handle ||
	    transition_handle != owner->transition_handle)
		return -EPROTO;

	return owner->writer_integration_ops->register_mainline_owner(
		owner->writer_integration_context, bridge_ops, bridge_context,
		runtime_ops, runtime_context, owner_handle, transition_handle);
}

static int mt6797_dvfsp_vendor_owner_integration_unregister_owner(
	void *context,
	const struct mt6797_dvfsp_vendor_writer_bridge_ops *bridge_ops,
	void *bridge_context,
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
	void *runtime_context, u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_owner *owner = context;

	if (!owner || !owner->bound || !owner->writer_integration_bound ||
	    owner->ops.writer_registration_ops !=
		&mt6797_dvfsp_vendor_owner_integration_registration_ops ||
	    owner->ops.writer_registration_context != owner ||
	    !owner->writer_integration_ops ||
	    bridge_ops != mt6797_dvfsp_vendor_writer_bridge_ops() ||
	    bridge_context != &owner->writer_bridge ||
	    runtime_ops != &mt6797_dvfsp_vendor_owner_runtime_adapter_ops ||
	    runtime_context != owner ||
	    owner_handle != owner->owner_handle ||
	    transition_handle != owner->transition_handle)
		return -EPROTO;

	return owner->writer_integration_ops->unregister_mainline_owner(
		owner->writer_integration_context, bridge_ops, bridge_context,
		runtime_ops, runtime_context, owner_handle, transition_handle);
}

static int mt6797_dvfsp_vendor_owner_integration_register_lifecycle(
	void *context)
{
	struct mt6797_dvfsp_vendor_owner *owner = context;

	if (!owner || !owner->writer_integration_bound)
		return -ENODEV;
	return mt6797_dvfsp_vendor_owner_register_writer(owner);
}

static int mt6797_dvfsp_vendor_owner_integration_unregister_lifecycle(
	void *context)
{
	struct mt6797_dvfsp_vendor_owner *owner = context;

	if (!owner || !owner->writer_integration_bound)
		return -ENODEV;
	return mt6797_dvfsp_vendor_owner_unregister_writer(owner);
}

static const struct mt6797_dvfsp_vendor_writer_registration_ops
mt6797_dvfsp_vendor_owner_integration_registration_ops = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_REGISTRATION_ABI,
	.register_owner = mt6797_dvfsp_vendor_owner_integration_register_owner,
	.unregister_owner = mt6797_dvfsp_vendor_owner_integration_unregister_owner,
};

static const struct mt6797_dvfsp_vendor_owner_lifecycle_ops
mt6797_dvfsp_vendor_owner_integration_lifecycle_ops = {
	.abi = MT6797_DVFSP_VENDOR_OWNER_LIFECYCLE_ABI,
	.register_writer = mt6797_dvfsp_vendor_owner_integration_register_lifecycle,
	.unregister_writer = mt6797_dvfsp_vendor_owner_integration_unregister_lifecycle,
};

int mt6797_dvfsp_vendor_owner_bind_writer_integration(
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context)
{
	const struct mt6797_dvfsp_vendor_source_registration *source_registration;
	int ret;

	if (!owner || !owner->bound || owner->writer_registered ||
	    owner->writer_integration_bound || !integration_context ||
	    owner->ops.writer_registration_ops)
		return -EBUSY;
	ret = mt6797_dvfsp_vendor_owner_integration_ops_check(integration_ops);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_owner_runtime_ops_check(
		owner->ops.writer_runtime_ops);
	if (ret)
		return ret;
	if (!owner->ops.writer_runtime_context)
		return -EINVAL;
	source_registration = integration_ops->source_registration;
	if (!source_registration || !source_registration->ops ||
	    !source_registration->context)
		return -EINVAL;
	ret = mt6797_dvfsp_vendor_writer_bridge_init(&owner->writer_bridge,
		&owner->writer);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_source_init(&owner->source,
		source_registration->ops, source_registration->context,
		owner->owner_handle, owner->transition_handle);
	if (ret) {
		memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
		return ret;
	}
	ret = mt6797_dvfsp_vendor_provider_bridge_init(&owner->provider_bridge,
		&owner->provider, &owner->source, owner->owner_handle,
		owner->transition_handle);
	if (ret) {
		mt6797_dvfsp_vendor_source_exit(&owner->source);
		memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
		return ret;
	}

	owner->writer_integration_ops = integration_ops;
	owner->writer_integration_context = integration_context;
	owner->ops.writer_registration_ops =
		&mt6797_dvfsp_vendor_owner_integration_registration_ops;
	owner->ops.writer_registration_context = owner;
	owner->writer_integration_bound = true;
	ret = integration_ops->bind_lifecycle(integration_context,
		mt6797_dvfsp_vendor_writer_bridge_ops(), &owner->writer_bridge,
		&mt6797_dvfsp_vendor_owner_runtime_adapter_ops, owner,
		&mt6797_dvfsp_vendor_owner_integration_lifecycle_ops, owner,
		owner->owner_handle, owner->transition_handle);
	if (ret) {
		owner->writer_integration_bound = false;
		owner->writer_integration_ops = NULL;
		owner->writer_integration_context = NULL;
		owner->ops.writer_registration_ops = NULL;
		owner->ops.writer_registration_context = NULL;
		mt6797_dvfsp_vendor_provider_bridge_exit(&owner->provider_bridge);
		mt6797_dvfsp_vendor_source_exit(&owner->source);
		memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_bind_writer_integration);

int mt6797_dvfsp_vendor_owner_unbind_writer_integration(
	struct mt6797_dvfsp_vendor_owner *owner)
{
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops;
	int ret;

	if (!owner || !owner->bound || !owner->writer_integration_bound)
		return -EINVAL;
	if (owner->writer_registered || owner->state_registered ||
	    owner->ops.writer_registration_ops !=
		&mt6797_dvfsp_vendor_owner_integration_registration_ops ||
	    owner->ops.writer_registration_context != owner)
		return -EBUSY;
	integration_ops = owner->writer_integration_ops;
	if (!integration_ops || !owner->writer_integration_context)
		return -EINVAL;
	ret = integration_ops->unbind_lifecycle(owner->writer_integration_context,
		mt6797_dvfsp_vendor_writer_bridge_ops(), &owner->writer_bridge,
		&mt6797_dvfsp_vendor_owner_runtime_adapter_ops, owner,
		&mt6797_dvfsp_vendor_owner_integration_lifecycle_ops, owner,
		owner->owner_handle, owner->transition_handle);
	if (ret)
		return ret;

	owner->writer_integration_bound = false;
	owner->writer_integration_ops = NULL;
	owner->writer_integration_context = NULL;
	owner->ops.writer_registration_ops = NULL;
	owner->ops.writer_registration_context = NULL;
	mt6797_dvfsp_vendor_provider_bridge_exit(&owner->provider_bridge);
	mt6797_dvfsp_vendor_source_exit(&owner->source);
	memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_unbind_writer_integration);

int mt6797_dvfsp_vendor_owner_bind(
	struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_resource_owner *resource_owner,
	const struct mt6797_dvfsp_vendor_owner_ops *ops,
	u64 owner_handle, u64 transition_handle)
{
	int ret;

	if (!owner || !resource_owner || !resource_owner->initialized ||
	    !resource_owner->attached || !owner_handle || !transition_handle ||
	    resource_owner->transition_handle != transition_handle ||
	    owner->bound || owner->provider.initialized)
		return -EINVAL;
	ret = mt6797_dvfsp_vendor_owner_ops_check(ops);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_owner_registration_ops_check(
		ops->writer_registration_ops);
	if (ret)
		return ret;

	memset(owner, 0, sizeof(*owner));
	owner->resource_owner = resource_owner;
	owner->ops = *ops;
	owner->owner_handle = owner_handle;
	owner->transition_handle = transition_handle;
	owner->identity_source.devices = resource_owner->devices;
	owner->identity_source.provenance_ops = owner->ops.identity_ops;
	owner->identity_source.provenance_context =
		owner->ops.identity_context;
	owner->live_source.ops = owner->ops.live_ops;
	owner->live_source.context = owner->ops.live_context;
	owner->ppm_source.ops = owner->ops.ppm_ops;
	owner->ppm_source.context = owner->ops.ppm_context;

	ret = mt6797_dvfsp_vendor_writer_init(&owner->writer,
		&resource_owner->transition_lock, &resource_owner->generation,
		owner->ops.writer_ops, owner->ops.writer_context, owner_handle,
		transition_handle);
	if (ret) {
		memset(owner, 0, sizeof(*owner));
		return ret;
	}

	ret = mt6797_dvfsp_calibrated_provider_bind_sources(
		&owner->provider, resource_owner, &owner->identity_source,
		&owner->live_source, &owner->ppm_source, owner->ops.provider_ops,
		owner->ops.provider_context, owner_handle, transition_handle);
	if (ret) {
		memset(owner, 0, sizeof(*owner));
		return ret;
	}

	owner->bound = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_bind);

int mt6797_dvfsp_vendor_owner_register_writer(
	struct mt6797_dvfsp_vendor_owner *owner)
{
	const struct mt6797_dvfsp_vendor_writer_registration_ops *ops;
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops;
	void *runtime_context;
	int ret;

	if (!owner || !owner->bound || owner->writer_registered)
		return -EINVAL;
	if (!owner->writer_integration_bound)
		return -ENODEV;
	ops = owner->ops.writer_registration_ops;
	if (!ops)
		return -EOPNOTSUPP;
	runtime_ops = &mt6797_dvfsp_vendor_owner_runtime_adapter_ops;
	runtime_context = owner;
	ret = mt6797_dvfsp_vendor_owner_runtime_ops_check(runtime_ops);
	if (ret)
		return ret;

	ret = mt6797_dvfsp_vendor_writer_bridge_init(&owner->writer_bridge,
		&owner->writer);
	if (ret)
		return ret;

	ret = ops->register_owner(owner->ops.writer_registration_context,
		mt6797_dvfsp_vendor_writer_bridge_ops(), &owner->writer_bridge,
		runtime_ops, runtime_context, owner->owner_handle,
		owner->transition_handle);
	if (ret) {
		memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
		return ret;
	}

	owner->writer_runtime_ops = runtime_ops;
	owner->writer_runtime_context = runtime_context;
	owner->writer_registered = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_register_writer);

static int mt6797_dvfsp_vendor_owner_read_state(
	struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped,
	struct mt6797_dvfsp_state_snapshot *snapshot,
	struct mt6797_dvfsp_state_owner_identity *identity)
{
	int ret;

	memset(mapped, 0, sizeof(*mapped));
	ret = mt6797_dvfsp_vendor_provider_bridge_snapshot(
		&owner->provider_bridge, mapped);
	if (ret)
		return ret;
	memset(snapshot, 0, sizeof(*snapshot));
	ret = mt6797_dvfsp_calibrated_provider_snapshot(
		&owner->provider, snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_calibrated_provider_validate(
		&owner->provider, snapshot);
	if (ret)
		return ret;
	memset(identity, 0, sizeof(*identity));
	ret = mt6797_dvfsp_calibrated_provider_identify(
		&owner->provider, identity);
	if (ret)
		return ret;
	return mt6797_dvfsp_vendor_provider_match_state(mapped, snapshot,
		identity);
}

int mt6797_dvfsp_vendor_owner_register_state(
	struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_handoff *handoff)
{
	struct mt6797_dvfsp_vendor_provider_snapshot *before_mapped;
	struct mt6797_dvfsp_vendor_provider_snapshot *after_mapped;
	struct mt6797_dvfsp_state_snapshot *before_snapshot;
	struct mt6797_dvfsp_state_snapshot *after_snapshot;
	struct mt6797_dvfsp_state_owner_identity before_identity;
	struct mt6797_dvfsp_state_owner_identity after_identity;
	struct mt6797_dvfsp_state_owner_identity registered_identity;
	int rollback_ret;
	int ret;

	if (!owner || !handoff)
		return -EINVAL;
	if (!owner->bound || !owner->writer_integration_bound ||
	    !owner->writer_registered || !owner->provider_bridge.initialized)
		return -ENODEV;
	if (owner->state_registered)
		return -EBUSY;
	before_mapped = kzalloc_obj(*before_mapped);
	after_mapped = kzalloc_obj(*after_mapped);
	before_snapshot = kzalloc_obj(*before_snapshot);
	after_snapshot = kzalloc_obj(*after_snapshot);
	if (!before_mapped || !after_mapped || !before_snapshot ||
	    !after_snapshot) {
		ret = -ENOMEM;
		goto out_free;
	}
	ret = mt6797_dvfsp_vendor_owner_read_state(owner, before_mapped,
		before_snapshot, &before_identity);
	if (ret)
		goto out_free;
	ret = mt6797_dvfsp_state_owner_arbitration_register(handoff,
		&owner->provider.arbitration);
	if (ret)
		goto out_free;
	owner->state_handoff = handoff;
	owner->state_registered = true;
	memset(&registered_identity, 0, sizeof(registered_identity));
	ret = mt6797_dvfsp_handoff_state_owner_identity(handoff,
		&registered_identity);
	if (!ret && memcmp(&before_identity, &registered_identity,
			   sizeof(before_identity)))
		ret = -EAGAIN;
	if (ret)
		goto out_rollback;
	ret = mt6797_dvfsp_vendor_owner_read_state(owner, after_mapped,
		after_snapshot, &after_identity);
	if (!ret && (memcmp(before_mapped, after_mapped,
			    sizeof(*before_mapped)) ||
		    memcmp(before_snapshot, after_snapshot,
			    sizeof(*before_snapshot)) ||
		    memcmp(&before_identity, &after_identity,
			    sizeof(before_identity))))
		ret = -EAGAIN;
	if (!ret)
		goto out_free;

out_rollback:
	rollback_ret = mt6797_dvfsp_state_owner_arbitration_unregister(
		handoff, &owner->provider.arbitration);
	if (rollback_ret) {
		ret = rollback_ret;
		goto out_free;
	}
	owner->state_handoff = NULL;
	owner->state_registered = false;

out_free:
	kfree(after_snapshot);
	kfree(before_snapshot);
	kfree(after_mapped);
	kfree(before_mapped);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_register_state);

int mt6797_dvfsp_vendor_owner_unregister_state(
	struct mt6797_dvfsp_vendor_owner *owner)
{
	int ret;

	if (!owner || !owner->bound)
		return -EINVAL;
	if (!owner->state_registered || !owner->state_handoff)
		return -ENOENT;
	ret = mt6797_dvfsp_state_owner_arbitration_unregister(
		owner->state_handoff, &owner->provider.arbitration);
	if (ret)
		return ret;
	owner->state_handoff = NULL;
	owner->state_registered = false;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_unregister_state);

int mt6797_dvfsp_vendor_owner_unregister_writer(
	struct mt6797_dvfsp_vendor_owner *owner)
{
	const struct mt6797_dvfsp_vendor_writer_registration_ops *ops;
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops;
	void *runtime_context;
	int ret;

	if (!owner || !owner->bound || !owner->writer_registered)
		return -EINVAL;
	if (owner->state_registered)
		return -EBUSY;
	ops = owner->ops.writer_registration_ops;
	if (!ops)
		return -EINVAL;
	runtime_ops = owner->writer_runtime_ops;
	runtime_context = owner->writer_runtime_context;
	ret = mt6797_dvfsp_vendor_owner_runtime_ops_check(runtime_ops);
	if (ret)
		return ret;

	ret = ops->unregister_owner(owner->ops.writer_registration_context,
		mt6797_dvfsp_vendor_writer_bridge_ops(), &owner->writer_bridge,
		runtime_ops, runtime_context, owner->owner_handle,
		owner->transition_handle);
	if (ret)
		return ret;

	owner->writer_registered = false;
	owner->writer_runtime_ops = NULL;
	owner->writer_runtime_context = NULL;
	memset(&owner->writer_bridge, 0, sizeof(owner->writer_bridge));
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_unregister_writer);

int mt6797_dvfsp_vendor_owner_unbind(
	struct mt6797_dvfsp_vendor_owner *owner)
{
	int ret;

	if (!owner || !owner->bound)
		return -EINVAL;
	if (owner->writer_registered || owner->writer_integration_bound ||
	    owner->state_registered)
		return -EBUSY;
	if (owner->writer.update_held)
		return -EBUSY;

	ret = mt6797_dvfsp_calibrated_provider_exit(&owner->provider);
	if (ret)
		return ret;

	memset(owner, 0, sizeof(*owner));
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_unbind);

int mt6797_dvfsp_vendor_owner_state_ops_init(
	const struct mt6797_dvfsp_vendor_owner *owner,
	struct mt6797_dvfsp_state_owner_ops *state_ops)
{
	if (!owner || !owner->bound || !state_ops)
		return -EINVAL;

	return mt6797_dvfsp_calibrated_provider_owner_ops_init(
		&owner->provider, state_ops);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_owner_state_ops_init);

MODULE_DESCRIPTION("MT6797 cooperative vendor DVFSP owner adapter");
MODULE_LICENSE("GPL");
