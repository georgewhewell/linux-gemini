// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant, hardware-neutral MT6797 CONSYS post-identity owner.
 *
 * The vendor order is intentionally represented as three callbacks:
 * MBIST, AFE, then reset release.  This file never supplies MMIO offsets and
 * never calls a live backend by itself.  A caller must provide an active,
 * exclusive CONSYS runtime lease and a separate experiment gate.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>

static bool mt6797_consys_postid_ops_valid(
		const struct mt6797_consys_postid_ops *ops)
{
	return ops && ops->abi == MT6797_CONSYS_POSTID_ABI &&
		ops->configure_mbist && ops->configure_afe &&
		ops->release_reset && ops->assert_reset;
}

static void mt6797_consys_postid_unpin(
		struct mt6797_consys_postid_owner *owner)
{
	const struct mt6797_consys_runtime_lease *lease = owner->lease;

	if (!lease)
		return;
	owner->lease = NULL;
	mt6797_consys_runtime_lease_unpin(lease);
}

static int mt6797_consys_postid_fail(
		struct mt6797_consys_postid_owner *owner, int error,
		bool persistent)
{
	int reset_ret;

	/* A failed callback may have changed hardware before returning an error. */
	reset_ret = owner->ops->assert_reset(owner->context);
	if (persistent || reset_ret) {
		/* Keep the pin: the subsystem is not known to be safely held/reset. */
		owner->state = MT6797_CONSYS_POSTID_ERROR;
		return error;
	}

	mt6797_consys_postid_unpin(owner);
	owner->state = MT6797_CONSYS_POSTID_IDLE;
	return error;
}

int mt6797_consys_postid_init(
		struct mt6797_consys_postid_owner *owner,
		struct mt6797_consys_lifecycle *lifecycle,
		const struct mt6797_consys_postid_ops *ops, void *context)
{
	if (!owner || !lifecycle || !mt6797_consys_postid_ops_valid(ops))
		return -EINVAL;

	owner->lifecycle = lifecycle;
	owner->ops = ops;
	owner->context = context;
	owner->lease = NULL;
	owner->state = MT6797_CONSYS_POSTID_IDLE;
	mutex_init(&owner->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_postid_init);

int mt6797_consys_postid_start(
		struct mt6797_consys_postid_owner *owner,
		const struct mt6797_consys_runtime_lease *lease, bool experiment_gate)
{
	int ret;

	if (!owner || !lease)
		return -EINVAL;
	if (!experiment_gate)
		return -EPERM;

	mutex_lock(&owner->lock);
	if (!owner->lifecycle || !owner->ops) {
		ret = -EINVAL;
	} else if (owner->state == MT6797_CONSYS_POSTID_ERROR) {
		ret = -EIO;
	} else if (owner->state != MT6797_CONSYS_POSTID_IDLE) {
		ret = -EBUSY;
	} else if (lease->lifecycle != owner->lifecycle ||
		   !mt6797_consys_runtime_lease_active(lease)) {
		ret = -ESHUTDOWN;
	} else {
		ret = mt6797_consys_runtime_lease_pin(lease);
		if (!ret) {
			owner->lease = lease;
			ret = owner->ops->configure_mbist(owner->context);
			if (ret)
				ret = mt6797_consys_postid_fail(owner, ret, false);
			else {
				ret = owner->ops->configure_afe(owner->context);
				if (ret)
					ret = mt6797_consys_postid_fail(owner, ret, true);
				else {
					ret = owner->ops->release_reset(owner->context);
					if (ret)
						ret = mt6797_consys_postid_fail(owner, ret, true);
					else
						owner->state = MT6797_CONSYS_POSTID_ACTIVE;
				}
			}
		}
	}
	mutex_unlock(&owner->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_postid_start);

int mt6797_consys_postid_stop(struct mt6797_consys_postid_owner *owner)
{
	int ret;

	if (!owner)
		return -EINVAL;

	mutex_lock(&owner->lock);
	if (!owner->lease)
		ret = owner->state == MT6797_CONSYS_POSTID_ERROR ? -EIO : -EINVAL;
	else {
		ret = owner->ops->assert_reset(owner->context);
		if (ret) {
			owner->state = MT6797_CONSYS_POSTID_ERROR;
		} else {
			mt6797_consys_postid_unpin(owner);
			owner->state = MT6797_CONSYS_POSTID_IDLE;
		}
	}
	mutex_unlock(&owner->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_postid_stop);

enum mt6797_consys_postid_state
mt6797_consys_postid_state(struct mt6797_consys_postid_owner *owner)
{
	enum mt6797_consys_postid_state state;

	if (!owner)
		return MT6797_CONSYS_POSTID_ERROR;
	mutex_lock(&owner->lock);
	state = owner->state;
	mutex_unlock(&owner->lock);
	return state;
}
EXPORT_SYMBOL_GPL(mt6797_consys_postid_state);

MODULE_DESCRIPTION("Dormant MT6797 CONSYS post-identity bootstrap owner");
MODULE_LICENSE("GPL");
