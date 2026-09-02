/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small, transport-free cleanup core for the optional MT6797 WLAN W0 probe.
 * It deliberately has no regulator or MMIO knowledge so its failure paths can
 * run unchanged in a host test and in KUnit.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_W0_PHASE_H
#define __LINUX_SOC_MEDIATEK_MT6797_WIFI_AHB_W0_PHASE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <errno.h>
#endif

struct mt6797_wifi_ahb_w0_ops {
	int (*get_voltage)(void *context);
	int (*is_enabled)(void *context, bool wifi);
	int (*set_voltage)(void *context, int uv);
	int (*enable)(void *context);
	int (*disable)(void *context);
	int (*observe)(void *context);
	void (*fatal)(void *context, int causal, int cleanup);
	void *context;
};

struct mt6797_wifi_ahb_w0_result {
	int causal;
	int cleanup;
	bool selector_dirty;
	bool fatal;
};

static inline void mt6797_wifi_ahb_w0_first_error(int *slot, int error)
{
	if (!*slot && error)
		*slot = error;
}

/* Return zero only when both independently reported rails are definitely off. */
static inline int mt6797_wifi_ahb_w0_verify_off(
		const struct mt6797_wifi_ahb_w0_ops *ops)
{
	int bt_on = ops->is_enabled(ops->context, false);
	int wifi_on = ops->is_enabled(ops->context, true);

	if (bt_on < 0)
		return bt_on;
	if (wifi_on < 0)
		return wifi_on;
	return (bt_on || wifi_on) ? -EBUSY : 0;
}

/*
 * Every path after the selector write reaches verification.  The first action
 * failure remains causal; a later cleanup failure is retained separately and
 * makes @result fatal, meaning the owner must fence/reset rather than claim a
 * clean radio-off state.
 */
static inline int mt6797_wifi_ahb_w0_run(
		const struct mt6797_wifi_ahb_w0_ops *ops,
		struct mt6797_wifi_ahb_w0_result *result, int target_uv)
{
	int original_uv, ret, enabled, off;
	bool enable_attempted = false;

	if (!ops || !result || !ops->get_voltage || !ops->is_enabled ||
	    !ops->set_voltage || !ops->enable || !ops->disable || !ops->observe)
		return -EINVAL;
	*result = (struct mt6797_wifi_ahb_w0_result) { };
	original_uv = ops->get_voltage(ops->context);
	if (original_uv <= 0)
		return original_uv ?: -EINVAL;
	off = mt6797_wifi_ahb_w0_verify_off(ops);
	if (off)
		return off;

	/* A regulator can alter the selector before returning an error. */
	result->selector_dirty = true;
	ret = ops->set_voltage(ops->context, target_uv);
	mt6797_wifi_ahb_w0_first_error(&result->causal, ret);
	if (!ret) {
		enable_attempted = true;
		ret = ops->enable(ops->context);
		mt6797_wifi_ahb_w0_first_error(&result->causal, ret);
		if (!ret) {
			ret = ops->observe(ops->context);
			mt6797_wifi_ahb_w0_first_error(&result->causal, ret);
		}
	}

	/* A failed enable is still followed by a status check and disable if on. */
	if (enable_attempted) {
		enabled = ops->is_enabled(ops->context, true);
		if (enabled > 0)
			mt6797_wifi_ahb_w0_first_error(&result->cleanup,
						       ops->disable(ops->context));
		else if (enabled < 0)
			mt6797_wifi_ahb_w0_first_error(&result->cleanup, enabled);
	}
	off = mt6797_wifi_ahb_w0_verify_off(ops);
	mt6797_wifi_ahb_w0_first_error(&result->cleanup, off);
	if (!off) {
		ret = ops->set_voltage(ops->context, original_uv);
		mt6797_wifi_ahb_w0_first_error(&result->cleanup, ret);
		if (!ret)
			result->selector_dirty = false;
	}
	if (result->cleanup || result->selector_dirty) {
		result->fatal = true;
		if (ops->fatal)
			ops->fatal(ops->context, result->causal, result->cleanup);
	}
	return result->causal ?: result->cleanup;
}

#endif
