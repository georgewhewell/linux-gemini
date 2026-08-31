// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MediaTek MT6797 Cortex-A72 platform-state source
 *
 * Copyright (c) 2026 Julien Etienne
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mt6797-a72-provider.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-a72-platform-state.h>

#include "mt6797-a72-platform-state-internal.h"

#define MT6797_SPM_PWR_STATUS			0x180
#define MT6797_SPM_PWR_STATUS_2ND		0x184
#define MT6797_SPM_CPU_PWR_STATUS		0x188
#define MT6797_SPM_CPU_PWR_STATUS_2ND		0x18c
#define MT6797_SPM_MP2_CPUSYS_PWR_CON		0x218
#define MT6797_SPM_MP2_CPU0_PWR_CON		0x240
#define MT6797_SPM_MP2_CPU1_PWR_CON		0x244
#define MT6797_SPM_CPU_EXT_BUCK_ISO		0x290

/* Legacy global status is retained as a transition-race diagnostic only.
 * MP2 CPU8/9 reset proof uses the dedicated CPU0/1 PWR_CON registers. */
#define MT6797_A72_LEGACY_PWR_STATUS_MASK	(BIT(20) | BIT(19) | BIT(16))

#define MT6797_MCUCFG_MP2_SYNC_DCM		0x274
#define MT6797_MCUCFG_MP2_SYNC_DCM_MASK		GENMASK(6, 0)

#define MT6797_CCI_STATUS			0x000c
#define MT6797_CCI_CHANGE_PENDING		BIT(0)
#define MT6797_CCI_MP2_PORT_CONTROL		0x6000
#define MT6797_CCI_MP2_REQUEST_MASK		GENMASK(1, 0)

struct mt6797_a72_platform_state_source {
	struct regmap *spm;
	struct reset_control *pwrap_reset;
	void __iomem *mcucfg;
	void __iomem *cci;
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_PLATFORM_EFFECTS)
	struct mt6797_a72_platform_effect_owner effect_owner;
#endif
	struct mutex lock; /* Serializes snapshots and platform effects. */
};

static int mt6797_state_read_spm(struct mt6797_a72_platform_state_source *source,
				 struct mt6797_a72_platform_state *sample)
{
	int ret;

	ret = regmap_read(source->spm, MT6797_SPM_PWR_STATUS,
			  &sample->spm_pwr_status);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_PWR_STATUS_2ND,
			  &sample->spm_pwr_status_2nd);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_CPU_PWR_STATUS,
			  &sample->spm_cpu_pwr_status);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_CPU_PWR_STATUS_2ND,
			  &sample->spm_cpu_pwr_status_2nd);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_MP2_CPUSYS_PWR_CON,
			  &sample->spm_mp2_cpusys_pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_MP2_CPU0_PWR_CON,
			  &sample->spm_mp2_cpu0_pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_MP2_CPU1_PWR_CON,
			  &sample->spm_mp2_cpu1_pwr_con);
	if (ret)
		return ret;
	ret = regmap_read(source->spm, MT6797_SPM_CPU_EXT_BUCK_ISO,
			  &sample->spm_cpu_ext_buck_iso);
	if (ret)
		return ret;

	return 0;
}

static int mt6797_state_read_once(struct mt6797_a72_platform_state_source *source,
				  struct mt6797_a72_platform_state *sample)
{
	int reset_state;
	int ret;

	*sample = (struct mt6797_a72_platform_state){};
	ret = mt6797_state_read_spm(source, sample);
	if (ret)
		return ret;

	reset_state = reset_control_status(source->pwrap_reset);
	if (reset_state < 0)
		return reset_state;
	sample->pwrap_reset_asserted = reset_state;

	sample->mp2_sync_dcm = readl(source->mcucfg +
				       MT6797_MCUCFG_MP2_SYNC_DCM);
	sample->cci_status_before = readl(source->cci + MT6797_CCI_STATUS);
	sample->cci_mp2_port_control = readl(source->cci +
					      MT6797_CCI_MP2_PORT_CONTROL);
	sample->cci_status_after = readl(source->cci + MT6797_CCI_STATUS);

	return 0;
}

