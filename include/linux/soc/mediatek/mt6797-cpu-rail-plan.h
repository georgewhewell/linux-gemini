/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MT6797_CPU_RAIL_PLAN_H
#define _LINUX_MT6797_CPU_RAIL_PLAN_H

#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/soc/mediatek/mt6797-cpu-rails.h>

#define MT6797_CPU_VPROC_MIN_UV		770000U
#define MT6797_CPU_VPROC_STEP_UV		10000U
#define MT6797_CPU_VSRAM_STEP_UV		25000U
#define MT6797_CPU_RAIL_STEP_DELTA_UV	275000U

struct mt6797_cpu_rail_pair {
	u32 vproc_uv;
	u32 vsram_uv;
};

static inline bool
mt6797_cpu_rail_pair_valid(const struct mt6797_cpu_rail_pair *pair)
{
	return pair->vproc_uv >= MT6797_CPU_VPROC_MIN_UV &&
		pair->vproc_uv <= MT6797_CPU_VSRAM_MAX_UV &&
		!(pair->vproc_uv % MT6797_CPU_VPROC_STEP_UV) &&
		!(pair->vsram_uv % MT6797_CPU_VSRAM_STEP_UV) &&
		mt6797_cpu_rails_valid(pair->vproc_uv, pair->vsram_uv);
}

/* Inputs and outputs are exact selector voltages, never rounded by the writer. */
static inline int
mt6797_cpu_rail_target(u32 vproc_uv, struct mt6797_cpu_rail_pair *target)
{
	u32 vsram_uv;

	if (!target || vproc_uv < MT6797_CPU_VPROC_MIN_UV ||
	    vproc_uv > MT6797_CPU_VSRAM_MAX_UV ||
	    vproc_uv % MT6797_CPU_VPROC_STEP_UV)
		return -EINVAL;
	vsram_uv = clamp(vproc_uv + MT6797_CPU_VSRAM_NORMAL_DELTA_UV,
			 MT6797_CPU_VSRAM_MIN_UV, MT6797_CPU_VSRAM_MAX_UV);
	vsram_uv = ((vsram_uv + MT6797_CPU_VSRAM_STEP_UV - 1) /
		    MT6797_CPU_VSRAM_STEP_UV) * MT6797_CPU_VSRAM_STEP_UV;
	target->vproc_uv = vproc_uv;
	target->vsram_uv = vsram_uv;
	return 0;
}

/*
 * One verified rail write per step. Raise SRAM before VPROC; lower VPROC
 * before SRAM. Keep the vendor's 25 mV margin below the 300 mV maximum
 * while traversing the discrete selectors. A valid inherited pair need
 * not have the nominal offset. Each step moves strictly toward target.
 * Return 1 for a write, 0 when complete, or an error without changing next.
 */
static inline int
mt6797_cpu_rail_next(const struct mt6797_cpu_rail_pair *from,
		     u32 target_vproc_uv, struct mt6797_cpu_rail_pair *next)
{
	struct mt6797_cpu_rail_pair target, step;
	u32 limit;
	int ret;

	if (!from || !next || !mt6797_cpu_rail_pair_valid(from))
		return -EINVAL;
	ret = mt6797_cpu_rail_target(target_vproc_uv, &target);
	if (ret)
		return ret;
	step = *from;
	if (from->vproc_uv < target.vproc_uv) {
		limit = ((from->vproc_uv + MT6797_CPU_RAIL_STEP_DELTA_UV) /
			 MT6797_CPU_VSRAM_STEP_UV) * MT6797_CPU_VSRAM_STEP_UV;
		limit = min(limit, target.vsram_uv);
		if (from->vsram_uv < limit)
			step.vsram_uv = limit;
		else {
			limit = from->vsram_uv;
			if (limit < MT6797_CPU_VSRAM_MAX_UV)
				limit -= MT6797_CPU_VSRAM_NORMAL_DELTA_UV;
			limit = limit / MT6797_CPU_VPROC_STEP_UV *
				MT6797_CPU_VPROC_STEP_UV;
			step.vproc_uv = min(target.vproc_uv, limit);
		}
	} else if (from->vproc_uv > target.vproc_uv) {
		limit = from->vsram_uv - MT6797_CPU_RAIL_STEP_DELTA_UV;
		limit = ((limit + MT6797_CPU_VPROC_STEP_UV - 1) /
			 MT6797_CPU_VPROC_STEP_UV) * MT6797_CPU_VPROC_STEP_UV;
		limit = max(limit, target.vproc_uv);
		if (from->vproc_uv > limit)
			step.vproc_uv = limit;
		else {
			struct mt6797_cpu_rail_pair nominal;

			mt6797_cpu_rail_target(from->vproc_uv, &nominal);
			step.vsram_uv = max(target.vsram_uv, nominal.vsram_uv);
		}
	} else {
		step.vsram_uv = target.vsram_uv;
	}
	if (!mt6797_cpu_rail_pair_valid(&step))
		return -ERANGE;
	if (step.vproc_uv == from->vproc_uv &&
	    step.vsram_uv == from->vsram_uv) {
		if (step.vproc_uv != target.vproc_uv ||
		    step.vsram_uv != target.vsram_uv)
			return -ERANGE;
		*next = step;
		return 0;
	}
	*next = step;
	return 1;
}

#endif
