// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral containment for the MT6797 MD1 WDT key-write boundary.
 *
 * Vendor order and operands are in eccci/mt6797/cldma_platform.c:839-845 and
 * cldma_platform.h:20-37. The vendor reports WDT status only on its interrupt
 * path; it supplies no post-key MODE acknowledgement or inverse operation.
 */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-wdt-gate.h>
#include <linux/string.h>

int mt6797_md1_wdt_gate_init(struct mt6797_md1_wdt_gate *gate,
	const struct mt6797_md1_wdt_gate_ops *ops, void *context)
{
	if (!gate || !ops || ops->abi != MT6797_MD1_WDT_GATE_ABI ||
	    !ops->observe_status || !ops->write_md_mode_key ||
	    !ops->write_l1_mode_key)
		return -EINVAL;

	memset(gate, 0, sizeof(*gate));
	gate->ops = ops;
	gate->context = context;
	mutex_init(&gate->lock);
	gate->state = MT6797_MD1_WDT_GATE_OFF;
	return 0;
}

static int mt6797_md1_wdt_gate_fatal(struct mt6797_md1_wdt_gate *gate,
	int error)
{
	gate->last_error = error;
	gate->state = MT6797_MD1_WDT_GATE_FATAL;
	return error;
}

int mt6797_md1_wdt_gate_run(struct mt6797_md1_wdt_gate *gate,
	const struct mt6797_md1_wdt_gate_preflight *preflight)
{
	int ret;

	if (!gate || !gate->ops || !preflight)
		return -EINVAL;

	mutex_lock(&gate->lock);
	if (gate->state == MT6797_MD1_WDT_GATE_FATAL) {
		ret = -EIO;
		goto out_unlock;
	}
	if (gate->state != MT6797_MD1_WDT_GATE_OFF) {
		ret = -EALREADY;
		goto out_unlock;
	}
	if (!preflight->cold_owner_pll_ready_wdt_unowned) {
		ret = -EPERM;
		goto out_unlock;
	}

	/* This is diagnostic only; observation failure has made no key write. */
	ret = gate->ops->observe_status(gate->context, &gate->observation);
	if (ret)
		goto out_unlock;
	gate->state = MT6797_MD1_WDT_GATE_PREFLIGHT_ACCEPTED;

	/* Never read MODE after a key: vendor source gives no acknowledgement. */
	ret = gate->ops->write_md_mode_key(gate->context);
	if (ret)
		goto out_fatal;
	gate->state = MT6797_MD1_WDT_GATE_MD_KEY_ATTEMPTED;
	ret = gate->ops->write_l1_mode_key(gate->context);
	if (ret)
		goto out_fatal;

	/* The documented status words are evidence only, never key acceptance. */
	ret = gate->ops->observe_status(gate->context, &gate->post_observation);
	if (ret)
		goto out_fatal;

	gate->state = MT6797_MD1_WDT_GATE_KEYS_ATTEMPTED_WRITE_ONLY;
	gate->last_error = 0;
	ret = 0;
	goto out_unlock;

out_fatal:
	ret = mt6797_md1_wdt_gate_fatal(gate, ret);
out_unlock:
	mutex_unlock(&gate->lock);
	return ret;
}

enum mt6797_md1_wdt_gate_state
mt6797_md1_wdt_gate_state(struct mt6797_md1_wdt_gate *gate)
{
	enum mt6797_md1_wdt_gate_state state;

	if (!gate)
		return MT6797_MD1_WDT_GATE_FATAL;
	mutex_lock(&gate->lock);
	state = gate->state;
	mutex_unlock(&gate->lock);
	return state;
}
