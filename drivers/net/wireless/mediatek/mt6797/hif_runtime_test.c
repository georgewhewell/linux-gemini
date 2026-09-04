// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kthread.h>

#include "hif_runtime.h"

enum mt6797_wifi_hif_test_event {
	TEST_PRECHECK,
	TEST_MASK,
	TEST_SAVE,
	TEST_PROGRAM,
	TEST_CLEAR,
	TEST_RUNTIME,
	TEST_QUIESCING,
	TEST_QUIESCENT,
	TEST_TERMINAL,
	TEST_DEVICE_ENABLE,
	TEST_LINUX_ENABLE,
	TEST_UNMASK,
	TEST_DEVICE_DISABLE,
	TEST_LINUX_DISABLE,
	TEST_RESTORE,
	TEST_FENCE,
	TEST_TEARDOWN,
};

struct mt6797_wifi_hif_test_context {
	struct kunit *test;
	enum mt6797_wifi_hif_test_event events[32];
	enum mt6797_wifi_hif_test_event fail_at;
	unsigned int event_count;
	int mode;
};

static int mt6797_wifi_hif_test_record(
	struct mt6797_wifi_hif_test_context *context,
	enum mt6797_wifi_hif_test_event event)
{
	KUNIT_ASSERT_LT(context->test, context->event_count,
			(unsigned int)ARRAY_SIZE(context->events));
	context->events[context->event_count++] = event;
	return context->fail_at == event ? -EIO : 0;
}

#define MT6797_WIFI_HIF_TEST_INT_CALLBACK(_name, _event) \
	static int mt6797_wifi_hif_test_##_name(void *data) \
	{ \
		return mt6797_wifi_hif_test_record(data, _event); \
	}

#define MT6797_WIFI_HIF_TEST_VOID_CALLBACK(_name, _event) \
	static void mt6797_wifi_hif_test_##_name(void *data) \
	{ \
		mt6797_wifi_hif_test_record(data, _event); \
	}

MT6797_WIFI_HIF_TEST_INT_CALLBACK(precheck, TEST_PRECHECK)
MT6797_WIFI_HIF_TEST_VOID_CALLBACK(mask, TEST_MASK)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(save, TEST_SAVE)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(program, TEST_PROGRAM)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(clear, TEST_CLEAR)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(device_enable, TEST_DEVICE_ENABLE)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(linux_enable, TEST_LINUX_ENABLE)
MT6797_WIFI_HIF_TEST_VOID_CALLBACK(unmask, TEST_UNMASK)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(device_disable, TEST_DEVICE_DISABLE)
MT6797_WIFI_HIF_TEST_VOID_CALLBACK(linux_disable, TEST_LINUX_DISABLE)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(restore, TEST_RESTORE)
MT6797_WIFI_HIF_TEST_VOID_CALLBACK(fence, TEST_FENCE)
MT6797_WIFI_HIF_TEST_INT_CALLBACK(teardown, TEST_TEARDOWN)

static void mt6797_wifi_hif_test_publish(
	void *data, enum mt6797_wifi_hif_irq_mode mode)
{
	struct mt6797_wifi_hif_test_context *context = data;
	enum mt6797_wifi_hif_test_event event;

	switch (mode) {
	case MT6797_WIFI_HIF_IRQ_RUNTIME:
		event = TEST_RUNTIME;
		break;
	case MT6797_WIFI_HIF_IRQ_QUIESCING:
		event = TEST_QUIESCING;
		break;
	case MT6797_WIFI_HIF_IRQ_QUIESCENT:
		event = TEST_QUIESCENT;
		break;
	case MT6797_WIFI_HIF_IRQ_TERMINAL:
		event = TEST_TERMINAL;
		break;
	default:
		KUNIT_FAIL(context->test, "unexpected HIF mode %u", mode);
		return;
	}
	context->mode = mode;
	mt6797_wifi_hif_test_record(context, event);
}

