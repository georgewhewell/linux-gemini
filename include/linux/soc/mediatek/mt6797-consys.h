/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_CONSYS_LIFECYCLE_ABI	1

/*
 * ERROR is sticky.  A backend cleanup error means the core cannot establish
 * that the subsystem is off, so callers must not issue another power vote.
 */
enum mt6797_consys_state {
	MT6797_CONSYS_OFF,
	MT6797_CONSYS_ON,
	MT6797_CONSYS_ERROR,
};

enum mt6797_consys_poll {
	MT6797_CONSYS_POLL_POWER_ON,
	MT6797_CONSYS_POLL_MCU_READY,
	MT6797_CONSYS_POLL_POWER_OFF,
};

struct mt6797_consys_timeouts {
	u32 poll_interval_us;
	u32 power_on_timeout_us;
	u32 mcu_ready_timeout_us;
	u32 power_off_timeout_us;
};

/*
 * The backend owns all hardware access.  Each cleanup operation must be
 * idempotent and tolerate a failed matching setup operation: a setup write
 * can have taken effect before it reports an error.  poll() returns 0 when
 * the condition is true, -EAGAIN while it is pending, or another errno.
 *
 * No MT6797 register offsets or protection masks are represented here.  The
 * vendor records disagree on the bus-protection ownership, and a later
 * backend must be enabled only after that sequence is observed safely.
 */
struct mt6797_consys_ops {
	u32 abi;
	int (*enable_supplies)(void *context);
	int (*disable_supplies)(void *context);
	int (*enable_clock)(void *context);
	int (*disable_clock)(void *context);
	int (*power_on)(void *context);
	int (*power_off)(void *context);
	int (*release_reset)(void *context);
	int (*assert_reset)(void *context);
	int (*poll)(void *context, enum mt6797_consys_poll poll);
	void (*delay_us)(void *context, u32 min, u32 max);
};

struct mt6797_consys_lifecycle {
	const struct mt6797_consys_ops *ops;
	void *context;
	struct mt6797_consys_timeouts timeouts;
	/* Serializes state and users, including the physical transition. */
	struct mutex lock;
	enum mt6797_consys_state state;
	u32 users;
};

int mt6797_consys_lifecycle_init(
	struct mt6797_consys_lifecycle *lifecycle,
	const struct mt6797_consys_ops *ops, void *context,
	const struct mt6797_consys_timeouts *timeouts);
int mt6797_consys_get(struct mt6797_consys_lifecycle *lifecycle);
int mt6797_consys_put(struct mt6797_consys_lifecycle *lifecycle);
enum mt6797_consys_state
mt6797_consys_lifecycle_state(struct mt6797_consys_lifecycle *lifecycle);
u32 mt6797_consys_lifecycle_users(struct mt6797_consys_lifecycle *lifecycle);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H */
