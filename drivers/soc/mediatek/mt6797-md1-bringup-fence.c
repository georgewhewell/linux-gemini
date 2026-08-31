// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 MD1 source-order bring-up fence.
 *
 * This core does not make incomplete vendor knowledge safe.  In particular,
 * it never tries to undo an EMI MPU update or CCIF clear: 3.18 gives no
 * independently-owned generic restoration contract for those operations.  A
 * remap is different: a future backend must snapshot it before writing and
 * offer a best-effort restore callback.  After any attempted prerequisite the
 * core requests reset-held power-off when power may have changed, restores an
 * attempted remap, then permanently fences the transaction.  A new,
 * separately-attested object is required to try again.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-fence.h>
#include <linux/string.h>

static const u8 mt6797_md1_plan_mpu_regions[] = { 11, 12, 13, 10, 14, 7 };

static bool mt6797_md1_bringup_plan_valid(
	const struct mt6797_md1_mpu_remap_plan *plan)
{
	unsigned int i;

	if (!plan || !plan->lk_reported_load_complete ||
	    plan->firmware.header_version < 4 || plan->firmware.bind_sys_id != 1 ||
	    plan->firmware.region_count < MT6797_MD1_MPU_PLAN_HEADER_ENTRIES)
		return false;

	for (i = 0; i < MT6797_MD1_MPU_PLAN_ENTRIES; i++) {
		if (plan->mpu[i].emi_mpu_region != mt6797_md1_plan_mpu_regions[i] ||
		    plan->mpu[i].start > plan->mpu[i].end)
			return false;
	}
	return true;
}

static bool mt6797_md1_bringup_authorized(
	const struct mt6797_md1_bringup_authorization *authorization)
{
	return authorization && authorization->lk_firmware_contract_valid &&
		authorization->mpu_owner_and_failure_fence &&
		authorization->remap_owner_and_failure_fence &&
		authorization->power_reset_owner_and_failure_fence &&
		authorization->clock_owner_and_bounded_pll &&
		authorization->ccif_owner_and_destructive_clear_fence;
}

int mt6797_md1_bringup_fence_init(struct mt6797_md1_bringup_fence *fence,
	const struct mt6797_md1_bringup_ops *ops, void *context)
{
	if (!fence || !ops || ops->abi != MT6797_MD1_BRINGUP_FENCE_ABI ||
	    !ops->apply_remap || !ops->restore_remap || !ops->apply_initial_mpu ||
	    !ops->mpu_latch_fatal ||
	    !ops->ccif_observe ||
	    !ops->ccif_clear || !ops->ccif_verify_cleared ||
	    !ops->power_on_reset_held || !ops->enable_srcclk || !ops->pll_poll ||
	    !ops->apply_stage1_mpu || !ops->power_off_reset_held)
		return -EINVAL;

	memset(fence, 0, sizeof(*fence));
	fence->ops = ops;
	fence->context = context;
	mutex_init(&fence->lock);
	fence->state = MT6797_MD1_BRINGUP_OFF;
	return 0;
}

static void mt6797_md1_bringup_phase_mpu(
	struct mt6797_md1_bringup_fence *fence)
{
	unsigned int i;

	/* Vendor pre-CCIF order: header region 0, then AP/MD shared region. */
	fence->initial_mpu[0] = fence->plan.mpu[0];
	fence->initial_mpu[1] =
		fence->plan.mpu[MT6797_MD1_MPU_SHARED_ENTRY];

	/* Vendor stage one covers header regions 1..4 after reset-held power. */
	for (i = 0; i < MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES; i++)
		fence->stage1_mpu[i] = fence->plan.mpu[i + 1];
	/* Its region-2 setting is explicitly temporary until the HS1 callback. */
	fence->stage1_mpu[1].access_permission =
		MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION;
}

int mt6797_md1_bringup_fence_prepare(struct mt6797_md1_bringup_fence *fence,
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_remap_mode remap_mode,
	const struct mt6797_md1_bringup_authorization *authorization)
{
	int ret = 0;

	if (!fence || !fence->ops || !plan || remap_mode >= MT6797_MD1_REMAP_MODE_COUNT)
		return -EINVAL;

	mutex_lock(&fence->lock);
	if (fence->state != MT6797_MD1_BRINGUP_OFF)
		ret = fence->state == MT6797_MD1_BRINGUP_FENCED ? -EIO : -EBUSY;
	else if (!mt6797_md1_bringup_plan_valid(plan))
		ret = -EINVAL;
	else if (!mt6797_md1_bringup_authorized(authorization))
		ret = -EPERM;
	else {
		fence->plan = *plan;
		mt6797_md1_bringup_phase_mpu(fence);
		fence->remap_mode = remap_mode;
		fence->last_error = 0;
		fence->state = MT6797_MD1_BRINGUP_PREPARED;
	}
	mutex_unlock(&fence->lock);
	return ret;
}

