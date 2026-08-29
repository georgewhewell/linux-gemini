// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 DVFSP resource-only owner.
 *
 * This object provides only device lifetime, one transition mutex, and an
 * epoch for a future source provider. It intentionally has no hardware
 * operation and no handoff registration path.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-resource-owner.h>

static int mt6797_dvfsp_resource_devices_check(
	const struct mt6797_dvfsp_state_source_devices *devices)
{
	if (!devices || !devices->clock_backend ||
	    !devices->bigidvfs_backend || !devices->thermal ||
	    !devices->calibration_source)
		return -ENODEV;

	return 0;
}

static void mt6797_dvfsp_resource_devices_release(
	struct mt6797_dvfsp_state_source_devices *devices)
{
	if (!devices)
		return;

	if (devices->calibration_source)
		put_device(devices->calibration_source);
	if (devices->thermal)
		put_device(devices->thermal);
	if (devices->bigidvfs_backend)
		put_device(devices->bigidvfs_backend);
	if (devices->clock_backend)
		put_device(devices->clock_backend);
	memset(devices, 0, sizeof(*devices));
}

static int mt6797_dvfsp_resource_devices_retain(
	struct mt6797_dvfsp_state_source_devices *held,
	const struct mt6797_dvfsp_state_source_devices *devices)
{
	memset(held, 0, sizeof(*held));
	held->clock_backend = get_device(devices->clock_backend);
	if (!held->clock_backend)
		goto err;
	held->bigidvfs_backend = get_device(devices->bigidvfs_backend);
	if (!held->bigidvfs_backend)
		goto err;
	held->thermal = get_device(devices->thermal);
	if (!held->thermal)
		goto err;
	held->calibration_source = get_device(devices->calibration_source);
	if (!held->calibration_source)
		goto err;
	return 0;

err:
	mt6797_dvfsp_resource_devices_release(held);
	return -ENODEV;
}

int mt6797_dvfsp_resource_owner_init(
	struct mt6797_dvfsp_resource_owner *owner, u64 transition_handle)
{
	if (!owner || !transition_handle)
		return -EINVAL;

	memset(owner, 0, sizeof(*owner));
	mutex_init(&owner->lifecycle_lock);
	mutex_init(&owner->transition_lock);
	owner->transition_handle = transition_handle;
	owner->writes_disabled = true;
	owner->initialized = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_init);

int mt6797_dvfsp_resource_owner_attach(
	struct mt6797_dvfsp_resource_owner *owner,
	const struct mt6797_dvfsp_state_source_devices *devices)
{
	struct mt6797_dvfsp_state_source_devices held;
	int ret;

	if (!owner || !owner->initialized)
		return -EINVAL;
	ret = mt6797_dvfsp_resource_devices_check(devices);
	if (ret)
		return ret;

	mutex_lock(&owner->lifecycle_lock);
	if (owner->attached) {
		ret = -EBUSY;
		goto out_unlock;
	}

	mutex_lock(&owner->transition_lock);
	ret = mt6797_dvfsp_resource_devices_retain(&held, devices);
	if (!ret) {
		owner->devices = held;
		owner->generation = 1;
		owner->attached = true;
	}
	mutex_unlock(&owner->transition_lock);

out_unlock:
	mutex_unlock(&owner->lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_attach);

int mt6797_dvfsp_resource_owner_detach(
	struct mt6797_dvfsp_resource_owner *owner)
{
	struct mt6797_dvfsp_state_source_devices held;

	if (!owner || !owner->initialized)
		return -EINVAL;

	mutex_lock(&owner->lifecycle_lock);
	if (!owner->attached) {
		mutex_unlock(&owner->lifecycle_lock);
		return -ENOENT;
	}
	if (owner->source_bound) {
		mutex_unlock(&owner->lifecycle_lock);
		return -EBUSY;
	}

	mutex_lock(&owner->transition_lock);
	owner->attached = false;
	if (owner->generation != U64_MAX)
		owner->generation++;
	held = owner->devices;
	memset(&owner->devices, 0, sizeof(owner->devices));
	mutex_unlock(&owner->transition_lock);
	mutex_unlock(&owner->lifecycle_lock);

	mt6797_dvfsp_resource_devices_release(&held);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_detach);

int mt6797_dvfsp_resource_owner_bind_source(
	struct mt6797_dvfsp_resource_owner *owner)
{
	if (!owner || !owner->initialized)
		return -EINVAL;

	mutex_lock(&owner->lifecycle_lock);
	if (!owner->attached) {
		mutex_unlock(&owner->lifecycle_lock);
		return -ENODEV;
	}
	if (owner->source_bound) {
		mutex_unlock(&owner->lifecycle_lock);
		return -EBUSY;
	}
	owner->source_bound = true;
	mutex_unlock(&owner->lifecycle_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_bind_source);

int mt6797_dvfsp_resource_owner_unbind_source(
	struct mt6797_dvfsp_resource_owner *owner)
{
	if (!owner || !owner->initialized)
		return -EINVAL;

	mutex_lock(&owner->lifecycle_lock);
	if (!owner->source_bound) {
		mutex_unlock(&owner->lifecycle_lock);
		return -ENOENT;
	}
	mutex_lock(&owner->transition_lock);
	owner->source_bound = false;
	mutex_unlock(&owner->transition_lock);
	mutex_unlock(&owner->lifecycle_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_unbind_source);

int mt6797_dvfsp_resource_owner_advance_generation(
	struct mt6797_dvfsp_resource_owner *owner)
{
	if (!owner || !owner->initialized)
		return -EINVAL;
	if (!owner->attached)
		return -ENODEV;
	if (owner->generation == U64_MAX)
		return -EOVERFLOW;

	owner->generation++;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_advance_generation);

static int mt6797_dvfsp_resource_owner_lock(void *context)
{
	struct mt6797_dvfsp_resource_owner *owner = context;

	if (!owner || !owner->initialized)
		return -EINVAL;
	mutex_lock(&owner->transition_lock);
	if (!owner->attached) {
		mutex_unlock(&owner->transition_lock);
		return -ENODEV;
	}

	return 0;
}

static void mt6797_dvfsp_resource_owner_unlock(void *context)
{
	struct mt6797_dvfsp_resource_owner *owner = context;

	if (owner && owner->initialized)
		mutex_unlock(&owner->transition_lock);
}

static int mt6797_dvfsp_resource_owner_read_generation(
	void *context, u64 *generation)
{
	struct mt6797_dvfsp_resource_owner *owner = context;

	if (!owner || !generation || !owner->initialized)
		return -EINVAL;
	if (!owner->attached || !owner->generation)
		return -ENODEV;

	*generation = owner->generation;
	return 0;
}

int mt6797_dvfsp_resource_owner_arbitration_ops_init(
	const struct mt6797_dvfsp_resource_owner *owner,
	struct mt6797_dvfsp_state_owner_arbitration_ops *ops)
{
	if (!owner || !ops || !owner->initialized)
		return -EINVAL;

	memset(ops, 0, sizeof(*ops));
	ops->abi = MT6797_DVFSP_STATE_OWNER_ARBITRATION_ABI;
	ops->lock = mt6797_dvfsp_resource_owner_lock;
	ops->unlock = mt6797_dvfsp_resource_owner_unlock;
	ops->read_generation = mt6797_dvfsp_resource_owner_read_generation;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_resource_owner_arbitration_ops_init);

MODULE_DESCRIPTION("MT6797 DVFSP resource-only transition owner");
MODULE_LICENSE("GPL");
