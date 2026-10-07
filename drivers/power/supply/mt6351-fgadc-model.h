/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6351_FGADC_MODEL_H
#define _MT6351_FGADC_MODEL_H

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/power_supply.h>

#define MT6351_MODEL_PROFILES 4
#define MT6351_MODEL_POINTS 100
#define MT6351_MODEL_SCALE 1000000
#define MT6351_MODEL_MAX_GAP 60
#define MT6351_MODEL_RELAX_TIME 600

struct mt6351_fg_profile {
	int temp; /* tenths of a degree Celsius */
	int full_uah; /* model Qmax, not measured or design capacity */
	const struct power_supply_vbat_ri_table *resistance;
	int resistance_count;
	const struct power_supply_battery_ocv_table *ocv;
	int ocv_count;
};

struct mt6351_fg_model {
	struct mt6351_fg_profile profiles[MT6351_MODEL_PROFILES];
	int count;
};

struct mt6351_fg_sample {
	int voltage_uv;
	int current_ua; /* positive into the battery */
	int counter_uah;
	int temp;
	int status;
	u64 seconds; /* includes suspend */
};

struct mt6351_fg_estimate {
	bool initialized;
	bool ocv_valid;
	bool relaxed;
	int ocv_uv;
	int capacity;
	int counter_uah;
	s64 charge_uah;
	u64 seconds;
	u64 relaxed_since;
};

/* All callers interpolate inside validated, nonempty table bounds. */
static inline int mt6351_model_lerp(int x0, int y0, int x1, int y1, int x)
{
	if (x0 == x1)
		return y0;
	return y0 + div_s64((s64)(y1 - y0) * (x - x0), x1 - x0);
}

static inline int mt6351_model_validate(const struct mt6351_fg_model *m)
{
	int i, j;

	if (m->count < 1 || m->count > MT6351_MODEL_PROFILES)
		return -EINVAL;
	for (i = 0; i < m->count; i++) {
		const struct mt6351_fg_profile *p = &m->profiles[i];

		if (p->temp < -500 || p->temp > 1000 ||
		    (i && p->temp <= m->profiles[i - 1].temp) ||
		    p->full_uah < 100000 || p->full_uah > 100000000 ||
		    !p->resistance || p->resistance_count < 2 ||
		    p->resistance_count > MT6351_MODEL_POINTS ||
		    !p->ocv || p->ocv_count < 2 || p->ocv_count > MT6351_MODEL_POINTS)
			return -EINVAL;
		for (j = 0; j < p->resistance_count; j++) {
			const struct power_supply_vbat_ri_table *r = &p->resistance[j];

			if (r->vbat_uv < 2000000 || r->vbat_uv > 5000000 ||
			    r->ri_uohm < 1 || r->ri_uohm > 20000000 ||
			    (j && r->vbat_uv >= p->resistance[j - 1].vbat_uv))
				return -EINVAL;
		}
		for (j = 0; j < p->ocv_count; j++) {
			const struct power_supply_battery_ocv_table *o = &p->ocv[j];

			if (o->ocv < 2000000 || o->ocv > 5000000 ||
			    o->capacity < 0 || o->capacity > 100 ||
			    (j && (o->ocv >= p->ocv[j - 1].ocv ||
				   o->capacity > p->ocv[j - 1].capacity)))
				return -EINVAL;
		}
		if (p->ocv[0].capacity != 100 || p->ocv[p->ocv_count - 1].capacity)
			return -EINVAL;
	}
	return 0;
}

static inline int mt6351_model_resistance(const struct mt6351_fg_profile *p, int uv)
{
	const struct power_supply_vbat_ri_table *r = p->resistance;
	int i;

	if (uv >= r[0].vbat_uv)
		return r[0].ri_uohm;
	for (i = 1; i < p->resistance_count; i++)
		if (uv >= r[i].vbat_uv)
			return mt6351_model_lerp(r[i].vbat_uv, r[i].ri_uohm,
						r[i - 1].vbat_uv, r[i - 1].ri_uohm, uv);
	return r[p->resistance_count - 1].ri_uohm;
}

static inline int mt6351_model_soc(const struct mt6351_fg_profile *p, int uv)
{
	const struct power_supply_battery_ocv_table *o = p->ocv;
	int i;

	if (uv >= o[0].ocv)
		return MT6351_MODEL_SCALE;
	for (i = 1; i < p->ocv_count; i++)
		if (uv >= o[i].ocv)
			return mt6351_model_lerp(o[i].ocv, o[i].capacity * 10000,
						o[i - 1].ocv, o[i - 1].capacity * 10000, uv);
	return 0;
}

static inline s64 mt6351_model_loaded_voltage(const struct mt6351_fg_profile *lo,
					      const struct mt6351_fg_profile *hi,
					      int temp, int current_ua, int ocv)
{
	int r = mt6351_model_lerp(lo->temp, mt6351_model_resistance(lo, ocv),
				hi->temp, mt6351_model_resistance(hi, ocv), temp);

	return ocv + div_s64((s64)current_ua * r, 1000000);
}

/*
 * Solve Vterminal = OCV + I * R(OCV, T) on the union of resistance-table
 * segments. A high load can fold the curve: accept a unique root only.
 * Constant endpoint resistance extends the model to the bounded 2-5 V range.
 */