static void mt6797_wifi_hif_test_init(
	struct mt6797_wifi_hif_test_context *context,
	struct mt6797_wifi_hif_runtime_ops *ops,
	struct mt6797_wifi_hif_runtime_transaction *transaction)
{
	*transaction = (struct mt6797_wifi_hif_runtime_transaction) {};
	*ops = (struct mt6797_wifi_hif_runtime_ops) {
		.precheck = mt6797_wifi_hif_test_precheck,
		.mask_wrapper = mt6797_wifi_hif_test_mask,
		.save_registers = mt6797_wifi_hif_test_save,
		.program_runtime = mt6797_wifi_hif_test_program,
		.clear_status = mt6797_wifi_hif_test_clear,
		.publish_mode = mt6797_wifi_hif_test_publish,
		.enable_device = mt6797_wifi_hif_test_device_enable,
		.enable_linux = mt6797_wifi_hif_test_linux_enable,
		.unmask_wrapper = mt6797_wifi_hif_test_unmask,
		.disable_device = mt6797_wifi_hif_test_device_disable,
		.disable_linux = mt6797_wifi_hif_test_linux_disable,
		.restore_registers = mt6797_wifi_hif_test_restore,
		.fence = mt6797_wifi_hif_test_fence,
		.teardown = mt6797_wifi_hif_test_teardown,
		.context = context,
	};
}

static void mt6797_wifi_hif_test_expect(
	struct kunit *test, struct mt6797_wifi_hif_test_context *context,
	const enum mt6797_wifi_hif_test_event *expected, size_t count)
{
	unsigned int index;

	KUNIT_ASSERT_EQ(test, context->event_count, (unsigned int)count);
	for (index = 0; index < count; index++)
		KUNIT_EXPECT_EQ(test, context->events[index], expected[index]);
}

static void mt6797_wifi_hif_start_stop_test(struct kunit *test)
{
	static const enum mt6797_wifi_hif_test_event start_expected[] = {
		TEST_PRECHECK, TEST_MASK, TEST_SAVE, TEST_PROGRAM, TEST_CLEAR,
		TEST_RUNTIME, TEST_DEVICE_ENABLE, TEST_LINUX_ENABLE, TEST_UNMASK,
	};
	static const enum mt6797_wifi_hif_test_event stop_expected[] = {
		TEST_QUIESCING, TEST_MASK, TEST_DEVICE_DISABLE,
		TEST_LINUX_DISABLE, TEST_MASK, TEST_RESTORE, TEST_TEARDOWN,
		TEST_QUIESCENT,
	};
	struct mt6797_wifi_hif_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	mt6797_wifi_hif_test_init(&context, &ops, &transaction);
	ret = mt6797_wifi_hif_runtime_transaction_start(&ops, &transaction);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.active);
	mt6797_wifi_hif_test_expect(test, &context, start_expected,
				      ARRAY_SIZE(start_expected));
	context.event_count = 0;
	ret = mt6797_wifi_hif_runtime_transaction_stop(&ops, &transaction,
							 false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, transaction.active);
	KUNIT_EXPECT_FALSE(test, transaction.terminal);
	mt6797_wifi_hif_test_expect(test, &context, stop_expected,
				      ARRAY_SIZE(stop_expected));
}

static void mt6797_wifi_hif_program_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_hif_test_event expected[] = {
		TEST_PRECHECK, TEST_MASK, TEST_SAVE, TEST_PROGRAM, TEST_TERMINAL,
		TEST_MASK, TEST_RESTORE, TEST_FENCE, TEST_TEARDOWN, TEST_TERMINAL,
	};
	struct mt6797_wifi_hif_test_context context = {
		.test = test,
		.fail_at = TEST_PROGRAM,
	};
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	mt6797_wifi_hif_test_init(&context, &ops, &transaction);
	ret = mt6797_wifi_hif_runtime_transaction_start(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_TRUE(test, transaction.terminal);
	KUNIT_EXPECT_FALSE(test, transaction.active);
	mt6797_wifi_hif_test_expect(test, &context, expected,
				      ARRAY_SIZE(expected));
}

static void mt6797_wifi_hif_device_enable_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_hif_test_event expected[] = {
		TEST_PRECHECK, TEST_MASK, TEST_SAVE, TEST_PROGRAM, TEST_CLEAR,
		TEST_RUNTIME, TEST_DEVICE_ENABLE, TEST_TERMINAL, TEST_MASK,
		TEST_DEVICE_DISABLE, TEST_MASK, TEST_RESTORE, TEST_FENCE,
		TEST_TEARDOWN, TEST_TERMINAL,
	};
	struct mt6797_wifi_hif_test_context context = {
		.test = test,
		.fail_at = TEST_DEVICE_ENABLE,
	};
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	mt6797_wifi_hif_test_init(&context, &ops, &transaction);
	ret = mt6797_wifi_hif_runtime_transaction_start(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_TRUE(test, transaction.terminal);
	mt6797_wifi_hif_test_expect(test, &context, expected,
				      ARRAY_SIZE(expected));
}

