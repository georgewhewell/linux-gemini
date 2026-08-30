// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 CONSYS identity transaction core.
 *
 * This intentionally has no platform backend.  In particular, the disabled
 * inventory driver's resources are not mapped here.  A later backend must
 * provide reviewed regulator, clock, reset, and power-domain operations.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-consys.h>

static void mt6797_consys_delay(struct mt6797_consys_lifecycle *lifecycle,
				u32 interval)
{
	u32 slack = min(interval / 4, 50U);

	if (interval > U32_MAX - slack)
		slack = U32_MAX - interval;
	if (lifecycle->ops->delay_us)
		lifecycle->ops->delay_us(lifecycle->context, interval,
					 interval + slack);
	else
		usleep_range(interval, interval + slack);
}

static int mt6797_consys_wait(struct mt6797_consys_lifecycle *lifecycle,
			      enum mt6797_consys_poll poll, u32 timeout_us)
{
	u32 interval = lifecycle->timeouts.poll_interval_us;
	u32 remaining_us = timeout_us;
	u32 delay_us;
	int ret;

	for (;;) {
		ret = lifecycle->ops->poll(lifecycle->context, poll);
		if (!ret)
			return 0;
		if (ret != -EAGAIN)
			return ret;
		if (!remaining_us)
			return -ETIMEDOUT;

		delay_us = min(interval, remaining_us);
		mt6797_consys_delay(lifecycle, delay_us);
		remaining_us -= delay_us;
	}
}