static u32
mt6797_state_movement_mask(const struct mt6797_a72_platform_state *first,
			   const struct mt6797_a72_platform_state *second)
{
	u32 movement = 0;

	if ((first->spm_pwr_status ^ second->spm_pwr_status) &
	    MT6797_A72_LEGACY_PWR_STATUS_MASK)
		movement |= MT6797_A72_PLATFORM_MOVED_SPM_PWR_STATUS;
	if ((first->spm_pwr_status_2nd ^ second->spm_pwr_status_2nd) &
	    MT6797_A72_LEGACY_PWR_STATUS_MASK)
		movement |= MT6797_A72_PLATFORM_MOVED_SPM_PWR_STATUS_2ND;
	if (first->spm_mp2_cpusys_pwr_con != second->spm_mp2_cpusys_pwr_con)
		movement |= MT6797_A72_PLATFORM_MOVED_MP2_CPUSYS_PWR_CON;
	if (first->spm_mp2_cpu0_pwr_con != second->spm_mp2_cpu0_pwr_con)
		movement |= MT6797_A72_PLATFORM_MOVED_MP2_CPU0_PWR_CON;
	if (first->spm_mp2_cpu1_pwr_con != second->spm_mp2_cpu1_pwr_con)
		movement |= MT6797_A72_PLATFORM_MOVED_MP2_CPU1_PWR_CON;
	if (first->spm_cpu_ext_buck_iso != second->spm_cpu_ext_buck_iso)
		movement |= MT6797_A72_PLATFORM_MOVED_CPU_EXT_BUCK_ISO;
	if ((first->mp2_sync_dcm ^ second->mp2_sync_dcm) &
	    MT6797_MCUCFG_MP2_SYNC_DCM_MASK)
		movement |= MT6797_A72_PLATFORM_MOVED_MP2_SYNC_DCM;
	if ((first->cci_mp2_port_control ^ second->cci_mp2_port_control) &
	    MT6797_CCI_MP2_REQUEST_MASK)
		movement |= MT6797_A72_PLATFORM_MOVED_CCI_MP2_PORT;
	if (first->pwrap_reset_asserted != second->pwrap_reset_asserted)
		movement |= MT6797_A72_PLATFORM_MOVED_PWRAP_RESET;

	return movement;
}

static bool mt6797_state_cci_busy(const struct mt6797_a72_platform_state *sample)
{
	return (sample->cci_status_before | sample->cci_status_after) &
		MT6797_CCI_CHANGE_PENDING;
}

int mt6797_a72_platform_state_capture(const struct mt6797_state_capture_ops *ops,
				      void *context,
				      struct mt6797_a72_platform_state *snapshot,
				      struct mt6797_a72_platform_state_failure *failure)
{
	struct mt6797_a72_platform_state first;
	struct mt6797_a72_platform_state second;
	u32 movement;
	int ret;

	if (!snapshot || !failure)
		return -EINVAL;
	*snapshot = (struct mt6797_a72_platform_state){};
	*failure = (struct mt6797_a72_platform_state_failure){};
	if (!ops || !ops->read_once)
		return -EINVAL;

	ret = ops->read_once(context, &first);
	if (ret)
		return ret;
	ret = ops->read_once(context, &second);
	if (ret)
		return ret;

	movement = mt6797_state_movement_mask(&first, &second);
	if (mt6797_state_cci_busy(&first) ||
	    mt6797_state_cci_busy(&second) || movement) {
		failure->first = first;
		failure->second = second;
		failure->movement_mask = movement;
		failure->samples_valid = true;
	}
	if (mt6797_state_cci_busy(&first) || mt6797_state_cci_busy(&second))
		return -EBUSY;
	if (movement)
		return -EAGAIN;

	*snapshot = second;
	snapshot->valid = true;
	return 0;
}

static int mt6797_state_read_source(void *context,
				    struct mt6797_a72_platform_state *sample)
{
	return mt6797_state_read_once(context, sample);
}

static const struct mt6797_state_capture_ops
mt6797_state_capture_ops = {
	.read_once = mt6797_state_read_source,
};

int mt6797_a72_platform_state_snapshot_detailed(struct device *dev,
						struct mt6797_a72_platform_state *snapshot,
						struct mt6797_a72_platform_state_failure *failure)
{
	struct mt6797_a72_platform_state_source *source;
	int ret;

