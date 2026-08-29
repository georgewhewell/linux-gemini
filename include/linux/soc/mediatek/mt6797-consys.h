/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H

#include <linux/mutex.h>
#include <linux/types.h>

/* mtk_wcn_consys_hw.c:284 holds VCN18 for this verified interval. */
#define MT6797_CONSYS_LIFECYCLE_ABI	1
#define MT6797_CONSYS_VCN18_SETTLE_US	240

/*
 * ERROR is sticky.  A backend cleanup error means the core cannot establish
 * that the subsystem is off, so callers must not issue another identity vote.
 */
enum mt6797_consys_state {
	MT6797_CONSYS_OFF,
	MT6797_CONSYS_IDENTITY_HELD,
	MT6797_CONSYS_ERROR,
};

enum mt6797_consys_poll {
	MT6797_CONSYS_POLL_POWER_READY,
	MT6797_CONSYS_POLL_CHIP_READY,
};

struct mt6797_consys_timeouts {
	u32 poll_interval_us;
	u32 power_ready_timeout_us;
	u32 chip_ready_timeout_us;
};

/*
 * The backend owns all hardware access.  Each cleanup operation must be
 * idempotent and tolerate a failed matching setup operation: a setup write
 * can have taken effect before it reports an error.  poll() returns 0 when
 * the condition is true, -EAGAIN while it is pending, or another errno.
 *
 * No MT6797 register offsets or protection masks are represented here.  The
 * Vendor source at c5b0be85017ad0c599725e8273842efdbecdd88a,
 * drivers/misc/mediatek/connectivity/common/common_main/mt6797/
 * mtk_wcn_consys_hw.c, holds reset while it enables the SCP_SYS_CONN domain
 * before it observes the chip ID. This core stops at that observation; it
 * never releases reset, configures AFE, or loads
 * firmware. The same source's include/mtk_wcn_consys_hw.h:38 sets
 * CONSYS_AHB_CLK_MAGEMENT to 0, so an AHB bus-clock operation is deliberately
 * absent. The exact bus-protection masks remain disputed.
 */
struct mt6797_consys_ops {
	u32 abi;
	int (*enable_supplies)(void *context);
	int (*disable_supplies)(void *context);
	/* Prepare the topckgen sleep mask before reset/domain observation. */
	int (*prepare_identity)(void *context);
	int (*unprepare_identity)(void *context);
	int (*enable_domain_clock)(void *context);
	int (*disable_domain_clock)(void *context);
	int (*assert_reset)(void *context);
	int (*poll)(void *context, enum mt6797_consys_poll poll);
	void (*delay_us)(void *context, u32 min, u32 max);
};

struct mt6797_consys_lifecycle {
	const struct mt6797_consys_ops *ops;
	void *context;
	struct mt6797_consys_timeouts timeouts;
	/* Serializes state and users, including the identity transaction. */
	struct mutex lock;
	enum mt6797_consys_state state;
	u32 users;
};

int mt6797_consys_lifecycle_init(
	struct mt6797_consys_lifecycle *lifecycle,
	const struct mt6797_consys_ops *ops, void *context,
	const struct mt6797_consys_timeouts *timeouts);
int mt6797_consys_identity_get(struct mt6797_consys_lifecycle *lifecycle);
int mt6797_consys_identity_put(struct mt6797_consys_lifecycle *lifecycle);
enum mt6797_consys_state
mt6797_consys_lifecycle_state(struct mt6797_consys_lifecycle *lifecycle);
u32 mt6797_consys_lifecycle_users(struct mt6797_consys_lifecycle *lifecycle);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_H */