/* Unwind only voted resources, preserving the first failure. */
static int mt6797_consys_identity_unwind(struct mt6797_consys_lifecycle *lifecycle,
					 bool reset, bool scp_sys_conn, bool vcn28,
					 bool vcn18)
{
	int ret;
	int first_error = 0;

	if (reset) {
		ret = lifecycle->ops->assert_reset(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	if (scp_sys_conn) {
		ret = lifecycle->ops->disable_scp_sys_conn(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	if (vcn28) {
		ret = lifecycle->ops->disable_vcn28(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	if (vcn18) {
		ret = lifecycle->ops->disable_vcn18(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	return first_error;
}

static int
mt6797_consys_identity_acquire(struct mt6797_consys_lifecycle *lifecycle)
{
	bool vcn18 = false;
	bool vcn28 = false;
	bool scp_sys_conn = false;
	bool reset = false;
	int unwind_ret;
	int ret;

	vcn18 = true;
	ret = lifecycle->ops->enable_vcn18(lifecycle->context);
	if (ret)
		goto unwind;

	mt6797_consys_delay(lifecycle, MT6797_CONSYS_VCN18_SETTLE_US);
	vcn28 = true;
	ret = lifecycle->ops->enable_vcn28(lifecycle->context);
	if (ret)
		goto unwind;

	ret = lifecycle->ops->configure_sleep_mask(lifecycle->context);
	if (ret)
		goto unwind;

	reset = true;
	ret = lifecycle->ops->assert_reset(lifecycle->context);
	if (ret)
		goto unwind;

	ret = lifecycle->ops->configure_pwron(lifecycle->context);
	if (ret)
		goto unwind;

	scp_sys_conn = true;
	ret = lifecycle->ops->enable_scp_sys_conn(lifecycle->context);
	if (ret)
		goto unwind;

	mt6797_consys_delay(lifecycle, MT6797_CONSYS_DOMAIN_SETTLE_US);

	ret = mt6797_consys_wait(lifecycle,
				 MT6797_CONSYS_POLL_CHIP_READY,
				 lifecycle->timeouts.chip_ready_timeout_us);
	if (ret)
		goto unwind;

	return 0;

unwind:
	unwind_ret = mt6797_consys_identity_unwind(lifecycle, reset,
						   scp_sys_conn, vcn28, vcn18);
	if (unwind_ret)
		lifecycle->state = MT6797_CONSYS_ERROR;
	return ret;
}

static bool mt6797_consys_ops_valid(const struct mt6797_consys_ops *ops)
{
	return ops && ops->abi == MT6797_CONSYS_LIFECYCLE_ABI &&
		ops->enable_vcn18 && ops->disable_vcn18 &&
		ops->enable_vcn28 && ops->disable_vcn28 &&
		ops->configure_sleep_mask && ops->assert_reset &&
		ops->configure_pwron && ops->enable_scp_sys_conn &&
		ops->disable_scp_sys_conn && ops->poll;
}

static bool
mt6797_consys_timeouts_valid(const struct mt6797_consys_timeouts *timeouts)
{
	return timeouts && timeouts->poll_interval_us &&
		timeouts->chip_ready_timeout_us >= timeouts->poll_interval_us;
}

static bool mt6797_consys_runtime_lease_valid_locked(
		const struct mt6797_consys_runtime_lease *lease)
{
	struct mt6797_consys_lifecycle *lifecycle;

	if (!lease || !lease->lifecycle)
		return false;
	lifecycle = lease->lifecycle;
	return lifecycle->runtime_lease == lease &&
		lifecycle->runtime_generation == lease->generation &&
		lifecycle->state == MT6797_CONSYS_IDENTITY_HELD &&
		lifecycle->users == 1;
}

int mt6797_consys_lifecycle_init(struct mt6797_consys_lifecycle *lifecycle,
				 const struct mt6797_consys_ops *ops, void *context,
				 const struct mt6797_consys_timeouts *timeouts)
{
	if (!lifecycle || !mt6797_consys_ops_valid(ops) ||
	    !mt6797_consys_timeouts_valid(timeouts))
		return -EINVAL;

	lifecycle->ops = ops;
	lifecycle->context = context;
	lifecycle->timeouts = *timeouts;
	lifecycle->state = MT6797_CONSYS_OFF;
	lifecycle->users = 0;
	lifecycle->runtime_lease = NULL;
	lifecycle->runtime_pins = 0;
	lifecycle->runtime_generation = 0;
	mutex_init(&lifecycle->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_lifecycle_init);

int mt6797_consys_identity_get(struct mt6797_consys_lifecycle *lifecycle)
{
	int ret = 0;

	if (!lifecycle)
		return -EINVAL;

	mutex_lock(&lifecycle->lock);
	if (lifecycle->state == MT6797_CONSYS_ERROR) {
		ret = -EIO;
	} else if (lifecycle->runtime_lease) {
		/* A long-lived owner excludes unrelated short identity votes. */
		ret = -EBUSY;
	} else if (lifecycle->state == MT6797_CONSYS_IDENTITY_HELD) {
		if (lifecycle->users == U32_MAX)
			ret = -EOVERFLOW;
		else
			lifecycle->users++;
	} else {
		ret = mt6797_consys_identity_acquire(lifecycle);
		if (!ret) {
			lifecycle->state = MT6797_CONSYS_IDENTITY_HELD;
			lifecycle->users = 1;
		} else if (lifecycle->state != MT6797_CONSYS_ERROR) {
			lifecycle->state = MT6797_CONSYS_OFF;
		}
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_identity_get);

int mt6797_consys_identity_put(struct mt6797_consys_lifecycle *lifecycle)
{
	int ret = 0;

	if (!lifecycle)
		return -EINVAL;

	mutex_lock(&lifecycle->lock);
	if (lifecycle->state == MT6797_CONSYS_ERROR) {
		ret = -EIO;
	} else if (lifecycle->runtime_lease) {
		ret = -EBUSY;
	} else if (lifecycle->state != MT6797_CONSYS_IDENTITY_HELD ||
		   !lifecycle->users) {
		ret = -EINVAL;
	} else if (--lifecycle->users) {
		/* The final voter owns the physical identity cleanup. */
	} else {
		ret = mt6797_consys_identity_unwind(lifecycle, true, true, true,
						    true);
		lifecycle->state = ret ? MT6797_CONSYS_ERROR : MT6797_CONSYS_OFF;
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_identity_put);

int mt6797_consys_runtime_lease_acquire(
		struct mt6797_consys_lifecycle *lifecycle,
		struct mt6797_consys_runtime_lease *lease)
{
	int ret = 0;

	if (!lifecycle || !lease || lease->lifecycle)
		return -EINVAL;

	mutex_lock(&lifecycle->lock);
	if (lifecycle->state == MT6797_CONSYS_ERROR) {
		ret = -EIO;
	} else if (lifecycle->runtime_lease || lifecycle->users ||
		   lifecycle->state != MT6797_CONSYS_OFF) {
		ret = -EBUSY;
	} else if (lifecycle->runtime_generation == U64_MAX) {
		ret = -EOVERFLOW;
	} else {
		ret = mt6797_consys_identity_acquire(lifecycle);
		if (!ret) {
			lifecycle->state = MT6797_CONSYS_IDENTITY_HELD;
			lifecycle->users = 1;
			lifecycle->runtime_generation++;
			lease->lifecycle = lifecycle;
			lease->generation = lifecycle->runtime_generation;
			lifecycle->runtime_lease = lease;
		} else if (lifecycle->state != MT6797_CONSYS_ERROR) {
			lifecycle->state = MT6797_CONSYS_OFF;
		}
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_lease_acquire);

int mt6797_consys_runtime_lease_release(
		struct mt6797_consys_runtime_lease *lease)
{
	struct mt6797_consys_lifecycle *lifecycle;
	int ret = 0;

	if (!lease || !lease->lifecycle)
		return -EINVAL;
	lifecycle = lease->lifecycle;
	mutex_lock(&lifecycle->lock);
	if (!mt6797_consys_runtime_lease_valid_locked(lease)) {
		ret = -ESHUTDOWN;
	} else if (lifecycle->runtime_pins) {
		ret = -EBUSY;
	} else {
		ret = mt6797_consys_identity_unwind(lifecycle, true, true, true,
							    true);
		lifecycle->state = ret ? MT6797_CONSYS_ERROR : MT6797_CONSYS_OFF;
		lifecycle->users = 0;
		lifecycle->runtime_lease = NULL;
		lease->lifecycle = NULL;
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_lease_release);

bool mt6797_consys_runtime_lease_active(
		const struct mt6797_consys_runtime_lease *lease)
{
	struct mt6797_consys_lifecycle *lifecycle;
	bool active;

	if (!lease || !lease->lifecycle)
		return false;
	lifecycle = lease->lifecycle;
	mutex_lock(&lifecycle->lock);
	active = mt6797_consys_runtime_lease_valid_locked(lease);
	mutex_unlock(&lifecycle->lock);

	return active;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_lease_active);

int mt6797_consys_runtime_lease_pin(
		const struct mt6797_consys_runtime_lease *lease)
{
	struct mt6797_consys_lifecycle *lifecycle;
	int ret = 0;

	if (!lease || !lease->lifecycle)
		return -EINVAL;
	lifecycle = lease->lifecycle;
	mutex_lock(&lifecycle->lock);
	if (!mt6797_consys_runtime_lease_valid_locked(lease))
		ret = -ESHUTDOWN;
	else if (lifecycle->runtime_pins == U32_MAX)
		ret = -EOVERFLOW;
	else
		lifecycle->runtime_pins++;
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_lease_pin);

void mt6797_consys_runtime_lease_unpin(
		const struct mt6797_consys_runtime_lease *lease)
{
	struct mt6797_consys_lifecycle *lifecycle;

	if (!lease || !lease->lifecycle)
		return;
	lifecycle = lease->lifecycle;
	mutex_lock(&lifecycle->lock);
	if (mt6797_consys_runtime_lease_valid_locked(lease) &&
	    lifecycle->runtime_pins)
		lifecycle->runtime_pins--;
	mutex_unlock(&lifecycle->lock);
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_lease_unpin);

enum mt6797_consys_state
mt6797_consys_lifecycle_state(struct mt6797_consys_lifecycle *lifecycle)
{
	enum mt6797_consys_state state;

	if (!lifecycle)
		return MT6797_CONSYS_ERROR;

	mutex_lock(&lifecycle->lock);
	state = lifecycle->state;
	mutex_unlock(&lifecycle->lock);

	return state;
}
EXPORT_SYMBOL_GPL(mt6797_consys_lifecycle_state);

u32 mt6797_consys_lifecycle_users(struct mt6797_consys_lifecycle *lifecycle)
{
	u32 users;

	if (!lifecycle)
		return 0;

	mutex_lock(&lifecycle->lock);
	users = lifecycle->users;
	mutex_unlock(&lifecycle->lock);

	return users;
}
EXPORT_SYMBOL_GPL(mt6797_consys_lifecycle_users);

bool mt6797_consys_shared_mem_valid(phys_addr_t start, resource_size_t size)
{
	if (size != MT6797_CONSYS_SHARED_MEM_SIZE ||
	    start & (MT6797_CONSYS_SHARED_MEM_SIZE - 1))
		return false;

	return start >= MT6797_CONSYS_SHARED_MEM_MIN &&
		start <= MT6797_CONSYS_SHARED_MEM_END - size;
}
EXPORT_SYMBOL_GPL(mt6797_consys_shared_mem_valid);

MODULE_DESCRIPTION("MT6797 CONSYS hardware-neutral identity transaction core");
MODULE_LICENSE("GPL");