	if (!snapshot || !failure)
		return -EINVAL;
	*snapshot = (struct mt6797_a72_platform_state){};
	*failure = (struct mt6797_a72_platform_state_failure){};
	if (!dev)
		return -EINVAL;

	source = dev_get_drvdata(dev);
	if (!source)
		return -ENODEV;

	mutex_lock(&source->lock);
	ret = mt6797_a72_platform_state_capture(&mt6797_state_capture_ops,
						source, snapshot, failure);
	mutex_unlock(&source->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_state_snapshot_detailed);

int mt6797_a72_platform_state_snapshot(struct device *dev,
				       struct mt6797_a72_platform_state *snapshot)
{
	struct mt6797_a72_platform_state_failure failure;

	return mt6797_a72_platform_state_snapshot_detailed(dev, snapshot,
						     &failure);
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_state_snapshot);

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_PLATFORM_EFFECTS)
static bool
mt6797_a72_platform_effect_ops_valid(const struct mt6797_a72_platform_effect_ops *ops)
{
	return ops && ops->spm_read && ops->spm_update_bits && ops->mcucfg_read &&
	       ops->mcucfg_write && ops->pwrap_assert && ops->pwrap_deassert && ops->pwrap_status &&
	       ops->delay;
}

static bool
mt6797_a72_platform_effect_handle_valid(const struct mt6797_a72_platform_effect_handle *handle)
{
	return handle && handle->attempt_id && handle->cookie;
}

static bool
mt6797_a72_platform_effect_handle_matches(const struct mt6797_a72_platform_effect_handle *left,
					  const struct mt6797_a72_platform_effect_handle *right)
{
	return mt6797_a72_platform_effect_handle_valid(left) &&
	       mt6797_a72_platform_effect_handle_valid(right) &&
	       left->attempt_id == right->attempt_id && left->cookie == right->cookie;
}

static bool
mt6797_a72_platform_effect_provider_matches(const struct mt6797_a72_platform_effect_handle *handle,
					    const struct mt6797_a72_provider_handle *provider)
{
	return provider && provider->generation == handle->attempt_id &&
	       provider->cookie == handle->cookie;
}

static void
mt6797_a72_platform_effect_output(const struct mt6797_a72_platform_effect_owner *owner,
				  struct mt6797_a72_platform_effect_result *result)
{
	*result = owner->result;
}

static int
mt6797_a72_effect_fail(struct mt6797_a72_platform_effect_owner *owner,
		       enum mt6797_a72_platform_effect_state state, int error,
		       struct mt6797_a72_platform_effect_result *result)
{
	owner->state = state;
	owner->result.error = error;
	owner->result.sealed = true;
	mt6797_a72_platform_effect_output(owner, result);
	return error;
}

static int
mt6797_a72_platform_effect_read_exact_spm(struct mt6797_a72_platform_effect_owner *owner,
					  const struct mt6797_a72_platform_effect_ops *ops,
					  void *context, u32 offset, u32 expected, u32 *observed,
					  struct mt6797_a72_platform_effect_result *result)
{
	int ret;

	ret = ops->spm_read(context, offset, observed);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	if (*observed != expected)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -ERANGE,
						       result);
	return 0;
}

