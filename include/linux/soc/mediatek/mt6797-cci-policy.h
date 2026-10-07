/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MT6797_CCI_POLICY_H
#define _LINUX_MT6797_CCI_POLICY_H

#include <linux/errno.h>
#include <linux/types.h>

struct mt6797_cci_opp {
	u32 hz;
	u32 uv;
};

struct mt6797_cci_cpu {
	u32 hz;
	u32 uv;
	bool available;
};

/* CPU_LEVEL_1/SB vendor CCI table, ascending frequency and voltage. */
static const struct mt6797_cci_opp mt6797_cci_opps[] = {
	{ 169000000, 770000 }, { 234000000, 800000 },
	{ 325000000, 850000 }, { 416000000, 900000 },
	{ 481000000, 940000 }, { 533000000, 970000 },
	{ 611000000, 1000000 }, { 676000000, 1040000 },
	{ 754000000, 1070000 }, { 819000000, 1100000 },
	{ 845000000, 1130000 }, { 884000000, 1140000 },
	{ 910000000, 1150000 }, { 936000000, 1170000 },
	{ 975000000, 1190000 }, { 988000000, 1200000 },
};
#define MT6797_CCI_OPP_COUNT 16

/*
 * LL/L share the CCI rail; A72 contributes frequency, not its separate
 * rail voltage. The vendor policy saturates half the fastest available
 * CPU rate at the highest CCI OPP, then selects the fastest CCI OPP that
 * fits max(CCI minimum voltage, LL voltage, L voltage).
 */
static inline int mt6797_cci_select(const struct mt6797_cci_cpu cpus[3])
{
	u32 fastest = 0, budget = 0, half;
	unsigned int i, selected = 0;

	if (!cpus)
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		if (!cpus[i].available)
			continue;
		if (!cpus[i].hz || (i < 2 &&
		    (cpus[i].uv < 770000 || cpus[i].uv > 1200000)))
			return -EINVAL;
		if (cpus[i].hz > fastest)
			fastest = cpus[i].hz;
		if (i < 2 && cpus[i].uv > budget)
			budget = cpus[i].uv;
	}
	if (!fastest)
		return -ENODATA;
	half = fastest / 2 + fastest % 2;
	while (selected + 1 < MT6797_CCI_OPP_COUNT &&
	       mt6797_cci_opps[selected].hz < half)
		selected++;
	if (mt6797_cci_opps[selected].uv > budget)
		budget = mt6797_cci_opps[selected].uv;
	while (selected + 1 < MT6797_CCI_OPP_COUNT &&
	       mt6797_cci_opps[selected + 1].uv <= budget)
		selected++;
	return selected;
}

struct mt6797_cci_state {
	u32 hz;
	u32 vote_uv;
	bool ready;
	bool faulted;
};

/* Callbacks finish settling/readback; caller serializes the whole sequence. */
struct mt6797_cci_io {
	int (*get_hz)(void *context, u32 *hz);
	int (*get_uv)(void *context, u32 *uv);
	int (*set_hz)(void *context, u32 hz);
	int (*vote_uv)(void *context, u32 minimum_uv);
	void (*fence)(void *context);
};

static inline int mt6797_cci_fault(struct mt6797_cci_state *state,
				 const struct mt6797_cci_io *io, void *context)
{
	state->faulted = true;
	io->fence(context);
	return -EUCLEAN;
}

static inline bool mt6797_cci_io_valid(const struct mt6797_cci_io *io)
{
	return io && io->get_hz && io->get_uv && io->set_hz &&
		io->vote_uv && io->fence;
}

/*
 * Adopt firmware without guessing an OPP for an unlisted boot frequency.
 * Hold the measured voltage, lower CCI to the fastest supported rate no
 * higher than that frequency and supported by that voltage, then release
 * the excess vote. In particular, boot 629999328 Hz at 1 V becomes 611 MHz.
 */
