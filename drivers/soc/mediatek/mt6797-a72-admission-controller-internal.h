/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_A72_ADMISSION_CONTROLLER_INTERNAL_H
#define __MT6797_A72_ADMISSION_CONTROLLER_INTERNAL_H

#include <asm/mt6797_a72_membership.h>

#include <linux/atomic.h>
#include <linux/gemini_admission_trace.h>
#include <linux/types.h>

struct arm64_late_cpu_ready_token;

struct mt6797_a72_admission_controller_ops {
	bool allow_trace_failure;
	bool (*binder_ready)(void *context);
	const struct arm64_late_cpu_ready_token *(*ready_token)(void *context);
	int (*trace_entry)(void *context);
	int (*trace_zero_request)(void *context, enum gemini_admission_trace_zero_result result);
	int (*source_register)(void *context);
	void (*source_unregister)(void *context);
	int (*derive_cpu8)(void *context,
			   const struct arm64_late_cpu_ready_token *ready,
			   struct mt6797_a72_transaction *transaction);
	int (*publish_up)(void *context,
			  struct mt6797_a72_transaction *transaction);
	int (*add_cpu)(void *context, unsigned int cpu);
};

#define MT6797_A72_ADMISSION_TRIGGER_TOKEN \
	"run-a72-admission-20260828-a\n"

struct mt6797_a72_admission_trigger_ops {
	int (*execute)(void *context);
};

struct mt6797_a72_admission_trigger_state {
	atomic_t consumed;
	bool complete;
	u32 executions;
	int operation_ret;
};

struct mt6797_a72_admission_controller_state {
	atomic_t consumed;
	u32 cpu_requests;
	u32 failure_stage;
	u32 derive_stage;
	int trace_snapshot_ret;
	struct gemini_admission_trace_snapshot trace_snapshot;
	int trace_entry_ret;
	int trace_ret;
	int operation_ret;
	struct mt6797_a72_transaction transaction;
};

void mt6797_a72_admission_trigger_state_init(struct mt6797_a72_admission_trigger_state *state);
int mt6797_a72_admission_trigger_run(struct mt6797_a72_admission_trigger_state *state,
				     const struct mt6797_a72_admission_trigger_ops *ops,
				     void *context, const char *buf, size_t count);

void
mt6797_a72_admission_state_init(struct mt6797_a72_admission_controller_state *state);
int
mt6797_a72_admission_run(struct mt6797_a72_admission_controller_state *state,
			 const struct mt6797_a72_admission_controller_ops *ops,
			 void *context);

#endif /* __MT6797_A72_ADMISSION_CONTROLLER_INTERNAL_H */