int
mt6797_a72_platform_effect_owner_p27_acquire(struct mt6797_a72_platform_effect_owner *owner,
					     const struct mt6797_a72_platform_effect_ops *ops,
					     void *context,
					     const struct mt6797_a72_platform_effect_handle *handle,
					     struct mt6797_a72_platform_effect_result *result)
{
	u32 value;
	int status;
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_a72_platform_effect_result){};
	if (!owner || !mt6797_a72_platform_effect_ops_valid(ops) ||
	    !mt6797_a72_platform_effect_handle_valid(handle))
		return -EINVAL;
	if (owner->state != MT6797_A72_EFFECT_STATE_UNUSED) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EALREADY;
	}

	owner->state = MT6797_A72_EFFECT_STATE_P27_INFLIGHT;
	owner->handle = *handle;
	owner->result = (struct mt6797_a72_platform_effect_result){
		.abi = MT6797_A72_PLATFORM_EFFECT_ABI,
		.operation = MT6797_A72_PLATFORM_EFFECT_P27_ACQUIRE,
	};
	ret = ops->spm_read(context, MT6797_A72_EFFECT_SPM_P27, &value);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_REJECTED, ret,
						       result);
	owner->result.spm_p27_before = value;
	if (value != MT6797_A72_EFFECT_P27_BEFORE)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_REJECTED, -ERANGE,
						       result);

	owner->result.p27_owned = true;
	owner->result.attempted_effects |= MT6797_A72_EFFECT_P27_RESET_RELEASED;
	ret = ops->spm_update_bits(context, MT6797_A72_EFFECT_SPM_P27, BIT(0), BIT(0));
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_P27,
							MT6797_A72_EFFECT_P27_HELD,
							&owner->result.spm_p27_after,
							result);
	if (ret)
		return ret;
	owner->result.completed_effects |= MT6797_A72_EFFECT_P27_RESET_RELEASED;

	owner->result.attempted_effects |= MT6797_A72_EFFECT_BPLL_ORDER_READ;
	ret = ops->mcucfg_read(context, MT6797_A72_EFFECT_MCUCFG_BPLL,
			       &owner->result.bpll_ordering_value);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_BPLL_ORDER_READ;

	owner->result.attempted_effects |= MT6797_A72_EFFECT_PWRAP_ASSERTED;
	ret = ops->pwrap_assert(context);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	status = ops->pwrap_status(context);
	if (status < 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, status,
						       result);
	if (status != 1)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_PWRAP_ASSERTED;
	owner->state = MT6797_A72_EFFECT_STATE_P27_HELD;
	mt6797_a72_platform_effect_output(owner, result);
	return 0;
}

int
mt6797_a72_platform_effect_owner_p27_release(struct mt6797_a72_platform_effect_owner *owner,
					     const struct mt6797_a72_platform_effect_ops *ops,
					     void *context,
					     const struct mt6797_a72_platform_effect_handle *handle,
					     struct mt6797_a72_platform_effect_result *result)
{
	u32 value;
	int status;
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_a72_platform_effect_result){};
	if (!owner || !mt6797_a72_platform_effect_ops_valid(ops) ||
	    !mt6797_a72_platform_effect_handle_valid(handle))
		return -EINVAL;
	if (!mt6797_a72_platform_effect_handle_matches(&owner->handle, handle)) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EPERM;
	}
	if (owner->state != MT6797_A72_EFFECT_STATE_P27_HELD) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EALREADY;
	}

	owner->state = MT6797_A72_EFFECT_STATE_RELEASE_INFLIGHT;
	owner->result.operation = MT6797_A72_PLATFORM_EFFECT_P27_RELEASE;
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_P27,
							MT6797_A72_EFFECT_P27_HELD, &value, result);
	if (ret)
		return ret;
	status = ops->pwrap_status(context);
	if (status < 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, status,
						       result);
	if (status != 1)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);

	owner->result.attempted_effects |= MT6797_A72_EFFECT_PWRAP_DEASSERTED;
	ret = ops->pwrap_deassert(context);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	status = ops->pwrap_status(context);
	if (status < 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, status,
						       result);
	if (status != 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_PWRAP_DEASSERTED;

	owner->result.attempted_effects |= MT6797_A72_EFFECT_P27_RESET_RESTORED;
	ret = ops->spm_update_bits(context, MT6797_A72_EFFECT_SPM_P27, BIT(0), 0);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_P27,
							MT6797_A72_EFFECT_P27_BEFORE,
							&value, result);
	if (ret)
		return ret;
	owner->result.completed_effects |= MT6797_A72_EFFECT_P27_RESET_RESTORED;
	owner->result.p27_owned = false;
	owner->result.error = 0;
	owner->result.sealed = true;
	owner->state = MT6797_A72_EFFECT_STATE_RELEASED;
	mt6797_a72_platform_effect_output(owner, result);
	return 0;
}

