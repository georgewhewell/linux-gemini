// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 CONSYS lifecycle core.
 *
 * This intentionally has no platform backend.  In particular, the disabled
 * inventory driver's resources are not mapped here.  A later backend must
 * provide reviewed regulator, clock, reset, and power-domain operations.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>

#include <linux/soc/mediatek/mt6797-consys.h>

static void mt6797_consys_delay(struct mt6797_consys_lifecycle *lifecycle)
{
	u32 interval = lifecycle->timeouts.poll_interval_us;
	u32 slack = min(interval / 4, 50U);

	if (interval > U32_MAX - slack)
		slack = U32_MAX - interval;
	if (lifecycle->ops->delay_us)
		lifecycle->ops->delay_us(lifecycle->context, interval,
					 interval + slack);
	else
		usleep_range(interval, interval + slack);
}

static int mt6797_consys_wait(
	struct mt6797_consys_lifecycle *lifecycle,
	enum mt6797_consys_poll poll, u32 timeout_us)
{
	u32 attempt;
	u32 interval = lifecycle->timeouts.poll_interval_us;
	u32 attempts = timeout_us / interval;
	int ret;

	if (timeout_us % interval)
		attempts++;

	for (attempt = 0; attempt < attempts; attempt++) {
		ret = lifecycle->ops->poll(lifecycle->context, poll);
		if (!ret)
			return 0;
		if (ret != -EAGAIN)
			return ret;
		if (attempt + 1 < attempts)
			mt6797_consys_delay(lifecycle);
	}

	return -ETIMEDOUT;
}

/* Execute every applicable inverse operation, preserving the first failure. */
static int mt6797_consys_power_down(struct mt6797_consys_lifecycle *lifecycle,
				    bool reset, bool power, bool clock, bool supplies)
{
	int ret;
	int first_error = 0;

	if (reset) {
		ret = lifecycle->ops->assert_reset(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	if (power) {
		ret = lifecycle->ops->power_off(lifecycle->context);
		if (ret) {
			if (!first_error)
				first_error = ret;
		} else {
			ret = mt6797_consys_wait(lifecycle,
						 MT6797_CONSYS_POLL_POWER_OFF,
						 lifecycle->timeouts.power_off_timeout_us);
			if (ret && !first_error)
				first_error = ret;
		}
	}

	if (clock) {
		ret = lifecycle->ops->disable_clock(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	if (supplies) {
		ret = lifecycle->ops->disable_supplies(lifecycle->context);
		if (ret && !first_error)
			first_error = ret;
	}

	return first_error;
}

static int mt6797_consys_power_up(struct mt6797_consys_lifecycle *lifecycle)
{
	bool supplies = false;
	bool clock = false;
	bool power = false;
	bool reset = false;
	int ret;

	supplies = true;
	ret = lifecycle->ops->enable_supplies(lifecycle->context);
	if (ret)
		goto unwind;

	clock = true;
	ret = lifecycle->ops->enable_clock(lifecycle->context);
	if (ret)
		goto unwind;

	power = true;
	ret = lifecycle->ops->power_on(lifecycle->context);
	if (ret)
		goto unwind;

	ret = mt6797_consys_wait(lifecycle,
				 MT6797_CONSYS_POLL_POWER_ON,
				 lifecycle->timeouts.power_on_timeout_us);
	if (ret)
		goto unwind;

	reset = true;
	ret = lifecycle->ops->release_reset(lifecycle->context);
	if (ret)
		goto unwind;

	ret = mt6797_consys_wait(lifecycle,
				 MT6797_CONSYS_POLL_MCU_READY,
				 lifecycle->timeouts.mcu_ready_timeout_us);
	if (ret)
		goto unwind;

	return 0;

unwind:
	if (mt6797_consys_power_down(lifecycle, reset, power, clock, supplies))
		lifecycle->state = MT6797_CONSYS_ERROR;
	return ret;
}

static bool mt6797_consys_ops_valid(const struct mt6797_consys_ops *ops)
{
	return ops && ops->abi == MT6797_CONSYS_LIFECYCLE_ABI &&
		ops->enable_supplies && ops->disable_supplies &&
		ops->enable_clock && ops->disable_clock && ops->power_on &&
		ops->power_off && ops->release_reset && ops->assert_reset &&
		ops->poll;
}

static bool
mt6797_consys_timeouts_valid(const struct mt6797_consys_timeouts *timeouts)
{
	return timeouts && timeouts->poll_interval_us &&
		timeouts->power_on_timeout_us >= timeouts->poll_interval_us &&
		timeouts->mcu_ready_timeout_us >= timeouts->poll_interval_us &&
		timeouts->power_off_timeout_us >= timeouts->poll_interval_us;
}

int mt6797_consys_lifecycle_init(
	struct mt6797_consys_lifecycle *lifecycle,
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
	mutex_init(&lifecycle->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_lifecycle_init);

int mt6797_consys_get(struct mt6797_consys_lifecycle *lifecycle)
{
	int ret = 0;

	if (!lifecycle)
		return -EINVAL;

	mutex_lock(&lifecycle->lock);
	if (lifecycle->state == MT6797_CONSYS_ERROR) {
		ret = -EIO;
	} else if (lifecycle->state == MT6797_CONSYS_ON) {
		if (lifecycle->users == U32_MAX)
			ret = -EOVERFLOW;
		else
			lifecycle->users++;
	} else {
		ret = mt6797_consys_power_up(lifecycle);
		if (!ret) {
			lifecycle->state = MT6797_CONSYS_ON;
			lifecycle->users = 1;
		} else if (lifecycle->state != MT6797_CONSYS_ERROR) {
			lifecycle->state = MT6797_CONSYS_OFF;
		}
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_get);

int mt6797_consys_put(struct mt6797_consys_lifecycle *lifecycle)
{
	int ret = 0;

	if (!lifecycle)
		return -EINVAL;

	mutex_lock(&lifecycle->lock);
	if (lifecycle->state == MT6797_CONSYS_ERROR) {
		ret = -EIO;
	} else if (lifecycle->state != MT6797_CONSYS_ON || !lifecycle->users) {
		ret = -EINVAL;
	} else if (--lifecycle->users) {
		/* The final voter owns the physical shutdown. */
	} else {
		ret = mt6797_consys_power_down(lifecycle, true, true, true, true);
		lifecycle->state = ret ? MT6797_CONSYS_ERROR : MT6797_CONSYS_OFF;
	}
	mutex_unlock(&lifecycle->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_put);

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

MODULE_DESCRIPTION("MT6797 CONSYS hardware-neutral lifecycle core");
MODULE_LICENSE("GPL");
