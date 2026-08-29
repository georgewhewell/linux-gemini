/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DA9213_LEGACY_OBSERVER_H
#define __DA9213_LEGACY_OBSERVER_H

#include <linux/types.h>

#define DA9213_LEGACY_BUCK_COUNT	2
#define DA9213_LEGACY_MIN_UV		300000
#define DA9213_LEGACY_MAX_UV		1570000
#define DA9213_LEGACY_STEP_UV		10000
#define DA9213_LEGACY_VSEL_MASK	0x7f
#define DA9213_LEGACY_ENABLE_MASK	0x01

enum da9213_legacy_observer_field {
	DA9213_LEGACY_OBSERVER_SELECTOR,
	DA9213_LEGACY_OBSERVER_ENABLED,
};

struct da9213_legacy_observer_buck {
	unsigned int selector;
	unsigned int microvolts;
	unsigned int enabled;
};

struct da9213_legacy_observation {
	bool valid;
	unsigned int identity_reads;
	unsigned int provider_count;
	unsigned int provider_read_attempts;
	unsigned int provider_read_completed;
	unsigned int register_data_writes;
	struct da9213_legacy_observer_buck buck[DA9213_LEGACY_BUCK_COUNT];
};

typedef int (*da9213_legacy_observer_read_fn)(
	void *context, unsigned int buck,
	enum da9213_legacy_observer_field field, unsigned int *value);

int da9213_legacy_observer_collect(
	da9213_legacy_observer_read_fn read, void *context,
	unsigned int identity_reads, unsigned int provider_count,
	struct da9213_legacy_observation *observation);
void da9213_legacy_observer_cleanup_state(
	struct da9213_legacy_observation *observation,
	unsigned int *provider_count);

#endif /* __DA9213_LEGACY_OBSERVER_H */
