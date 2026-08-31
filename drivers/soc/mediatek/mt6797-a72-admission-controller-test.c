// SPDX-License-Identifier: GPL-2.0-only

#include <asm/late_cpu_profile.h>

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mt6797-a72-admission-controller-internal.h"

enum mt6797_a72_admission_test_event {
	MT6797_ADMISSION_TRACE_ENTRY,
	MT6797_ADMISSION_BINDER_READY,
	MT6797_ADMISSION_READY_TOKEN,
	MT6797_ADMISSION_SOURCE_REGISTER,
	MT6797_ADMISSION_DERIVE,
	MT6797_ADMISSION_PUBLISH,
	MT6797_ADMISSION_ADD_CPU,
	MT6797_ADMISSION_SOURCE_UNREGISTER,
	MT6797_ADMISSION_TRACE_ZERO_REQUEST,
};

struct mt6797_a72_admission_test_context {
	struct mt6797_a72_admission_controller_state controller;
	struct arm64_late_cpu_ready_token ready;
	enum mt6797_a72_admission_test_event events[16];
	unsigned int event_count;
	int fail_event;
	bool binder_ready;
	bool ready_available;
	bool consumed_before_operation;
	bool same_task;
	struct task_struct *operation_task;
	unsigned int requested_cpu;
	enum gemini_admission_trace_zero_result zero_result;
	bool trace_zero_fails;
	unsigned int trigger_execute_calls;
	int trigger_execute_ret;
};

static void
mt6797_a72_admission_test_event(struct mt6797_a72_admission_test_context *context,
				enum mt6797_a72_admission_test_event event,
				bool operation)
{
	if (context->event_count < ARRAY_SIZE(context->events))
		context->events[context->event_count++] = event;
	if (!operation)
		return;
	if (!atomic_read(&context->controller.consumed))
		context->consumed_before_operation = false;
	if (!context->operation_task)
		context->operation_task = current;
	else if (context->operation_task != current)
		context->same_task = false;
}

static int mt6797_a72_admission_test_trace_entry(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_TRACE_ENTRY, false);
	return context->fail_event == MT6797_ADMISSION_TRACE_ENTRY ? -ENOSPC : 0;
}

static int
mt6797_a72_admission_test_trace_zero_request(void *data,
					     enum gemini_admission_trace_zero_result result)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_TRACE_ZERO_REQUEST,
					false);
	context->zero_result = result;
	return context->trace_zero_fails ? -ENOSPC : 0;
}

static bool mt6797_a72_admission_test_binder_ready(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_BINDER_READY, false);
	return context->binder_ready;
}

static const struct arm64_late_cpu_ready_token *
mt6797_a72_admission_test_ready_token(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_READY_TOKEN, false);
	return context->ready_available ? &context->ready : NULL;
}

static int mt6797_a72_admission_test_register(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_SOURCE_REGISTER, true);
	return context->fail_event == MT6797_ADMISSION_SOURCE_REGISTER ?
		-EIO : 0;
}

static void mt6797_a72_admission_test_unregister(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context,
					MT6797_ADMISSION_SOURCE_UNREGISTER, true);
}

static int
mt6797_a72_admission_test_derive(void *data,
				 const struct arm64_late_cpu_ready_token *ready,
				 struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context, MT6797_ADMISSION_DERIVE, true);
	if (ready != &context->ready)
		return -EINVAL;
	if (context->fail_event == MT6797_ADMISSION_DERIVE)
		return -EIO;
	transaction->valid = 1;
	return 0;
}

static int
mt6797_a72_admission_test_publish(void *data,
				  struct mt6797_a72_transaction *transaction)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context, MT6797_ADMISSION_PUBLISH, true);
	if (!transaction->valid)
		return -EINVAL;
	return context->fail_event == MT6797_ADMISSION_PUBLISH ? -EIO : 0;
}

static int
mt6797_a72_admission_test_add_cpu(void *data, unsigned int cpu)
{
	struct mt6797_a72_admission_test_context *context = data;

	mt6797_a72_admission_test_event(context, MT6797_ADMISSION_ADD_CPU, true);
	context->requested_cpu = cpu;
	return context->fail_event == MT6797_ADMISSION_ADD_CPU ? -EIO : 0;
}

static const struct mt6797_a72_admission_controller_ops test_ops = {
	.binder_ready = mt6797_a72_admission_test_binder_ready,
	.trace_entry = mt6797_a72_admission_test_trace_entry,
	.trace_zero_request = mt6797_a72_admission_test_trace_zero_request,
	.ready_token = mt6797_a72_admission_test_ready_token,
	.source_register = mt6797_a72_admission_test_register,
	.source_unregister = mt6797_a72_admission_test_unregister,
	.derive_cpu8 = mt6797_a72_admission_test_derive,
	.publish_up = mt6797_a72_admission_test_publish,
	.add_cpu = mt6797_a72_admission_test_add_cpu,
};

static int mt6797_a72_admission_test_trigger_execute(void *data)
{
	struct mt6797_a72_admission_test_context *context = data;

	context->trigger_execute_calls++;
	return context->trigger_execute_ret;
}