static int mt6797_md1_bringup_fence_fail(struct mt6797_md1_bringup_fence *fence,
	bool power_may_be_on, bool remap_may_be_changed, bool mpu_may_be_changed,
	int ret)
{
	int cleanup, first_cleanup = 0;

	if (power_may_be_on) {
		cleanup = fence->ops->power_off_reset_held(fence->context);
		first_cleanup = cleanup;
		if (!ret)
			ret = cleanup;
	}
	if (remap_may_be_changed) {
		cleanup = fence->ops->restore_remap(fence->context);
		if (!first_cleanup)
			first_cleanup = cleanup;
		if (!ret)
			ret = cleanup;
	}
	/* Secure EMI-MPU has no source-backed readback or inverse transaction. */
	if (mpu_may_be_changed)
		fence->ops->mpu_latch_fatal(fence->context);
	if (first_cleanup)
		pr_err("MT6797 MD1 bring-up cleanup failed: %d\n", first_cleanup);
	fence->last_error = ret;
	fence->state = MT6797_MD1_BRINGUP_FENCED;
	return ret;
}

int mt6797_md1_bringup_fence_run(struct mt6797_md1_bringup_fence *fence)
{
	bool power_may_be_on = false;
	bool remap_may_be_changed = false;
	bool mpu_may_be_changed = false;
	unsigned int attempt;
	int ret;

	if (!fence || !fence->ops)
		return -EINVAL;

	mutex_lock(&fence->lock);
	if (fence->state == MT6797_MD1_BRINGUP_FENCED) {
		ret = -EIO;
		goto out_unlock;
	}
	if (fence->state != MT6797_MD1_BRINGUP_PREPARED) {
		ret = fence->state == MT6797_MD1_BRINGUP_READY_FOR_BOOT_VECTOR ?
			-EALREADY : -EINVAL;
		goto out_unlock;
	}

	/* A write/readback failure can still have changed one remap word. */
	remap_may_be_changed = true;
	ret = fence->ops->apply_remap(fence->context,
			&fence->plan.remap[fence->remap_mode]);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_REMAP_APPLIED;

	/* An SMC error can follow a hardware side effect, so latch before it. */
	mpu_may_be_changed = true;
	ret = fence->ops->apply_initial_mpu(fence->context, &fence->plan,
			fence->initial_mpu,
			MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_INITIAL_MPU_APPLIED;

	ret = fence->ops->ccif_observe(fence->context, &fence->ccif_before);
	if (ret)
		goto out_fence;
	ret = fence->ops->ccif_clear(fence->context);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_CCIF_CLEARED;
	ret = fence->ops->ccif_observe(fence->context, &fence->ccif_after);
	if (ret)
		goto out_fence;
	ret = fence->ops->ccif_verify_cleared(fence->context,
			&fence->ccif_before, &fence->ccif_after);
	if (ret)
		goto out_fence;

	/* This future backend must keep reset asserted until this core returns. */
	power_may_be_on = true;
	ret = fence->ops->power_on_reset_held(fence->context);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_POWERED_RESET_HELD;

	/* Vendor md_cd_power_on() programs INFRA_AO_MD_SRCCLKENA before PLL init. */
	ret = fence->ops->enable_srcclk(fence->context);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_SRC_CLK_ENABLED;

	for (attempt = 0; attempt < MT6797_MD1_BRINGUP_MAX_PLL_POLLS; attempt++) {
		ret = fence->ops->pll_poll(fence->context);
		if (!ret)
			break;
		if (ret != -EAGAIN)
			goto out_fence;
	}
	if (ret == -EAGAIN) {
		ret = -ETIMEDOUT;
		goto out_fence;
	}
	fence->state = MT6797_MD1_BRINGUP_PLL_READY;

	ret = fence->ops->apply_stage1_mpu(fence->context, &fence->plan,
			fence->stage1_mpu,
			MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES);
	if (ret)
		goto out_fence;
	fence->state = MT6797_MD1_BRINGUP_STAGE1_MPU_APPLIED;

	/* Deliberately terminal: md_cd_let_md_go() has no representation here. */
	fence->state = MT6797_MD1_BRINGUP_READY_FOR_BOOT_VECTOR;
	ret = 0;
	goto out_unlock;

out_fence:
	ret = mt6797_md1_bringup_fence_fail(fence, power_may_be_on,
					      remap_may_be_changed, mpu_may_be_changed,
					      ret);
out_unlock:
	mutex_unlock(&fence->lock);
	return ret;
}

enum mt6797_md1_bringup_state
mt6797_md1_bringup_fence_state(struct mt6797_md1_bringup_fence *fence)
{
	enum mt6797_md1_bringup_state state;

	if (!fence)
		return MT6797_MD1_BRINGUP_FENCED;
	mutex_lock(&fence->lock);
	state = fence->state;
	mutex_unlock(&fence->lock);
	return state;
}
