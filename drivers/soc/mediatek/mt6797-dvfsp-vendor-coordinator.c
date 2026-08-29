// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explicit mainline caller boundary for the MT6797 vendor writer.
 *
 * A platform-specific integration owns the ordering around this object:
 * start before the vendor probe and stop after the vendor remove path.  The
 * coordinator itself only binds and unbinds the source-independent owner ABI.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-coordinator.h>

static struct mt6797_dvfsp_vendor_coordinator
mt6797_dvfsp_vendor_global_coordinator;

int mt6797_dvfsp_vendor_coordinator_bind_caller(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context)
{
	if (!coordinator || coordinator->started || coordinator->caller_bound ||
	    coordinator->owner || coordinator->integration_ops ||
	    coordinator->integration_context)
		return -EBUSY;
	if (!owner || !integration_ops || !integration_context)
		return -EINVAL;

	coordinator->caller_owner = owner;
	coordinator->caller_integration_ops = integration_ops;
	coordinator->caller_integration_context = integration_context;
	coordinator->caller_bound = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_bind_caller);

int mt6797_dvfsp_vendor_coordinator_unbind_caller(
	struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	if (!coordinator || !coordinator->caller_bound)
		return -EINVAL;
	if (coordinator->started)
		return -EBUSY;

	coordinator->caller_owner = NULL;
	coordinator->caller_integration_ops = NULL;
	coordinator->caller_integration_context = NULL;
	coordinator->caller_bound = false;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_unbind_caller);

int mt6797_dvfsp_vendor_coordinator_start(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context)
{
	int ret;

	if (!coordinator || coordinator->started || coordinator->owner ||
	    coordinator->integration_ops || coordinator->integration_context)
		return -EBUSY;
	if (!owner || !integration_ops || !integration_context)
		return -EINVAL;
	if (coordinator->caller_bound &&
	    (coordinator->caller_owner != owner ||
	     coordinator->caller_integration_ops != integration_ops ||
	     coordinator->caller_integration_context != integration_context))
		return -EPROTO;

	ret = mt6797_dvfsp_vendor_owner_bind_writer_integration(owner,
		integration_ops, integration_context);
	if (ret)
		return ret;

	coordinator->owner = owner;
	coordinator->integration_ops = integration_ops;
	coordinator->integration_context = integration_context;
	coordinator->started = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_start);

int mt6797_dvfsp_vendor_coordinator_forward_runtime_event(
	struct mt6797_dvfsp_vendor_coordinator *coordinator,
	unsigned int type, unsigned int cpu)
{
	if (!coordinator || !coordinator->started || !coordinator->owner ||
	    !coordinator->integration_ops || !coordinator->integration_context)
		return -ENODEV;

	return mt6797_dvfsp_vendor_owner_forward_runtime_event(
		coordinator->owner, type, cpu);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_forward_runtime_event);

int mt6797_dvfsp_vendor_coordinator_caller_probe(
	struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	if (!coordinator || !coordinator->caller_bound ||
	    !coordinator->caller_owner ||
	    !coordinator->caller_integration_ops ||
	    !coordinator->caller_integration_context)
		return -ENODEV;

	return mt6797_dvfsp_vendor_coordinator_start(coordinator,
		coordinator->caller_owner, coordinator->caller_integration_ops,
		coordinator->caller_integration_context);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_caller_probe);

int mt6797_dvfsp_vendor_coordinator_caller_remove(
	struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	if (!coordinator || !coordinator->caller_bound)
		return -ENODEV;
	if (!coordinator->started)
		return -EINVAL;

	return mt6797_dvfsp_vendor_coordinator_stop(coordinator);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_caller_remove);

int mt6797_dvfsp_vendor_coordinator_vendor_bind(
	struct mt6797_dvfsp_vendor_owner *owner,
	const struct mt6797_dvfsp_vendor_integration_ops *integration_ops,
	void *integration_context)
{
	return mt6797_dvfsp_vendor_coordinator_bind_caller(
		&mt6797_dvfsp_vendor_global_coordinator, owner, integration_ops,
		integration_context);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_vendor_bind);

int mt6797_dvfsp_vendor_coordinator_vendor_unbind(void)
{
	return mt6797_dvfsp_vendor_coordinator_unbind_caller(
		&mt6797_dvfsp_vendor_global_coordinator);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_vendor_unbind);

int mt6797_dvfsp_vendor_coordinator_vendor_probe(void)
{
	return mt6797_dvfsp_vendor_coordinator_caller_probe(
		&mt6797_dvfsp_vendor_global_coordinator);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_vendor_probe);

int mt6797_dvfsp_vendor_coordinator_vendor_remove(void)
{
	return mt6797_dvfsp_vendor_coordinator_caller_remove(
		&mt6797_dvfsp_vendor_global_coordinator);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_vendor_remove);

int mt6797_dvfsp_vendor_coordinator_stop(
	struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	int ret;

	if (!coordinator || !coordinator->started || !coordinator->owner ||
	    !coordinator->integration_ops || !coordinator->integration_context)
		return -EINVAL;
	if (!coordinator->owner->writer_integration_bound ||
	    coordinator->owner->writer_integration_ops !=
		coordinator->integration_ops ||
	    coordinator->owner->writer_integration_context !=
		coordinator->integration_context)
		return -EPROTO;

	ret = mt6797_dvfsp_vendor_owner_unbind_writer_integration(
		coordinator->owner);
	if (ret)
		return ret;

	coordinator->owner = NULL;
	coordinator->integration_ops = NULL;
	coordinator->integration_context = NULL;
	coordinator->started = false;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_stop);

bool mt6797_dvfsp_vendor_coordinator_started(
	const struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	return coordinator && coordinator->started;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_started);

bool mt6797_dvfsp_vendor_coordinator_caller_bound(
	const struct mt6797_dvfsp_vendor_coordinator *coordinator)
{
	return coordinator && coordinator->caller_bound;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_coordinator_caller_bound);

MODULE_DESCRIPTION("MT6797 vendor writer coordinator");
MODULE_LICENSE("GPL");
