// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 CONSYS handoff. Vendor hw_init() precedes the post-ID
 * sequence (wmt_plat_alps.c:353; mtk_wcn_consys_hw.c:1098-1141,592-606).
 * Unlike the old provider path, identity activation is explicitly placed
 * after EMI here. No platform caller or hardware backend is supplied.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-consys-handoff.h>

static bool mt6797_consys_handoff_valid(
		const struct mt6797_consys_handoff_owner *owner)
{
	return owner && owner->emi && owner->postid && owner->runtime_dev;
}

static void mt6797_consys_handoff_release_resources(
		struct mt6797_consys_handoff_owner *owner)
{
	if (!owner->resources_held)
		return;
	if (!mt6797_consys_runtime_resource_lease_release(&owner->resources))
		owner->resources_held = false;
}

static int mt6797_consys_handoff_release_identity(
		struct mt6797_consys_handoff_owner *owner)
{
	int ret;

	if (!owner->identity_held)
		return 0;
	ret = mt6797_consys_runtime_platform_identity_release(
			&owner->identity);
	if (!ret)
		owner->identity_held = false;
	return ret;
}

int mt6797_consys_handoff_init(struct mt6797_consys_handoff_owner *owner,
		struct mt6797_consys_emi_owner *emi,
		struct mt6797_consys_postid_owner *postid,
		struct device *runtime_dev)
{
	if (!owner || !emi || !postid || !runtime_dev)
		return -EINVAL;
	owner->emi = emi;
	owner->postid = postid;
	owner->runtime_dev = runtime_dev;
	owner->resources = (struct mt6797_consys_runtime_resource_lease){ };
	owner->identity = (struct mt6797_consys_runtime_provider_lease){ };
	owner->resources_held = false;
	owner->identity_held = false;
	owner->state = MT6797_CONSYS_HANDOFF_IDLE;
	mutex_init(&owner->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_handoff_init);

int mt6797_consys_handoff_start(struct mt6797_consys_handoff_owner *owner,
		const struct mt6797_consys_emi_layout *layout, bool experiment_gate)
{
	int ret;

	if (!owner || !layout)
		return -EINVAL;
	if (!experiment_gate)
		return -EPERM;
	mutex_lock(&owner->lock);
	if (!mt6797_consys_handoff_valid(owner))
		ret = -EINVAL;
	else if (owner->state == MT6797_CONSYS_HANDOFF_ERROR)
		ret = -EIO;
	else if (owner->state != MT6797_CONSYS_HANDOFF_IDLE)
		ret = -EBUSY;
	else {
		owner->state = MT6797_CONSYS_HANDOFF_STARTING;
		ret = mt6797_consys_runtime_resource_lease_acquire(
				owner->runtime_dev, &owner->resources);
		if (ret)
			goto fail;
		owner->resources_held = true;

		/* Vendor hw_init(): protection, remap, then map/clear. */
		ret = mt6797_consys_emi_start(owner->emi, layout, true);
		if (ret)
			goto fail_resources;

		/* This is the first hardware identity activation in this path. */
		ret = mt6797_consys_runtime_platform_identity_acquire(
				&owner->resources, &owner->identity);
		if (ret)
			goto fail_resources;
		owner->identity_held = true;

		ret = mt6797_consys_postid_start(owner->postid,
				owner->identity.identity);
		if (ret)
			goto fail_identity;
		owner->state = MT6797_CONSYS_HANDOFF_ACTIVE;
		goto out;
	}
	goto out;

fail_identity:
	/* postid retains the identity pin if reset assertion failed. */
	if (mt6797_consys_postid_state(owner->postid) ==
			MT6797_CONSYS_POSTID_ERROR)
		goto fail;
	if (mt6797_consys_handoff_release_identity(owner))
		goto fail;
fail_resources:
	mt6797_consys_handoff_release_resources(owner);
fail:
	/* EMI has no inverse; all failures after it are terminal. */
	owner->state = MT6797_CONSYS_HANDOFF_ERROR;
out:
	mutex_unlock(&owner->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_handoff_start);

int mt6797_consys_handoff_stop(struct mt6797_consys_handoff_owner *owner)
{
	int ret;

	if (!owner)
		return -EINVAL;
	mutex_lock(&owner->lock);
	if (owner->state == MT6797_CONSYS_HANDOFF_ERROR)
		ret = -EIO;
	else if (owner->state != MT6797_CONSYS_HANDOFF_ACTIVE)
		ret = -EINVAL;
	else {
		ret = mt6797_consys_postid_stop(owner->postid);
		if (!ret)
			ret = mt6797_consys_handoff_release_identity(owner);
		if (!ret) {
			mt6797_consys_handoff_release_resources(owner);
			if (owner->resources_held)
				ret = -EIO;
		}
		owner->state = ret ? MT6797_CONSYS_HANDOFF_ERROR :
			MT6797_CONSYS_HANDOFF_QUIESCED;
	}
	mutex_unlock(&owner->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_handoff_stop);

enum mt6797_consys_handoff_state
mt6797_consys_handoff_state(struct mt6797_consys_handoff_owner *owner)
{
	enum mt6797_consys_handoff_state state;

	if (!owner)
		return MT6797_CONSYS_HANDOFF_ERROR;
	mutex_lock(&owner->lock);
	state = owner->state;
	mutex_unlock(&owner->lock);
	return state;
}
EXPORT_SYMBOL_GPL(mt6797_consys_handoff_state);

MODULE_DESCRIPTION("Dormant MT6797 CONSYS true chronology handoff");
MODULE_LICENSE("GPL");
