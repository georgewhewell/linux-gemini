// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral fail-closed transaction for a single MD1 cold start.
 *
 * The backend makes all writes.  This core deliberately has no cleanup path:
 * after CCIF clear or a power/clock operation has been attempted, neither
 * vendor source supplies a generally-owned inverse transaction.  Failure
 * permanently latches this object and requires reset/reboot before review of
 * any further MD1 action.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/string.h>

static bool mt6797_md1_cold_owner_preflight_valid(
	const struct mt6797_md1_boot_status_preflight *preflight)
{
	/* The observer's three reads establish access, not a value meaning. */
	return preflight && preflight->lk_image_loaded &&
		preflight->boot_status_observed &&
		preflight->stats0_samples == 3 && preflight->stats1_samples == 3;
}

int mt6797_md1_cold_owner_init(struct mt6797_md1_cold_owner *owner,
	const struct mt6797_md1_cold_owner_ops *ops, void *context)
{
	if (!owner || !ops || ops->abi != MT6797_MD1_COLD_OWNER_ABI ||
	    !ops->verify_cold_reset_held || !ops->ccif_observe ||
	    !ops->ccif_clear || !ops->ccif_verify_cleared || !ops->power_on ||
	    !ops->enable_srcclk || !ops->pll_start || !ops->pll_poll ||
	    !ops->pll_timeout_dump)
		return -EINVAL;

	memset(owner, 0, sizeof(*owner));
	owner->ops = ops;
	owner->context = context;
	mutex_init(&owner->lock);
	owner->state = MT6797_MD1_COLD_OWNER_OFF;
	return 0;
}

static int mt6797_md1_cold_owner_fatal(struct mt6797_md1_cold_owner *owner,
	int error)
{
	owner->last_error = error;
	owner->state = MT6797_MD1_COLD_OWNER_FATAL;
	return error;
}

int mt6797_md1_cold_owner_run(struct mt6797_md1_cold_owner *owner,
	const struct mt6797_md1_boot_status_preflight *preflight)
{
	unsigned int attempt;
	int ret;

	if (!owner || !owner->ops || !preflight)
		return -EINVAL;

	mutex_lock(&owner->lock);
	if (owner->state == MT6797_MD1_COLD_OWNER_FATAL) {
		ret = -EIO;
		goto out_unlock;
	}
	if (owner->state != MT6797_MD1_COLD_OWNER_OFF) {
		ret = -EALREADY;
		goto out_unlock;
	}
	if (!mt6797_md1_cold_owner_preflight_valid(preflight)) {
		ret = -EPERM;
		goto out_unlock;
	}

	/* A rejected cold snapshot has no side effect and does not consume owner. */
	ret = owner->ops->verify_cold_reset_held(owner->context,
						  &owner->cold_state);
	if (ret)
		goto out_unlock;
	owner->state = MT6797_MD1_COLD_OWNER_PREFLIGHT_ACCEPTED;

	/* Every operation below may have changed hardware even when it returns an error. */
	ret = owner->ops->ccif_observe(owner->context, &owner->ccif_before);
	if (ret)
		goto out_fatal;
	ret = owner->ops->ccif_clear(owner->context);
	if (ret)
		goto out_fatal;
	owner->state = MT6797_MD1_COLD_OWNER_CCIF_CLEARED;
	ret = owner->ops->ccif_observe(owner->context, &owner->ccif_after);
	if (ret)
		goto out_fatal;
	ret = owner->ops->ccif_verify_cleared(owner->context,
					       &owner->ccif_before,
					       &owner->ccif_after);
	if (ret)
		goto out_fatal;

	ret = owner->ops->power_on(owner->context);
	if (ret)
		goto out_fatal;
	owner->state = MT6797_MD1_COLD_OWNER_POWERED;
	ret = owner->ops->enable_srcclk(owner->context);
	if (ret)
		goto out_fatal;
	owner->state = MT6797_MD1_COLD_OWNER_SRC_CLK_ENABLED;
	ret = owner->ops->pll_start(owner->context);
	if (ret)
		goto out_fatal;

	for (attempt = 0; attempt < MT6797_MD1_COLD_OWNER_MAX_PLL_POLLS;
	     attempt++) {
		ret = owner->ops->pll_poll(owner->context, &owner->last_pll);
		if (!ret)
			break;
		if (ret != -EAGAIN)
			goto out_fatal;
	}
	if (ret == -EAGAIN) {
		pr_warn("mt6797-md1-cold-owner: MD1 pll poll timed out after %u polls: stage=%d offset=%#x mask=%#x last_value=%#x\n",
			MT6797_MD1_COLD_OWNER_MAX_PLL_POLLS,
			owner->last_pll.stage, owner->last_pll.offset,
			owner->last_pll.mask, owner->last_pll.last_value);
		owner->ops->pll_timeout_dump(owner->context);
		ret = -ETIMEDOUT;
		goto out_fatal;
	}
	/*
	 * Do not write the vendor WDT magic keys: the source gives no AP-visible
	 * readback/ownership contract. There is also no reset or boot-vector
	 * callback here. A later gate must establish both independently.
	 */
	owner->state = MT6797_MD1_COLD_OWNER_PLL_READY_WDT_UNOWNED;
	owner->last_error = 0;
	ret = 0;
	goto out_unlock;

out_fatal:
	ret = mt6797_md1_cold_owner_fatal(owner, ret);
out_unlock:
	mutex_unlock(&owner->lock);
	return ret;
}

enum mt6797_md1_cold_owner_state
mt6797_md1_cold_owner_state(struct mt6797_md1_cold_owner *owner)
{
	enum mt6797_md1_cold_owner_state state;

	if (!owner)
		return MT6797_MD1_COLD_OWNER_FATAL;
	mutex_lock(&owner->lock);
	state = owner->state;
	mutex_unlock(&owner->lock);
	return state;
}

bool mt6797_md1_cold_owner_post_vector_activity(
	const struct mt6797_md1_post_vector_evidence *evidence)
{
	const struct mt6797_md1_cold_owner_ccif *before, *after;

	if (!evidence)
		return false;

	before = &evidence->ccif_before;
	after = &evidence->ccif_after;
	return evidence->boot_status0_before != evidence->boot_status0_after ||
		evidence->boot_status1_before != evidence->boot_status1_after ||
		before->ap_busy != after->ap_busy ||
		before->md_busy != after->md_busy ||
		before->ap_ack != after->ap_ack ||
		before->md_ack != after->md_ack ||
		before->ap_sram_nonzero_words != after->ap_sram_nonzero_words ||
		before->md_sram_nonzero_words != after->md_sram_nonzero_words;
}
