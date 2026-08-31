/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_GEMINI_TRANSITION_LEDGER_H
#define _LINUX_GEMINI_TRANSITION_LEDGER_H

#include <linux/errno.h>
#include <linux/types.h>

enum gemini_transition_ledger_phase {
	GEMINI_TRANSITION_LEDGER_BEFORE = 1,
	GEMINI_TRANSITION_LEDGER_AFTER,
	GEMINI_TRANSITION_LEDGER_TERMINAL,
};

struct gemini_transition_ledger_snapshot {
	u64 attempt_id;
	u32 generation;
	u32 phase;
	u32 stage;
	u32 terminal;
};

#ifdef CONFIG_PSTORE_GEMINI_TRANSITION_LEDGER
int gemini_transition_ledger_prepare(void);
int gemini_transition_ledger_snapshot(struct gemini_transition_ledger_snapshot *snapshot);
int gemini_transition_ledger_ack_terminal(
	const struct gemini_transition_ledger_snapshot *expected);
int gemini_transition_ledger_begin(u64 attempt_id);
int gemini_transition_ledger_checkpoint(u64 attempt_id, u32 phase,
					u32 stage, u32 terminal);
#else
static inline int gemini_transition_ledger_prepare(void)
{
	return -EOPNOTSUPP;
}

static inline int
gemini_transition_ledger_snapshot(struct gemini_transition_ledger_snapshot *snapshot)
{
	if (snapshot)
		*snapshot = (struct gemini_transition_ledger_snapshot){};
	return -EOPNOTSUPP;
}

static inline int gemini_transition_ledger_ack_terminal(
	const struct gemini_transition_ledger_snapshot *expected)
{
	return -EOPNOTSUPP;
}

static inline int gemini_transition_ledger_begin(u64 attempt_id)
{
	return -EOPNOTSUPP;
}

static inline int gemini_transition_ledger_checkpoint(u64 attempt_id, u32 phase,
						      u32 stage, u32 terminal)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_GEMINI_TRANSITION_LEDGER_H */
