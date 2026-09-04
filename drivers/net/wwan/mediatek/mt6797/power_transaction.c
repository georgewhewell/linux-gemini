// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-independent MT6797 MD1 power transaction and rollback. */

#include <linux/bitops.h>
#include <linux/errno.h>

#include "power_transaction.h"

enum mt6797_md1_power_step {
	MT6797_MD1_STEP_ENABLE_VSRAM_MD,
	MT6797_MD1_STEP_ENABLE_HW_VSRAM_MD,
	MT6797_MD1_STEP_ENABLE_VMD1,
	MT6797_MD1_STEP_ENABLE_HW_VMD1,
	MT6797_MD1_STEP_ENABLE_VMODEM,
	MT6797_MD1_STEP_ENABLE_HW_VMODEM,
	MT6797_MD1_STEP_VOLTAGE_VSRAM_MD,
	MT6797_MD1_STEP_VOLTAGE_VMODEM,
	MT6797_MD1_STEP_VOLTAGE_HW_VSRAM_MD,
	MT6797_MD1_STEP_VOLTAGE_HW_VMD1,
	MT6797_MD1_STEP_VOLTAGE_HW_VMODEM,
	MT6797_MD1_STEP_EXT_BUCK_ISO,
	MT6797_MD1_STEP_PWR_ON,
	MT6797_MD1_STEP_PWR_ON_2ND,
	MT6797_MD1_STEP_PWR_CLK_DIS,
	MT6797_MD1_STEP_PWR_ISO,
	MT6797_MD1_STEP_SRAM_PDN,
	MT6797_MD1_STEP_BUS_PROTECT,
	MT6797_MD1_STEP_RESET_ASSERT,
	MT6797_MD1_POWER_STEP_COUNT,
};

struct mt6797_md1_inherited_power {
	int voltage_uv[MT6797_MD1_POWER_RAIL_COUNT];
	bool hw_control[MT6797_MD1_POWER_RAIL_COUNT]
		       [MT6797_MD1_RAIL_HW_CONTROL_COUNT];
};

static bool
mt6797_md1_control_forward_value(enum mt6797_md1_power_control control)
{
	switch (control) {
	case MT6797_MD1_CTRL_PWR_ON:
	case MT6797_MD1_CTRL_PWR_ON_2ND:
		return true;
	case MT6797_MD1_CTRL_EXT_BUCK_ISO:
	case MT6797_MD1_CTRL_PWR_CLK_DIS:
	case MT6797_MD1_CTRL_PWR_ISO:
	case MT6797_MD1_CTRL_SRAM_PDN:
	case MT6797_MD1_CTRL_BUS_PROTECT:
	case MT6797_MD1_CTRL_RESET_ASSERT:
	case MT6797_MD1_POWER_CONTROL_COUNT:
		return false;
	}

	return false;
}