int
mt6797_a72_effect_owner_isolate(struct mt6797_a72_platform_effect_owner *owner,
				const struct mt6797_a72_platform_effect_ops *ops, void *context,
				const struct mt6797_a72_platform_effect_handle *handle,
				const struct mt6797_a72_provider_handle *provider,
				struct mt6797_a72_platform_effect_result *result)
{
	u32 value;
	int status;
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_a72_platform_effect_result){};
	if (!owner || !mt6797_a72_platform_effect_ops_valid(ops) ||
	    !mt6797_a72_platform_effect_handle_valid(handle))
		return -EINVAL;
	if (!mt6797_a72_platform_effect_handle_matches(&owner->handle, handle) ||
	    !mt6797_a72_platform_effect_provider_matches(handle, provider)) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EPERM;
	}
	if (owner->state != MT6797_A72_EFFECT_STATE_P27_HELD) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EALREADY;
	}

	owner->state = MT6797_A72_EFFECT_STATE_ISOLATION_INFLIGHT;
	owner->result.operation = MT6797_A72_PLATFORM_EFFECT_ISOLATION_CLEAR;
	owner->result.isolation_attempted = true;
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_P27,
							MT6797_A72_EFFECT_P27_HELD, &value, result);
	if (ret)
		return ret;
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_ISOLATION,
							MT6797_A72_EFFECT_ISOLATION_BEFORE,
							&owner->result.isolation_before,
							result);
	if (ret)
		return ret;
	status = ops->pwrap_status(context);
	if (status < 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, status,
						       result);
	if (status != 1)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);

	owner->result.attempted_effects |= MT6797_A72_EFFECT_ISOLATION_CLEARED;
	ret = ops->spm_update_bits(context, MT6797_A72_EFFECT_SPM_ISOLATION,
				   MT6797_A72_EFFECT_ISOLATION_MASK, 0);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	ret = mt6797_a72_platform_effect_read_exact_spm(owner, ops, context,
							MT6797_A72_EFFECT_SPM_ISOLATION,
							MT6797_A72_EFFECT_ISOLATION_AFTER,
							&owner->result.isolation_after,
							result);
	if (ret)
		return ret;
	owner->result.completed_effects |= MT6797_A72_EFFECT_ISOLATION_CLEARED;
	owner->result.isolation_crossed = true;

	owner->result.attempted_effects |= MT6797_A72_EFFECT_PWRAP_DEASSERTED;
	ret = ops->pwrap_deassert(context);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	status = ops->pwrap_status(context);
	if (status < 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, status,
						       result);
	if (status != 0)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_PWRAP_DEASSERTED;

	owner->result.attempted_effects |= MT6797_A72_EFFECT_ISOLATION_GUARD;
	ops->delay(context, MT6797_A72_EFFECT_GUARD_MIN_US, MT6797_A72_EFFECT_GUARD_MAX_US);
	owner->result.completed_effects |= MT6797_A72_EFFECT_ISOLATION_GUARD;
	owner->state = MT6797_A72_EFFECT_STATE_ISOLATED;
	mt6797_a72_platform_effect_output(owner, result);
	return 0;
}

int
mt6797_a72_platform_effect_owner_dcm_update(struct mt6797_a72_platform_effect_owner *owner,
					    const struct mt6797_a72_platform_effect_ops *ops,
					    void *context,
					    const struct mt6797_a72_platform_effect_handle *handle,
					    bool cpu8_online, bool cpu9_online,
					    struct mt6797_a72_platform_effect_result *result)
{
	u32 expected;
	u32 value;
	int ret;