static const struct mt6797_a72_admission_trigger_ops test_trigger_ops = {
	.execute = mt6797_a72_admission_test_trigger_execute,
};

static struct mt6797_a72_admission_test_context *
mt6797_a72_admission_test_context(struct kunit *test)
{
	struct mt6797_a72_admission_test_context *context;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	if (!context)
		return NULL;
	mt6797_a72_admission_state_init(&context->controller);
	context->fail_event = -1;
	context->binder_ready = true;
	context->ready_available = true;
	context->consumed_before_operation = true;
	context->same_task = true;
	return context;
}

static void mt6797_a72_admission_success_test(struct kunit *test)
{
	static const enum mt6797_a72_admission_test_event expected[] = {
		MT6797_ADMISSION_TRACE_ENTRY,
		MT6797_ADMISSION_BINDER_READY,
		MT6797_ADMISSION_READY_TOKEN,
		MT6797_ADMISSION_SOURCE_REGISTER,
		MT6797_ADMISSION_DERIVE,
		MT6797_ADMISSION_PUBLISH,
		MT6797_ADMISSION_ADD_CPU,
		MT6797_ADMISSION_SOURCE_UNREGISTER,
	};
	struct mt6797_a72_admission_test_context *context =
		mt6797_a72_admission_test_context(test);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, context);
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, context->event_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, memcmp(context->events, expected,
				     sizeof(expected)), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->controller.consumed), 1);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)1);
	KUNIT_EXPECT_EQ(test, context->requested_cpu, 8U);
	KUNIT_EXPECT_TRUE(test, context->consumed_before_operation);
	KUNIT_EXPECT_TRUE(test, context->same_task);
}

static void mt6797_a72_admission_preconsume_gates_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context *context;
	int ret;

	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->binder_ready = false;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->controller.consumed), 0);
	KUNIT_EXPECT_EQ(test, context->event_count, 2U);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)0);

	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->ready_available = false;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->controller.consumed), 0);
	KUNIT_EXPECT_EQ(test, context->event_count, 3U);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)0);
}

static void mt6797_a72_admission_terminal_failures_test(struct kunit *test)
{
	static const enum gemini_admission_trace_zero_result results[] = {
		GEMINI_ADMISSION_TRACE_ZERO_SOURCE_REGISTER,
		GEMINI_ADMISSION_TRACE_ZERO_DERIVE,
		GEMINI_ADMISSION_TRACE_ZERO_PUBLISH,
	};
	static const int failures[] = {
		MT6797_ADMISSION_SOURCE_REGISTER,
		MT6797_ADMISSION_DERIVE,
		MT6797_ADMISSION_PUBLISH,
	};
	struct mt6797_a72_admission_test_context *context;
	unsigned int events;
	size_t failure;
	int ret;

	for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
		context = mt6797_a72_admission_test_context(test);
		KUNIT_ASSERT_NOT_NULL(test, context);
		context->fail_event = failures[failure];
		ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test,
				atomic_read(&context->controller.consumed), 1);
		KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)0);
		KUNIT_EXPECT_EQ(test, context->events[context->event_count - 1],
				MT6797_ADMISSION_TRACE_ZERO_REQUEST);
		KUNIT_EXPECT_EQ(test, context->zero_result,
				results[failure]);
		KUNIT_EXPECT_EQ(test, context->controller.failure_stage,
				(u32)results[failure]);
		KUNIT_EXPECT_TRUE(test, context->consumed_before_operation);
		events = context->event_count;
		ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
		KUNIT_EXPECT_EQ(test, ret, -EALREADY);
		KUNIT_EXPECT_EQ(test, context->event_count, events);
	}
}

static void mt6797_a72_admission_request_failure_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context *context =
		mt6797_a72_admission_test_context(test);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, context);
	context->fail_event = MT6797_ADMISSION_ADD_CPU;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)1);
	KUNIT_EXPECT_EQ(test, context->requested_cpu, 8U);
	KUNIT_EXPECT_EQ(test, context->events[context->event_count - 1],
			MT6797_ADMISSION_SOURCE_UNREGISTER);
	KUNIT_EXPECT_TRUE(test, context->consumed_before_operation);
}

static void mt6797_a72_admission_trace_failures_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context *context;
	int ret;

	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->fail_event = MT6797_ADMISSION_TRACE_ENTRY;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->controller.consumed), 0);
	KUNIT_EXPECT_EQ(test, context->event_count, 1U);

	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->fail_event = MT6797_ADMISSION_SOURCE_REGISTER;
	context->trace_zero_fails = true;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, context->controller.trace_ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)0);
	KUNIT_EXPECT_EQ(test, context->events[context->event_count - 1],
			MT6797_ADMISSION_TRACE_ZERO_REQUEST);
}