static int mt6797_md1_apply_step(const struct mt6797_md1_power_ops *ops,
				 void *context,
				 const struct mt6797_md1_power_req *req,
				 const struct mt6797_md1_inherited_power *state,
				 enum mt6797_md1_power_step step,
				 bool forward)
{
	enum mt6797_md1_power_control control;
	enum mt6797_md1_power_rail rail;
	bool value;
	int voltage_uv;

	switch (step) {
	case MT6797_MD1_STEP_ENABLE_VSRAM_MD:
		rail = MT6797_MD1_RAIL_VSRAM_MD;
		goto set_rail;
	case MT6797_MD1_STEP_ENABLE_VMD1:
		rail = MT6797_MD1_RAIL_VMD1;
		goto set_rail;
	case MT6797_MD1_STEP_ENABLE_VMODEM:
		rail = MT6797_MD1_RAIL_VMODEM;
set_rail:
		return ops->set_rail(context, rail, forward);
	case MT6797_MD1_STEP_ENABLE_HW_VSRAM_MD:
		rail = MT6797_MD1_RAIL_VSRAM_MD;
		goto set_enable_hw;
	case MT6797_MD1_STEP_ENABLE_HW_VMD1:
		rail = MT6797_MD1_RAIL_VMD1;
		goto set_enable_hw;
	case MT6797_MD1_STEP_ENABLE_HW_VMODEM:
		rail = MT6797_MD1_RAIL_VMODEM;
set_enable_hw:
		value = forward ? true :
			state->hw_control[rail][MT6797_MD1_RAIL_ENABLE_HW_CONTROL];
		return ops->set_rail_hw_control(context, rail,
				MT6797_MD1_RAIL_ENABLE_HW_CONTROL, value);
	case MT6797_MD1_STEP_VOLTAGE_VSRAM_MD:
		rail = MT6797_MD1_RAIL_VSRAM_MD;
		goto set_voltage;
	case MT6797_MD1_STEP_VOLTAGE_VMODEM:
		rail = MT6797_MD1_RAIL_VMODEM;
set_voltage:
		if (forward)
			voltage_uv = rail == MT6797_MD1_RAIL_VSRAM_MD ?
				req->vsram_md_uv : req->vmodem_uv;
		else
			voltage_uv = state->voltage_uv[rail];
		return ops->set_rail_voltage(context, rail, voltage_uv);
	case MT6797_MD1_STEP_VOLTAGE_HW_VSRAM_MD:
		rail = MT6797_MD1_RAIL_VSRAM_MD;
		goto set_voltage_hw;
	case MT6797_MD1_STEP_VOLTAGE_HW_VMD1:
		rail = MT6797_MD1_RAIL_VMD1;
		goto set_voltage_hw;
	case MT6797_MD1_STEP_VOLTAGE_HW_VMODEM:
		rail = MT6797_MD1_RAIL_VMODEM;
set_voltage_hw:
		value = forward ? true :
			state->hw_control[rail][MT6797_MD1_RAIL_VOLTAGE_HW_CONTROL];
		return ops->set_rail_hw_control(context, rail,
				MT6797_MD1_RAIL_VOLTAGE_HW_CONTROL, value);
	case MT6797_MD1_STEP_EXT_BUCK_ISO ... MT6797_MD1_STEP_RESET_ASSERT:
		control = step - MT6797_MD1_STEP_EXT_BUCK_ISO;
		value = mt6797_md1_control_forward_value(control);
		return ops->set_control(context, control,
					forward ? value : !value);
	case MT6797_MD1_POWER_STEP_COUNT:
		return -EINVAL;
	}

	return -EINVAL;
}

static int mt6797_md1_poll_power(const struct mt6797_md1_power_ops *ops,
				 void *context, unsigned int attempts,
				 bool target)
{
	bool primary;
	bool secondary;
	unsigned int i;
	int ret;

	for (i = 0; i < attempts; i++) {
		ret = ops->read_power_ack(context, &primary, &secondary);
		if (ret)
			return ret;
		if (primary == target && secondary == target)
			return 0;
		if (i + 1 < attempts)
			ops->ack_delay(context);
	}

	return -ETIMEDOUT;
}

static int mt6797_md1_poll_bus(const struct mt6797_md1_power_ops *ops,
			       void *context, unsigned int attempts,
			       bool target)
{
	bool protected;
	unsigned int i;
	int ret;

	for (i = 0; i < attempts; i++) {
		ret = ops->read_bus_protected(context, &protected);
		if (ret)
			return ret;
		if (protected == target)
			return 0;
		if (i + 1 < attempts)
			ops->ack_delay(context);
	}

	return -ETIMEDOUT;
}