static void mt6797_wifi_hif_restore_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_hif_test_event expected[] = {
		TEST_QUIESCING, TEST_MASK, TEST_DEVICE_DISABLE,
		TEST_LINUX_DISABLE, TEST_MASK, TEST_RESTORE, TEST_FENCE,
		TEST_TEARDOWN, TEST_TERMINAL,
	};
	struct mt6797_wifi_hif_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	mt6797_wifi_hif_test_init(&context, &ops, &transaction);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_transaction_start(&ops, &transaction), 0);
	context.event_count = 0;
	context.fail_at = TEST_RESTORE;
	ret = mt6797_wifi_hif_runtime_transaction_stop(&ops, &transaction,
							 false);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_TRUE(test, transaction.terminal);
	mt6797_wifi_hif_test_expect(test, &context, expected,
				      ARRAY_SIZE(expected));
}

static void mt6797_wifi_hif_terminal_transition_test(struct kunit *test)
{
	int mode = MT6797_WIFI_HIF_IRQ_RUNTIME;

	KUNIT_EXPECT_TRUE(test, mt6797_wifi_hif_runtime_mark_terminal(&mode));
	KUNIT_EXPECT_EQ(test, mode, (int)MT6797_WIFI_HIF_IRQ_TERMINAL);
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_hif_runtime_mark_terminal(&mode));
	mode = MT6797_WIFI_HIF_IRQ_CAPABILITY;
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_hif_runtime_mark_terminal(&mode));
	KUNIT_EXPECT_EQ(test, mode, (int)MT6797_WIFI_HIF_IRQ_CAPABILITY);
}

struct mt6797_wifi_hif_terminal_race {
	struct mt6797_wifi_hif_irq *irq;
	struct completion start;
	struct completion publisher_done;
	struct completion sender_done;
	int sender_status;
	bool published;
};

static int mt6797_wifi_hif_terminal_publisher(void *data)
{
	struct mt6797_wifi_hif_terminal_race *race = data;

	wait_for_completion(&race->start);
	race->published = mt6797_wifi_hif_runtime_publish_terminal(race->irq,
								 -EUCLEAN);
	complete(&race->publisher_done);
	return 0;
}

static int mt6797_wifi_hif_terminal_sender(void *data)
{
	struct mt6797_wifi_hif_terminal_race *race = data;

	wait_for_completion(&race->start);
	race->sender_status = mt6797_wifi_hif_runtime_status(race->irq);
	complete(&race->sender_done);
	return 0;
}

static void mt6797_wifi_hif_terminal_reason_race_test(struct kunit *test)
{
	struct mt6797_wifi_hif_irq irq = {
		.mode = MT6797_WIFI_HIF_IRQ_RUNTIME,
	};
	struct mt6797_wifi_hif_terminal_race race = {
		.irq = &irq,
	};
	struct task_struct *publisher;
	struct task_struct *sender;
	unsigned long flags;

	spin_lock_init(&irq.terminal_lock);
	init_completion(&race.start);
	init_completion(&race.publisher_done);
	init_completion(&race.sender_done);
	publisher = kthread_run(mt6797_wifi_hif_terminal_publisher, &race,
				"mt6797-hif-terminal-publisher");
	KUNIT_ASSERT_FALSE(test, IS_ERR(publisher));
	sender = kthread_run(mt6797_wifi_hif_terminal_sender, &race,
			     "mt6797-hif-terminal-sender");
	if (IS_ERR(sender)) {
		complete_all(&race.start);
		kthread_stop(publisher);
		KUNIT_FAIL(test, "failed to start terminal sender");
		return;
	}

	/* Release both contenders while the observable pair is locked. */
	spin_lock_irqsave(&irq.terminal_lock, flags);
	complete_all(&race.start);
	spin_unlock_irqrestore(&irq.terminal_lock, flags);
	if (!wait_for_completion_timeout(&race.publisher_done, HZ) ||
	    !wait_for_completion_timeout(&race.sender_done, HZ)) {
		KUNIT_FAIL(test, "terminal publication race timed out");
		kthread_stop(publisher);
		kthread_stop(sender);
		return;
	}
	kthread_stop(publisher);
	kthread_stop(sender);
	KUNIT_EXPECT_TRUE(test, race.published);
	/* The sender may win first, but must never observe a reasonless terminal. */
	KUNIT_EXPECT_TRUE(test, race.sender_status == 0 ||
				 race.sender_status == -EUCLEAN);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_hif_runtime_status(&irq), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, READ_ONCE(irq.terminal_reason), -EUCLEAN);
	KUNIT_EXPECT_FALSE(test,
		mt6797_wifi_hif_runtime_publish_terminal(&irq, -EIO));
	KUNIT_EXPECT_EQ(test, mt6797_wifi_hif_runtime_status(&irq), -EUCLEAN);
}

