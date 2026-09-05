/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * One-way MT6797 MD1 watchdog-key acceptance gate.
 *
 * The vendor start sequence supplies two key writes, but no post-write MODE
 * acknowledgement, inverse, or ownership-transfer predicate. This core
 * records exactly what was attempted; it never calls that success "disabled".
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_WDT_GATE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_WDT_GATE_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_MD1_WDT_GATE_ABI	1

enum mt6797_md1_wdt_gate_state {
	MT6797_MD1_WDT_GATE_OFF,
	MT6797_MD1_WDT_GATE_PREFLIGHT_ACCEPTED,
	MT6797_MD1_WDT_GATE_MD_KEY_ATTEMPTED,
	/* Both write-only vendor key commands returned; effect is not read back. */
	MT6797_MD1_WDT_GATE_KEYS_ATTEMPTED_WRITE_ONLY,
	MT6797_MD1_WDT_GATE_FATAL,
};

/* This must be supplied only by a future owner of the cold-owner terminal state. */
struct mt6797_md1_wdt_gate_preflight {
	bool cold_owner_pll_ready_wdt_unowned;
};

/* Values are diagnostic samples, not a success acknowledgement or mode readback. */
struct mt6797_md1_wdt_status_observation {
	u32 md_status;
	u32 l1_status;
};

struct mt6797_md1_wdt_gate_ops {
	u32 abi;
	int (*observe_status)(void *context,
		struct mt6797_md1_wdt_status_observation *observation);
	/* Each command is irreversible from this core's perspective. */
	int (*write_md_mode_key)(void *context);
	int (*write_l1_mode_key)(void *context);
};

struct mt6797_md1_wdt_gate {
	const struct mt6797_md1_wdt_gate_ops *ops;
	void *context;
	struct mutex lock;
	/* Samples immediately before and after the two vendor key writes. */
	struct mt6797_md1_wdt_status_observation observation;
	struct mt6797_md1_wdt_status_observation post_observation;
	enum mt6797_md1_wdt_gate_state state;
	int last_error;
};

int mt6797_md1_wdt_gate_init(struct mt6797_md1_wdt_gate *gate,
	const struct mt6797_md1_wdt_gate_ops *ops, void *context);
int mt6797_md1_wdt_gate_run(struct mt6797_md1_wdt_gate *gate,
	const struct mt6797_md1_wdt_gate_preflight *preflight);
enum mt6797_md1_wdt_gate_state
mt6797_md1_wdt_gate_state(struct mt6797_md1_wdt_gate *gate);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_WDT_GATE_H */