static int
mt6797_md1_verify_domain_off(const struct mt6797_md1_power_ops *ops,
			     void *context)
{
	bool enabled;
	bool expected;
	bool asserted;
	bool primary;
	bool protected;
	bool secondary;
	unsigned int rail;
	unsigned int control;
	int ret;

	ret = ops->read_power_ack(context, &primary, &secondary);
	if (ret)
		return ret;
	if (primary || secondary)
		return -EBUSY;

	ret = ops->read_bus_protected(context, &protected);
	if (ret)
		return ret;
	if (!protected)
		return -EBUSY;

	for (rail = 0; rail < MT6797_MD1_POWER_RAIL_COUNT; rail++) {
		ret = ops->read_rail(context, rail, &enabled);
		if (ret)
			return ret;
		if (enabled)
			return -EBUSY;
	}
	for (control = 0; control < MT6797_MD1_POWER_CONTROL_COUNT;
	     control++) {
		ret = ops->read_control(context, control, &asserted);
		if (ret)
			return ret;
		expected = !mt6797_md1_control_forward_value(control);
		if (asserted != expected)
			return -EBUSY;
	}

	return 0;
}

static int
mt6797_md1_snapshot_inherited(const struct mt6797_md1_power_ops *ops,
			      void *context,
			      struct mt6797_md1_inherited_power *state)
{
	unsigned int control;
	unsigned int rail;
	int ret;

	ret = mt6797_md1_verify_domain_off(ops, context);
	if (ret)
		return ret;

	for (rail = 0; rail < MT6797_MD1_POWER_RAIL_COUNT; rail++) {
		for (control = 0;
		     control < MT6797_MD1_RAIL_HW_CONTROL_COUNT; control++) {
			ret = ops->read_rail_hw_control(context, rail, control,
				&state->hw_control[rail][control]);
			if (ret)
				return ret;
		}
	}

	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VSRAM_MD,
				     &state->voltage_uv[MT6797_MD1_RAIL_VSRAM_MD]);
	if (ret)
		return ret;
	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VMODEM,
				     &state->voltage_uv[MT6797_MD1_RAIL_VMODEM]);
	if (ret)
		return ret;
	if (state->voltage_uv[MT6797_MD1_RAIL_VSRAM_MD] <= 0 ||
	    state->voltage_uv[MT6797_MD1_RAIL_VMODEM] <= 0)
		return -EINVAL;

	return 0;
}

static int
mt6797_md1_verify_rail_contract(const struct mt6797_md1_power_ops *ops,
				void *context,
				const struct mt6797_md1_power_req *req)
{
	bool enabled;
	unsigned int control;
	unsigned int rail;
	int voltage_uv;
	int ret;

	for (rail = 0; rail < MT6797_MD1_POWER_RAIL_COUNT; rail++) {
		ret = ops->read_rail(context, rail, &enabled);
		if (ret)
			return ret;
		if (!enabled)
			return -EBUSY;
		for (control = 0;
		     control < MT6797_MD1_RAIL_HW_CONTROL_COUNT; control++) {
			ret = ops->read_rail_hw_control(context, rail, control,
							&enabled);
			if (ret)
				return ret;
			if (!enabled)
				return -EBUSY;
		}
	}

	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VSRAM_MD,
				     &voltage_uv);
	if (ret)
		return ret;
	if (voltage_uv != req->vsram_md_uv)
		return -ERANGE;
	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VMODEM,
				     &voltage_uv);
	if (ret)
		return ret;
	if (voltage_uv != req->vmodem_uv)
		return -ERANGE;

	return 0;
}

static int
mt6797_md1_verify_restored(const struct mt6797_md1_power_ops *ops,
			   void *context,
			   const struct mt6797_md1_inherited_power *state)
{
	bool enabled;
	unsigned int control;
	unsigned int rail;
	int voltage_uv;
	int ret;

	ret = mt6797_md1_verify_domain_off(ops, context);
	if (ret)
		return ret;

	for (rail = 0; rail < MT6797_MD1_POWER_RAIL_COUNT; rail++) {
		for (control = 0;
		     control < MT6797_MD1_RAIL_HW_CONTROL_COUNT; control++) {
			ret = ops->read_rail_hw_control(context, rail, control,
							&enabled);
			if (ret)
				return ret;
			if (enabled != state->hw_control[rail][control])
				return -EBUSY;
		}
	}

	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VSRAM_MD,
				     &voltage_uv);
	if (ret)
		return ret;
	if (voltage_uv != state->voltage_uv[MT6797_MD1_RAIL_VSRAM_MD])
		return -ERANGE;
	ret = ops->read_rail_voltage(context, MT6797_MD1_RAIL_VMODEM,
				     &voltage_uv);
	if (ret)
		return ret;
	if (voltage_uv != state->voltage_uv[MT6797_MD1_RAIL_VMODEM])
		return -ERANGE;

	return 0;
}