static inline int mt6351_model_ocv(const struct mt6351_fg_profile *lo,
				   const struct mt6351_fg_profile *hi,
				   const struct mt6351_fg_sample *s, int temp, int *ocv)
{
	const struct mt6351_fg_profile *profiles[] = { lo, hi };
	int indices[] = { lo->resistance_count - 1, hi->resistance_count - 1 };
	int left = 2000000, found = 0, root = 0;
	s64 target = s->voltage_uv;

	while (left < 5000000) {
		int i, right = 5000000, low, high, candidate;
		s64 vleft, vright;
		bool increasing;

		for (i = 0; i < ARRAY_SIZE(profiles); i++) {
			const struct power_supply_vbat_ri_table *r = profiles[i]->resistance;

			while (indices[i] >= 0 && r[indices[i]].vbat_uv <= left)
				indices[i]--;
			if (indices[i] >= 0)
				right = min(right, r[indices[i]].vbat_uv);
		}
		vleft = mt6351_model_loaded_voltage(lo, hi, temp, s->current_ua, left);
		vright = mt6351_model_loaded_voltage(lo, hi, temp, s->current_ua, right);
		if (target < min(vleft, vright) || target > max(vleft, vright)) {
			left = right;
			continue;
		}
		if (vleft == vright)
			return -ERANGE;
		increasing = vright > vleft;
		low = left;
		high = right;
		while (high - low > 1) {
			int mid = low + (high - low) / 2;
			s64 voltage = mt6351_model_loaded_voltage(lo, hi, temp,
								s->current_ua, mid);

			if ((voltage > target) == increasing)
				high = mid;
			else
				low = mid;
		}
		candidate = target == vright ? right : low;
		if (found && abs(candidate - root) > 1)
			return -ERANGE;
		root = candidate;
		found++;
		left = right;
	}
	if (!found)
		return -ERANGE;
	*ocv = root;
	return 0;
}

/*
 * Integrate measured coulombs between anchors. A long gap, a zeroed counter
 * or a step exceeding 5% of model Qmax loses continuity: use OCV again.
 * No hardware counter modulus or retention across suspend is assumed.
 */
static inline int mt6351_model_update(const struct mt6351_fg_model *m,
				      struct mt6351_fg_estimate *e,
				      const struct mt6351_fg_sample *s)
{
	const struct mt6351_fg_profile *lo, *hi;
	int index = 0, temp, full, max_full = 0, ocv = 0, soc = 0;
	s64 delta = (s64)s->counter_uah - e->counter_uah;
	bool anchor, ocv_valid, relaxed;

	if (s->voltage_uv < 2000000 || s->voltage_uv > 5000000 ||
	    s->temp < -500 || s->temp > 1000)
		return -ERANGE;
	for (index = 0; index < m->count; index++)
		max_full = max(max_full, m->profiles[index].full_uah);
	index = 0;
	while (index + 1 < m->count && s->temp > m->profiles[index + 1].temp)
		index++;
	lo = &m->profiles[index];
	hi = &m->profiles[min(index + 1, m->count - 1)];
	temp = clamp(s->temp, lo->temp, hi->temp);
	/* At exact endpoints, do not reject an unused neighboring load curve. */
	if (temp == lo->temp)
		hi = lo;
	else if (temp == hi->temp)
		lo = hi;
	full = mt6351_model_lerp(lo->temp, lo->full_uah, hi->temp, hi->full_uah, temp);
	ocv_valid = !mt6351_model_ocv(lo, hi, s, temp, &ocv);
	if (ocv_valid)
		soc = mt6351_model_lerp(lo->temp, mt6351_model_soc(lo, ocv),
					hi->temp, mt6351_model_soc(hi, ocv), temp);
	anchor = !e->initialized || s->seconds < e->seconds ||
		 s->seconds - e->seconds > MT6351_MODEL_MAX_GAP ||
		 (!s->counter_uah && e->counter_uah) ||
		 delta > full / 20 || delta < -full / 20;
	relaxed = s->current_ua < full / 50 && s->current_ua > -full / 50 &&
		  s->status != POWER_SUPPLY_STATUS_CHARGING &&
		  s->status != POWER_SUPPLY_STATUS_FULL;
	if (anchor || !relaxed || !e->relaxed)
		e->relaxed_since = s->seconds;
	if (relaxed && s->seconds - e->relaxed_since >= MT6351_MODEL_RELAX_TIME && ocv_valid) {
		anchor = true;
		e->relaxed_since = s->seconds;
	}
	if (s->status == POWER_SUPPLY_STATUS_FULL) {
		e->charge_uah = full;
	} else if (anchor) {
		if (!ocv_valid) {
			e->initialized = false;
			return -ENODATA;
		}
		e->charge_uah = div_s64((s64)soc * full, MT6351_MODEL_SCALE);
	} else {
		e->charge_uah += delta;
	}
	if (ocv_valid && !soc && s->current_ua <= 0 &&
	    s->status != POWER_SUPPLY_STATUS_FULL)
		e->charge_uah = 0;
	/* A temperature excursion must not destroy accumulated charge. */
	e->charge_uah = clamp_t(s64, e->charge_uah, 0, max_full);
	e->capacity = min_t(s64, 100, div_s64(e->charge_uah * 100 + full / 2, full));
	e->ocv_uv = ocv;
	e->ocv_valid = ocv_valid;
	e->counter_uah = s->counter_uah;
	e->seconds = s->seconds;
	e->relaxed = relaxed;
	e->initialized = true;
	return 0;
}

#endif
