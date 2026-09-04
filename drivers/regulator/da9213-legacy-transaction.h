/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DA9213_LEGACY_TRANSACTION_H
#define __DA9213_LEGACY_TRANSACTION_H

#include <linux/types.h>

#define DA9213_LEGACY_BUCK_COUNT	2
#define DA9213_LEGACY_MIN_UV		300000
#define DA9213_LEGACY_STEP_UV		10000
#define DA9213_LEGACY_VSEL_MASK		0x7f
#define DA9213_LEGACY_ENABLE_MASK	0x01

struct da9213_legacy_transaction_io {
	int (*read)(void *context, u8 reg, u8 *value);
	int (*write)(void *context, u8 reg, u8 value);
};

int da9213_legacy_update_bits_transaction(const struct da9213_legacy_transaction_io *io,
					  void *context, u8 reg, u8 mask,
					  u8 value);

#endif /* __DA9213_LEGACY_TRANSACTION_H */