static int mt6797_md1_unwind(const struct mt6797_md1_power_ops *ops,
			     void *context,
			     const struct mt6797_md1_power_req *req,
			     const struct mt6797_md1_inherited_power *state,
			     unsigned long attempted,
			     unsigned int ack_attempts)
{
	int step;
	int first_ret = 0;
	int ret;

	for (step = MT6797_MD1_POWER_STEP_COUNT - 1; step >= 0; step--) {
		if (!(attempted & BIT(step)))
			continue;

		ret = mt6797_md1_apply_step(ops, context, req, state,
					    step, false);
		if (ret && !first_ret)
			first_ret = ret;

		if (step == MT6797_MD1_STEP_BUS_PROTECT) {
			ret = mt6797_md1_poll_bus(ops, context, ack_attempts, true);
			if (ret && !first_ret)
				first_ret = ret;
		} else if (step == MT6797_MD1_STEP_PWR_ON) {
			ret = mt6797_md1_poll_power(ops, context, ack_attempts, false);
			if (ret && !first_ret)
				first_ret = ret;
		}
	}

	ret = mt6797_md1_verify_restored(ops, context, state);
	return ret ?: first_ret;
}

int mt6797_md1_power_transaction(const struct mt6797_md1_power_req *req,
				 const struct mt6797_md1_power_ops *ops,
				 void *context, unsigned int ack_attempts)
{
	struct mt6797_md1_inherited_power state = {};
	unsigned long attempted = 0;
	unsigned int step;
	int unwind_ret;
	int ret;

	if (!req || req->vsram_md_uv <= 0 || req->vmodem_uv <= 0 || !ops ||
	    !context || !ack_attempts ||
	    !ops->set_rail ||
	    !ops->set_rail_voltage || !ops->set_rail_hw_control ||
	    !ops->set_control || !ops->read_rail || !ops->read_rail_voltage ||
	    !ops->read_rail_hw_control || !ops->read_control ||
	    !ops->read_power_ack || !ops->read_bus_protected ||
	    !ops->settle_rails || !ops->ack_delay)
		return -EINVAL;

	ret = mt6797_md1_snapshot_inherited(ops, context, &state);
	if (ret)
		return ret;

	for (step = 0; step < MT6797_MD1_POWER_STEP_COUNT; step++) {
		/* A failing callback may have changed hardware: unwind it too. */
		attempted |= BIT(step);
		ret = mt6797_md1_apply_step(ops, context, req, &state,
					    step, true);
		if (ret)
			break;

		if (step == MT6797_MD1_STEP_VOLTAGE_VMODEM) {
			ret = ops->settle_rails(context,
						MT6797_MD1_RAIL_SETTLE_US);
			if (ret)
				break;
		} else if (step == MT6797_MD1_STEP_VOLTAGE_HW_VMODEM) {
			ret = mt6797_md1_verify_rail_contract(ops, context, req);
			if (ret)
				break;
		} else if (step == MT6797_MD1_STEP_PWR_ON_2ND) {
			ret = mt6797_md1_poll_power(ops, context, ack_attempts, true);
			if (ret)
				break;
		} else if (step == MT6797_MD1_STEP_BUS_PROTECT) {
			ret = mt6797_md1_poll_bus(ops, context, ack_attempts, false);
			if (ret)
				break;
		}
	}

	unwind_ret = mt6797_md1_unwind(ops, context, req, &state, attempted,
				       ack_attempts);
	return unwind_ret ?: ret;
}