	if (!result)
		return -EINVAL;
	*result = (struct mt6797_a72_platform_effect_result){};
	if (!owner || !mt6797_a72_platform_effect_ops_valid(ops) ||
	    !mt6797_a72_platform_effect_handle_valid(handle))
		return -EINVAL;
	if (!mt6797_a72_platform_effect_handle_matches(&owner->handle, handle)) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EPERM;
	}
	if (owner->state != MT6797_A72_EFFECT_STATE_ISOLATED) {
		mt6797_a72_platform_effect_output(owner, result);
		return -EALREADY;
	}

	owner->state = MT6797_A72_EFFECT_STATE_DCM_INFLIGHT;
	owner->result.operation = MT6797_A72_PLATFORM_EFFECT_DCM_UPDATE;
	if (!cpu8_online || cpu9_online)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EPERM,
						       result);
	ret = ops->mcucfg_read(context, MT6797_A72_EFFECT_MCUCFG_DCM, &owner->result.dcm_before);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	if (owner->result.dcm_before & MT6797_A72_EFFECT_DCM_MASK)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -ERANGE,
						       result);

	expected = owner->result.dcm_before & ~MT6797_A72_EFFECT_DCM_MASK;
	expected |= MT6797_A72_EFFECT_DCM_TOGGLE_VALUE;
	owner->result.attempted_effects |= MT6797_A72_EFFECT_DCM_TOGGLE;
	ops->mcucfg_write(context, MT6797_A72_EFFECT_MCUCFG_DCM, expected);
	ret = ops->mcucfg_read(context, MT6797_A72_EFFECT_MCUCFG_DCM, &value);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	owner->result.dcm_toggle = value;
	if (value != expected)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_DCM_TOGGLE;

	expected &= ~BIT(1);
	owner->result.attempted_effects |= MT6797_A72_EFFECT_DCM_FINAL;
	ops->mcucfg_write(context, MT6797_A72_EFFECT_MCUCFG_DCM, expected);
	ret = ops->mcucfg_read(context, MT6797_A72_EFFECT_MCUCFG_DCM, &value);
	if (ret)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, ret,
						       result);
	owner->result.dcm_final = value;
	if (value != expected)
		return mt6797_a72_effect_fail(owner, MT6797_A72_EFFECT_STATE_FAULTED, -EIO,
						       result);
	owner->result.completed_effects |= MT6797_A72_EFFECT_DCM_FINAL;
	owner->result.error = 0;
	owner->result.sealed = true;
	owner->state = MT6797_A72_EFFECT_STATE_COMPLETE;
	mt6797_a72_platform_effect_output(owner, result);
	return 0;
}

static int
mt6797_platform_effect_spm_read(void *context, u32 offset, u32 *value)
{
	struct mt6797_a72_platform_state_source *source = context;

	return regmap_read(source->spm, offset, value);
}

static int
mt6797_platform_effect_spm_update_bits(void *context, u32 offset, u32 mask, u32 value)
{
	struct mt6797_a72_platform_state_source *source = context;

	return regmap_update_bits(source->spm, offset, mask, value);
}

static int
mt6797_platform_effect_mcucfg_read(void *context, u32 offset, u32 *value)
{
	struct mt6797_a72_platform_state_source *source = context;

	*value = readl(source->mcucfg + offset);
	return 0;
}

static void
mt6797_platform_effect_mcucfg_write(void *context, u32 offset, u32 value)
{
	struct mt6797_a72_platform_state_source *source = context;

	writel(value, source->mcucfg + offset);
}

static int
mt6797_platform_effect_pwrap_assert(void *context)
{
	struct mt6797_a72_platform_state_source *source = context;

	return reset_control_assert(source->pwrap_reset);
}

static int
mt6797_platform_effect_pwrap_deassert(void *context)
{
	struct mt6797_a72_platform_state_source *source = context;

	return reset_control_deassert(source->pwrap_reset);
}

static int
mt6797_platform_effect_pwrap_status(void *context)
{
	struct mt6797_a72_platform_state_source *source = context;

	return reset_control_status(source->pwrap_reset);
}

static void
mt6797_platform_effect_delay(void *context, unsigned int min_us, unsigned int max_us)
{
	(void)context;
	usleep_range(min_us, max_us);
}

static const struct mt6797_a72_platform_effect_ops mt6797_platform_effect_ops = {
	.spm_read = mt6797_platform_effect_spm_read,
	.spm_update_bits = mt6797_platform_effect_spm_update_bits,
	.mcucfg_read = mt6797_platform_effect_mcucfg_read,
	.mcucfg_write = mt6797_platform_effect_mcucfg_write,
	.pwrap_assert = mt6797_platform_effect_pwrap_assert,
	.pwrap_deassert = mt6797_platform_effect_pwrap_deassert,
	.pwrap_status = mt6797_platform_effect_pwrap_status,
	.delay = mt6797_platform_effect_delay,
};

static struct mt6797_a72_platform_state_source *
mt6797_platform_effect_source(struct device *dev, struct mt6797_a72_platform_effect_result *result)
{
	if (result)
		*result = (struct mt6797_a72_platform_effect_result){};
	if (!dev || !result)
		return NULL;
	return dev_get_drvdata(dev);
}