static void mt6797_wifi_hif_scan_event_routing_test(struct kunit *test)
{
	u8 payload[4] = { 7 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.id = MT6797_WIFI_SCAN_DONE_EVENT_ID,
		.sequence = 99,
	};
	struct mt6797_wifi_scan_transaction transaction = {};

	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_route_event(&transaction, &event), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_IDLE);
	transaction.state = MT6797_WIFI_SCAN_WAIT_DONE;
	transaction.scan_sequence = 7;
	event.id = 0x27;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_route_event(&transaction, &event), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_WAIT_DONE);
	event.id = MT6797_WIFI_SCAN_DONE_EVENT_ID;
	payload[0] = 6;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_route_event(&transaction, &event), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_WAIT_DONE);
	payload[0] = 7;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_route_event(&transaction, &event), 0);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_DONE);
}

static void mt6797_wifi_hif_scan_event_error_test(struct kunit *test)
{
	u8 payload[3] = { 7 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.id = MT6797_WIFI_SCAN_DONE_EVENT_ID,
	};
	struct mt6797_wifi_scan_transaction transaction = {
		.state = MT6797_WIFI_SCAN_WAIT_DONE,
		.scan_sequence = 7,
	};

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_hif_runtime_route_event(&transaction, &event), -EPROTO);
	KUNIT_EXPECT_EQ(test, transaction.state,
			(enum mt6797_wifi_scan_state)MT6797_WIFI_SCAN_TERMINAL);
}

static void mt6797_wifi_hif_remove_after_fault_test(struct kunit *test)
{
	static const enum mt6797_wifi_hif_test_event expected[] = {
		TEST_TERMINAL, TEST_MASK, TEST_DEVICE_DISABLE,
		TEST_LINUX_DISABLE, TEST_MASK, TEST_RESTORE, TEST_FENCE,
		TEST_TEARDOWN, TEST_TERMINAL,
	};
	struct mt6797_wifi_hif_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
		.mode = MT6797_WIFI_HIF_IRQ_QUIESCENT,
	};
	struct mt6797_wifi_hif_runtime_transaction transaction;
	struct mt6797_wifi_hif_runtime_ops ops;
	int ret;

	mt6797_wifi_hif_test_init(&context, &ops, &transaction);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_hif_runtime_transaction_start(&ops, &transaction), 0);
	context.event_count = 0;
	KUNIT_ASSERT_TRUE(test,
			  mt6797_wifi_hif_runtime_mark_terminal(&context.mode));
	ret = mt6797_wifi_hif_runtime_transaction_stop(
		&ops, &transaction,
		context.mode == MT6797_WIFI_HIF_IRQ_TERMINAL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.terminal);
	mt6797_wifi_hif_test_expect(test, &context, expected,
				      ARRAY_SIZE(expected));
}

static struct kunit_case mt6797_wifi_hif_runtime_cases[] = {
	KUNIT_CASE(mt6797_wifi_hif_start_stop_test),
	KUNIT_CASE(mt6797_wifi_hif_program_failure_test),
	KUNIT_CASE(mt6797_wifi_hif_device_enable_failure_test),
	KUNIT_CASE(mt6797_wifi_hif_restore_failure_test),
	KUNIT_CASE(mt6797_wifi_hif_terminal_transition_test),
	KUNIT_CASE(mt6797_wifi_hif_terminal_reason_race_test),
	KUNIT_CASE(mt6797_wifi_hif_scan_event_routing_test),
	KUNIT_CASE(mt6797_wifi_hif_scan_event_error_test),
	KUNIT_CASE(mt6797_wifi_hif_remove_after_fault_test),
	{}
};

static struct kunit_suite mt6797_wifi_hif_runtime_suite = {
	.name = "mt6797-wifi-hif-runtime",
	.test_cases = mt6797_wifi_hif_runtime_cases,
};

kunit_test_suite(mt6797_wifi_hif_runtime_suite);

MODULE_LICENSE("GPL");
