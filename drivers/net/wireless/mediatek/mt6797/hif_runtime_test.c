// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

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

static void mt6797_wifi_hif_unexpected_irq_test(struct kunit *test)
{
	int mode = MT6797_WIFI_HIF_IRQ_RUNTIME;

	KUNIT_EXPECT_TRUE(test, mt6797_wifi_hif_runtime_irq_terminal(&mode));
	KUNIT_EXPECT_EQ(test, mode, (int)MT6797_WIFI_HIF_IRQ_TERMINAL);
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_hif_runtime_irq_terminal(&mode));
	mode = MT6797_WIFI_HIF_IRQ_CAPABILITY;
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_hif_runtime_irq_terminal(&mode));
	KUNIT_EXPECT_EQ(test, mode, (int)MT6797_WIFI_HIF_IRQ_CAPABILITY);
}

static void mt6797_wifi_hif_remove_after_irq_test(struct kunit *test)
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
			  mt6797_wifi_hif_runtime_irq_terminal(&context.mode));
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
	KUNIT_CASE(mt6797_wifi_hif_unexpected_irq_test),
	KUNIT_CASE(mt6797_wifi_hif_remove_after_irq_test),
	{}
};

static struct kunit_suite mt6797_wifi_hif_runtime_suite = {
	.name = "mt6797-wifi-hif-runtime",
	.test_cases = mt6797_wifi_hif_runtime_cases,
};

kunit_test_suite(mt6797_wifi_hif_runtime_suite);

MODULE_LICENSE("GPL");