int
mt6797_a72_platform_effect_p27_acquire(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result)
{
	struct mt6797_a72_platform_state_source *source;
	int ret;

	source = mt6797_platform_effect_source(dev, result);
	if (!source)
		return dev && result ? -ENODEV : -EINVAL;
	mutex_lock(&source->lock);
	ret = mt6797_a72_platform_effect_owner_p27_acquire(&source->effect_owner,
							   &mt6797_platform_effect_ops,
							   source, handle, result);
	mutex_unlock(&source->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_effect_p27_acquire);

int
mt6797_a72_platform_effect_p27_release(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result)
{
	struct mt6797_a72_platform_state_source *source;
	int ret;

	source = mt6797_platform_effect_source(dev, result);
	if (!source)
		return dev && result ? -ENODEV : -EINVAL;
	mutex_lock(&source->lock);
	ret = mt6797_a72_platform_effect_owner_p27_release(&source->effect_owner,
							   &mt6797_platform_effect_ops,
							   source, handle, result);
	mutex_unlock(&source->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_effect_p27_release);

int
mt6797_a72_platform_effect_isolation_clear(struct device *dev,
					   const struct mt6797_a72_platform_effect_handle *handle,
					   const struct mt6797_a72_provider_handle *provider,
					   struct mt6797_a72_platform_effect_result *result)
{
	struct mt6797_a72_platform_state_source *source;
	int ret;

	source = mt6797_platform_effect_source(dev, result);
	if (!source)
		return dev && result ? -ENODEV : -EINVAL;
	mutex_lock(&source->lock);
	ret = mt6797_a72_effect_owner_isolate(&source->effect_owner, &mt6797_platform_effect_ops,
					      source, handle, provider, result);
	mutex_unlock(&source->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_effect_isolation_clear);

int
mt6797_a72_platform_effect_dcm_update(struct device *dev,
				      const struct mt6797_a72_platform_effect_handle *handle,
				      bool cpu8_online, bool cpu9_online,
				      struct mt6797_a72_platform_effect_result *result)
{
	struct mt6797_a72_platform_state_source *source;
	int ret;

	source = mt6797_platform_effect_source(dev, result);
	if (!source)
		return dev && result ? -ENODEV : -EINVAL;
	mutex_lock(&source->lock);
	ret = mt6797_a72_platform_effect_owner_dcm_update(&source->effect_owner,
							  &mt6797_platform_effect_ops, source,
							  handle, cpu8_online, cpu9_online, result);
	mutex_unlock(&source->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_platform_effect_dcm_update);
#endif

static int mt6797_a72_platform_state_probe(struct platform_device *pdev)
{
	struct mt6797_a72_platform_state_source *source;
	struct device *dev = &pdev->dev;

	source = devm_kzalloc(dev, sizeof(*source), GFP_KERNEL);
	if (!source)
		return -ENOMEM;

	source->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(source->spm))
		return dev_err_probe(dev, PTR_ERR(source->spm),
				     "failed to get SPM syscon\n");

	source->pwrap_reset = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(source->pwrap_reset))
		return dev_err_probe(dev, PTR_ERR(source->pwrap_reset),
				     "failed to get PWRAP reset\n");

	source->mcucfg = devm_platform_ioremap_resource_byname(pdev, "mcucfg");
	if (IS_ERR(source->mcucfg))
		return PTR_ERR(source->mcucfg);
	source->cci = devm_platform_ioremap_resource_byname(pdev, "cci");
	if (IS_ERR(source->cci))
		return PTR_ERR(source->cci);

	mutex_init(&source->lock);
	platform_set_drvdata(pdev, source);
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_PLATFORM_EFFECTS)
	dev_info(dev, "platform source and effects ready; no caller\n");
#else
	dev_info(dev, "read-only capture source ready; no lifecycle caller\n");
#endif

	return 0;
}

static const struct of_device_id mt6797_a72_platform_state_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-platform-state" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_a72_platform_state_of_match);

static struct platform_driver mt6797_a72_platform_state_driver = {
	.probe = mt6797_a72_platform_state_probe,
	.driver = {
		.name = "mt6797-a72-platform-state",
		.of_match_table = mt6797_a72_platform_state_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_platform_state_driver);

MODULE_DESCRIPTION("MediaTek MT6797 Cortex-A72 platform-state source");
MODULE_LICENSE("GPL");