static void mt6797_a72_admission_live_trace_softfail_test(struct kunit *test)
{
	struct mt6797_a72_admission_controller_ops live_ops = test_ops;
	struct mt6797_a72_admission_test_context *context;
	int ret;

	live_ops.allow_trace_failure = true;
	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->fail_event = MT6797_ADMISSION_TRACE_ENTRY;
	ret = mt6797_a72_admission_run(&context->controller, &live_ops, context);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, context->controller.trace_entry_ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->controller.consumed), 1);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)1);
	KUNIT_EXPECT_EQ(test, context->requested_cpu, 8U);

	context = mt6797_a72_admission_test_context(test);
	KUNIT_ASSERT_NOT_NULL(test, context);
	context->fail_event = MT6797_ADMISSION_SOURCE_REGISTER;
	context->trace_zero_fails = true;
	ret = mt6797_a72_admission_run(&context->controller, &live_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, context->controller.trace_entry_ret, 0);
	KUNIT_EXPECT_EQ(test, context->controller.trace_ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, context->controller.operation_ret, -EIO);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)0);
}

static void mt6797_a72_admission_repeat_closed_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context *context =
		mt6797_a72_admission_test_context(test);
	unsigned int events;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, context);
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_ASSERT_EQ(test, ret, 0);
	events = context->event_count;
	ret = mt6797_a72_admission_run(&context->controller, &test_ops, context);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, context->event_count, events);
	KUNIT_EXPECT_EQ(test, context->controller.cpu_requests, (u32)1);
}

static void mt6797_a72_admission_trigger_invalid_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context context = { };
	struct mt6797_a72_admission_trigger_state trigger;
	int ret;

	mt6797_a72_admission_trigger_state_init(&trigger);
	ret = mt6797_a72_admission_trigger_run(&trigger, &test_trigger_ops,
					       &context, MT6797_A72_ADMISSION_TRIGGER_TOKEN,
					       sizeof(MT6797_A72_ADMISSION_TRIGGER_TOKEN) - 2);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, atomic_read(&trigger.consumed), 0);
	KUNIT_EXPECT_FALSE(test, trigger.complete);
	KUNIT_EXPECT_EQ(test, trigger.executions, (u32)0);
	KUNIT_EXPECT_EQ(test, context.trigger_execute_calls, 0U);
}

static void mt6797_a72_admission_trigger_terminal_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context context = { };
	struct mt6797_a72_admission_trigger_state trigger;
	int ret;

	mt6797_a72_admission_trigger_state_init(&trigger);
	context.trigger_execute_ret = -EIO;
	ret = mt6797_a72_admission_trigger_run(&trigger, &test_trigger_ops,
					       &context, MT6797_A72_ADMISSION_TRIGGER_TOKEN,
					       sizeof(MT6797_A72_ADMISSION_TRIGGER_TOKEN) - 1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&trigger.consumed), 1);
	KUNIT_EXPECT_TRUE(test, trigger.complete);
	KUNIT_EXPECT_EQ(test, trigger.executions, (u32)1);
	KUNIT_EXPECT_EQ(test, trigger.operation_ret, -EIO);
	KUNIT_EXPECT_EQ(test, context.trigger_execute_calls, 1U);
}

static void mt6797_a72_admission_trigger_repeat_closed_test(struct kunit *test)
{
	struct mt6797_a72_admission_test_context context = { };
	struct mt6797_a72_admission_trigger_state trigger;
	int ret;

	mt6797_a72_admission_trigger_state_init(&trigger);
	ret = mt6797_a72_admission_trigger_run(&trigger, &test_trigger_ops,
					       &context, MT6797_A72_ADMISSION_TRIGGER_TOKEN,
					       sizeof(MT6797_A72_ADMISSION_TRIGGER_TOKEN) - 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_admission_trigger_run(&trigger, &test_trigger_ops,
					       &context, MT6797_A72_ADMISSION_TRIGGER_TOKEN,
					       sizeof(MT6797_A72_ADMISSION_TRIGGER_TOKEN) - 1);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, trigger.executions, (u32)1);
	KUNIT_EXPECT_EQ(test, context.trigger_execute_calls, 1U);
}

static struct kunit_case mt6797_a72_admission_controller_cases[] = {
	KUNIT_CASE(mt6797_a72_admission_trigger_invalid_test),
	KUNIT_CASE(mt6797_a72_admission_trigger_terminal_test),
	KUNIT_CASE(mt6797_a72_admission_trigger_repeat_closed_test),
	KUNIT_CASE(mt6797_a72_admission_success_test),
	KUNIT_CASE(mt6797_a72_admission_preconsume_gates_test),
	KUNIT_CASE(mt6797_a72_admission_terminal_failures_test),
	KUNIT_CASE(mt6797_a72_admission_request_failure_test),
	KUNIT_CASE(mt6797_a72_admission_trace_failures_test),
	KUNIT_CASE(mt6797_a72_admission_live_trace_softfail_test),
	KUNIT_CASE(mt6797_a72_admission_repeat_closed_test),
	{ }
};

static struct kunit_suite mt6797_a72_admission_controller_suite = {
	.name = "mt6797-a72-admission-controller",
	.test_cases = mt6797_a72_admission_controller_cases,
};

kunit_test_suite(mt6797_a72_admission_controller_suite);

MODULE_LICENSE("GPL");