static inline int mt6797_cci_adopt(struct mt6797_cci_state *state,
				 const struct mt6797_cci_io *io, void *context)
{
	u32 hz, uv, actual;
	int ret, selected = -1;
	unsigned int i;

	if (!state || !mt6797_cci_io_valid(io))
		return -EINVAL;
	if (state->faulted)
		return -EHOSTDOWN;
	if (state->ready)
		return -EALREADY;
	ret = io->get_hz(context, &hz);
	if (ret)
		return ret == -EUCLEAN ? mt6797_cci_fault(state, io, context) : ret;
	ret = io->get_uv(context, &uv);
	if (ret)
		return ret == -EUCLEAN ? mt6797_cci_fault(state, io, context) : ret;
	if (uv < 770000 || uv > 1200000)
		return -ERANGE;
	for (i = 0; i < MT6797_CCI_OPP_COUNT; i++)
		if (mt6797_cci_opps[i].hz <= hz && mt6797_cci_opps[i].uv <= uv)
			selected = i;
	if (selected < 0)
		return -ERANGE;
	ret = io->vote_uv(context, uv);
	if (ret)
		return mt6797_cci_fault(state, io, context);
	ret = io->get_uv(context, &actual);
	if (ret || actual < uv || actual > 1200000)
		return mt6797_cci_fault(state, io, context);
	if (hz != mt6797_cci_opps[selected].hz) {
		ret = io->set_hz(context, mt6797_cci_opps[selected].hz);
		if (ret)
			return mt6797_cci_fault(state, io, context);
	}
	ret = io->get_hz(context, &actual);
	if (ret || actual != mt6797_cci_opps[selected].hz)
		return mt6797_cci_fault(state, io, context);
	ret = io->vote_uv(context, mt6797_cci_opps[selected].uv);
	if (ret)
		return mt6797_cci_fault(state, io, context);
	ret = io->get_uv(context, &actual);
	if (ret || actual < mt6797_cci_opps[selected].uv || actual > 1200000)
		return mt6797_cci_fault(state, io, context);
	state->hz = mt6797_cci_opps[selected].hz;
	state->vote_uv = mt6797_cci_opps[selected].uv;
	state->ready = true;
	return 0;
}

static inline int mt6797_cci_transition(struct mt6797_cci_state *state,
				      const struct mt6797_cci_io *io,
				      void *context, unsigned int target)
{
	const struct mt6797_cci_opp *opp;
	u32 hz, uv;
	int ret;

	if (!state || !mt6797_cci_io_valid(io) || target >= MT6797_CCI_OPP_COUNT)
		return -EINVAL;
	if (state->faulted)
		return -EHOSTDOWN;
	if (!state->ready)
		return -EAGAIN;
	opp = &mt6797_cci_opps[target];
	ret = io->get_hz(context, &hz);
	if (ret)
		return ret == -EUCLEAN ? mt6797_cci_fault(state, io, context) : ret;
	ret = io->get_uv(context, &uv);
	if (ret)
		return ret == -EUCLEAN ? mt6797_cci_fault(state, io, context) : ret;
	if (hz != state->hz || uv < state->vote_uv || uv > 1200000)
		return mt6797_cci_fault(state, io, context);
	if (opp->hz == hz && opp->uv == state->vote_uv)
		return 0;

	/* Raise the vote before the clock; lower it only after clock readback. */
	if (opp->uv > state->vote_uv) {
		ret = io->vote_uv(context, opp->uv);
		if (ret)
			return mt6797_cci_fault(state, io, context);
		ret = io->get_uv(context, &uv);
		if (ret || uv < opp->uv || uv > 1200000)
			return mt6797_cci_fault(state, io, context);
	}
	if (opp->hz != hz) {
		ret = io->set_hz(context, opp->hz);
		if (ret)
			return mt6797_cci_fault(state, io, context);
	}
	ret = io->get_hz(context, &hz);
	if (ret || hz != opp->hz)
		return mt6797_cci_fault(state, io, context);
	if (opp->uv < state->vote_uv) {
		ret = io->vote_uv(context, opp->uv);
		if (ret)
			return mt6797_cci_fault(state, io, context);
	}
	ret = io->get_uv(context, &uv);
	if (ret || uv < opp->uv || uv > 1200000)
		return mt6797_cci_fault(state, io, context);
	state->hz = opp->hz;
	state->vote_uv = opp->uv;
	return 0;
}

#endif
