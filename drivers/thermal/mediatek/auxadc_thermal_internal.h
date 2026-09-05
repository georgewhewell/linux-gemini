/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_AUXADC_THERMAL_INTERNAL_H
#define __MTK_AUXADC_THERMAL_INTERNAL_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>

struct mtk_thermal_transaction_state {
	bool auxadc_clock_enabled;
	bool thermal_clock_enabled;
	bool reset_deasserted;
	bool apmixed_configured;
	bool banks_touched;
	bool channel_touched;
	bool ready;
};

struct mtk_thermal_transaction_ops {
	int (*enable_auxadc_clock)(void *context);
	void (*disable_auxadc_clock)(void *context);
	int (*enable_thermal_clock)(void *context);
	void (*disable_thermal_clock)(void *context);
	int (*reset_thermal)(void *context);
	void (*assert_thermal_reset)(void *context);
	int (*configure_apmixed)(void *context);
	void (*restore_apmixed)(void *context);
	int (*wait_for_idle)(void *context);
	void (*pause_disable_banks)(void *context);
	int (*clear_auxadc_channel)(void *context);
	void (*disable_auxadc_channel)(void *context);
	int (*prepare_bank)(void *context, unsigned int bank);
	int (*commit_auxadc_channel)(void *context);
	int (*enable_bank)(void *context, unsigned int bank);
	int (*release_bank)(void *context, unsigned int bank);
	int (*first_sample)(void *context, unsigned int bank);
};

static inline bool
mtk_thermal_transaction_state_is_closed(const struct mtk_thermal_transaction_state *state)
{
	return state && !state->auxadc_clock_enabled &&
		!state->thermal_clock_enabled && !state->reset_deasserted &&
		!state->apmixed_configured && !state->banks_touched &&
		!state->channel_touched && !state->ready;
}

static inline u32 mtk_thermal_mt6797_apmixed_value(u32 value)
{
	return value & ~GENMASK(5, 4);
}

static inline bool mtk_thermal_mt6797_ahb_idle(u32 value)
{
	return !(value >> 16);
}

static inline bool mtk_thermal_mt6797_auxadc_idle(u32 value)
{
	return !(value & BIT(0));
}

static inline bool
mtk_thermal_mt6797_first_sample_valid(u32 raw, int temperature)
{
	return (raw & GENMASK(11, 0)) && temperature >= -20000 &&
		temperature <= 150000;
}

static inline void
mtk_thermal_transaction_close(void *context,
			      const struct mtk_thermal_transaction_ops *ops,
			      struct mtk_thermal_transaction_state *state)
{
	if (!context || !ops || !state)
		return;

	if (state->banks_touched)
		ops->pause_disable_banks(context);
	if (state->channel_touched)
		ops->disable_auxadc_channel(context);
	if (state->apmixed_configured)
		ops->restore_apmixed(context);
	if (state->reset_deasserted)
		ops->assert_thermal_reset(context);
	if (state->thermal_clock_enabled)
		ops->disable_thermal_clock(context);
	if (state->auxadc_clock_enabled)
		ops->disable_auxadc_clock(context);

	state->auxadc_clock_enabled = false;
	state->thermal_clock_enabled = false;
	state->reset_deasserted = false;
	state->apmixed_configured = false;
	state->banks_touched = false;
	state->channel_touched = false;
	state->ready = false;
}

static inline bool
mtk_thermal_transaction_ops_valid(const struct mtk_thermal_transaction_ops *ops)
{
	return ops && ops->enable_auxadc_clock &&
		ops->disable_auxadc_clock && ops->enable_thermal_clock &&
		ops->disable_thermal_clock && ops->reset_thermal &&
		ops->assert_thermal_reset && ops->configure_apmixed &&
		ops->restore_apmixed && ops->wait_for_idle &&
		ops->pause_disable_banks && ops->clear_auxadc_channel &&
		ops->disable_auxadc_channel && ops->prepare_bank &&
		ops->commit_auxadc_channel && ops->enable_bank &&
		ops->release_bank && ops->first_sample;
}

static inline int
mtk_thermal_transaction_execute(void *context,
				const struct mtk_thermal_transaction_ops *ops,
				struct mtk_thermal_transaction_state *state,
				unsigned int banks)
{
	unsigned int bank;
	int ret;

	if (!context || !mtk_thermal_transaction_ops_valid(ops) ||
	    !mtk_thermal_transaction_state_is_closed(state) || !banks)
		return -EINVAL;

	ret = ops->enable_auxadc_clock(context);
	if (ret)
		goto fail;
	state->auxadc_clock_enabled = true;

	ret = ops->enable_thermal_clock(context);
	if (ret)
		goto fail;
	state->thermal_clock_enabled = true;

	ret = ops->reset_thermal(context);
	if (ret)
		goto fail;
	state->reset_deasserted = true;

	state->apmixed_configured = true;
	ret = ops->configure_apmixed(context);
	if (ret)
		goto fail;

	ret = ops->wait_for_idle(context);
	if (ret)
		goto fail;

	state->banks_touched = true;
	ops->pause_disable_banks(context);
	state->channel_touched = true;
	ret = ops->clear_auxadc_channel(context);
	if (ret)
		goto fail;

	for (bank = 0; bank < banks; bank++) {
		ret = ops->prepare_bank(context, bank);
		if (ret)
			goto fail;
	}

	ret = ops->commit_auxadc_channel(context);
	if (ret)
		goto fail;

	for (bank = 0; bank < banks; bank++) {
		ret = ops->enable_bank(context, bank);
		if (ret)
			goto fail;
	}

	for (bank = 0; bank < banks; bank++) {
		ret = ops->release_bank(context, bank);
		if (ret)
			goto fail;
	}

	for (bank = 0; bank < banks; bank++) {
		ret = ops->first_sample(context, bank);
		if (ret)
			goto fail;
	}

	state->ready = true;

	return 0;

fail:
	mtk_thermal_transaction_close(context, ops, state);
	return ret;
}

#endif /* __MTK_AUXADC_THERMAL_INTERNAL_H */
