// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "boot.h"

enum mt6797_wifi_test_event {
	TEST_ENABLE,
	TEST_BLOCK,
	TEST_OWN,
	TEST_INTERRUPTS,
	TEST_CONFIG0,
	TEST_IDLM0,
	TEST_CONFIG1,
	TEST_IDLM1,
	TEST_EMI,
	TEST_START,
	TEST_READY,
	TEST_STATUS,
	TEST_RESTORE_BLOCK,
	TEST_RESTORE_FUNCTION,
	TEST_FENCE,
};

struct mt6797_wifi_test_context {
	struct kunit *test;
	enum mt6797_wifi_test_event event[24];
	enum mt6797_wifi_test_event fail_at;
	unsigned int event_count;
};

static int
mt6797_wifi_test_record(struct mt6797_wifi_test_context *context,
			enum mt6797_wifi_test_event event)
{
	KUNIT_ASSERT_LT(context->test, context->event_count,
			(unsigned int)ARRAY_SIZE(context->event));
	context->event[context->event_count++] = event;
	return context->fail_at == event ? -EIO : 0;
}

static int mt6797_wifi_test_enable(void *data, bool *touched)
{
	struct mt6797_wifi_test_context *context = data;

	*touched = true;
	return mt6797_wifi_test_record(context, TEST_ENABLE);
}

static int mt6797_wifi_test_unclean_enable(void *data, bool *touched)
{
	struct mt6797_wifi_test_context *context = data;

	*touched = false;
	mt6797_wifi_test_record(context, TEST_ENABLE);
	return -EUCLEAN;
}

static int mt6797_wifi_test_block(void *data, bool *touched)
{
	struct mt6797_wifi_test_context *context = data;

	*touched = true;
	return mt6797_wifi_test_record(context, TEST_BLOCK);
}

#define MT6797_WIFI_TEST_CALLBACK(_name, _event) \
	static int mt6797_wifi_test_##_name(void *data) \
	{ \
		return mt6797_wifi_test_record(data, _event); \
	}

MT6797_WIFI_TEST_CALLBACK(own, TEST_OWN)
MT6797_WIFI_TEST_CALLBACK(interrupts, TEST_INTERRUPTS)
MT6797_WIFI_TEST_CALLBACK(emi, TEST_EMI)
MT6797_WIFI_TEST_CALLBACK(ready, TEST_READY)
MT6797_WIFI_TEST_CALLBACK(status, TEST_STATUS)
MT6797_WIFI_TEST_CALLBACK(restore_block, TEST_RESTORE_BLOCK)
MT6797_WIFI_TEST_CALLBACK(restore_function, TEST_RESTORE_FUNCTION)

static int mt6797_wifi_test_config(void *data, unsigned int section,
				   bool reset, u8 sequence)
{
	struct mt6797_wifi_test_context *context = data;

	KUNIT_EXPECT_LT(context->test, section, 2U);
	KUNIT_EXPECT_EQ(context->test, reset, !section);
	KUNIT_EXPECT_EQ(context->test, sequence, (u8)(section + 1));
	return mt6797_wifi_test_record(context,
			section ? TEST_CONFIG1 : TEST_CONFIG0);
}

static int mt6797_wifi_test_idlm(void *data, unsigned int section)
{
	struct mt6797_wifi_test_context *context = data;

	KUNIT_EXPECT_LT(context->test, section, 2U);
	return mt6797_wifi_test_record(context,
			section ? TEST_IDLM1 : TEST_IDLM0);
}

static int mt6797_wifi_test_start(void *data, u8 sequence)
{
	struct mt6797_wifi_test_context *context = data;

	KUNIT_EXPECT_EQ(context->test, sequence, (u8)3);
	return mt6797_wifi_test_record(context, TEST_START);
}

static void mt6797_wifi_test_fence(void *data)
{
	struct mt6797_wifi_test_context *context = data;

	mt6797_wifi_test_record(context, TEST_FENCE);
}

static void mt6797_wifi_test_init_ops(struct mt6797_wifi_test_context *context,
				      struct mt6797_wifi_boot_ops *ops)
{
	*ops = (struct mt6797_wifi_boot_ops) {
		.enable_function = mt6797_wifi_test_enable,
		.set_block_size = mt6797_wifi_test_block,
		.driver_own = mt6797_wifi_test_own,
		.disable_interrupts = mt6797_wifi_test_interrupts,
		.download_config = mt6797_wifi_test_config,
		.download_idlm = mt6797_wifi_test_idlm,
		.download_emi = mt6797_wifi_test_emi,
		.start = mt6797_wifi_test_start,
		.wait_ready = mt6797_wifi_test_ready,
		.clear_status = mt6797_wifi_test_status,
		.restore_block_size = mt6797_wifi_test_restore_block,
		.restore_function = mt6797_wifi_test_restore_function,
		.fence = mt6797_wifi_test_fence,
		.context = context,
	};
}

static void mt6797_wifi_test_expect_events(struct kunit *test,
					   struct mt6797_wifi_test_context *context,
					   const enum mt6797_wifi_test_event *expected,
					   size_t count)
{
	unsigned int index;

	KUNIT_ASSERT_EQ(test, context->event_count, (unsigned int)count);
	for (index = 0; index < count; index++)
		KUNIT_EXPECT_EQ(test, context->event[index], expected[index]);
}

static void mt6797_wifi_boot_success_test(struct kunit *test)
{
	static const enum mt6797_wifi_test_event expected[] = {
		TEST_ENABLE, TEST_BLOCK, TEST_OWN, TEST_INTERRUPTS,
		TEST_CONFIG0, TEST_IDLM0, TEST_CONFIG1, TEST_IDLM1,
		TEST_EMI, TEST_START, TEST_READY, TEST_STATUS,
		TEST_RESTORE_BLOCK, TEST_RESTORE_FUNCTION,
	};
	struct mt6797_wifi_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_boot_transaction tx;
	struct mt6797_wifi_boot_ops ops;
	int ret;

	mt6797_wifi_test_init_ops(&context, &ops);
	ret = mt6797_wifi_boot_transaction_run(&ops, &tx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, tx.ready);
	KUNIT_EXPECT_TRUE(test, tx.firmware_dirty);
	ret = mt6797_wifi_boot_transaction_stop(&ops, &tx);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, tx.ready);
	mt6797_wifi_test_expect_events(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_boot_predirty_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_test_event expected[] = {
		TEST_ENABLE, TEST_BLOCK, TEST_OWN,
		TEST_RESTORE_BLOCK, TEST_RESTORE_FUNCTION,
	};
	struct mt6797_wifi_test_context context = {
		.test = test,
		.fail_at = TEST_OWN,
	};
	struct mt6797_wifi_boot_transaction tx;
	struct mt6797_wifi_boot_ops ops;
	int ret;

	mt6797_wifi_test_init_ops(&context, &ops);
	ret = mt6797_wifi_boot_transaction_run(&ops, &tx);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	mt6797_wifi_test_expect_events(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_boot_dirty_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_test_event expected[] = {
		TEST_ENABLE, TEST_BLOCK, TEST_OWN, TEST_INTERRUPTS,
		TEST_CONFIG0, TEST_IDLM0, TEST_CONFIG1, TEST_IDLM1, TEST_EMI,
		TEST_RESTORE_BLOCK, TEST_RESTORE_FUNCTION, TEST_FENCE,
	};
	struct mt6797_wifi_test_context context = {
		.test = test,
		.fail_at = TEST_EMI,
	};
	struct mt6797_wifi_boot_transaction tx;
	struct mt6797_wifi_boot_ops ops;
	int ret;

	mt6797_wifi_test_init_ops(&context, &ops);
	ret = mt6797_wifi_boot_transaction_run(&ops, &tx);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	mt6797_wifi_test_expect_events(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_boot_unclean_input_test(struct kunit *test)
{
	static const enum mt6797_wifi_test_event expected[] = {
		TEST_ENABLE, TEST_FENCE,
	};
	struct mt6797_wifi_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_boot_transaction tx;
	struct mt6797_wifi_boot_ops ops;
	int ret;

	mt6797_wifi_test_init_ops(&context, &ops);
	ops.enable_function = mt6797_wifi_test_unclean_enable;
	ret = mt6797_wifi_boot_transaction_run(&ops, &tx);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	mt6797_wifi_test_expect_events(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_boot_cleanup_failure_test(struct kunit *test)
{
	struct mt6797_wifi_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_boot_transaction tx;
	struct mt6797_wifi_boot_ops ops;
	int ret;

	mt6797_wifi_test_init_ops(&context, &ops);
	ret = mt6797_wifi_boot_transaction_run(&ops, &tx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	context.fail_at = TEST_RESTORE_BLOCK;
	ret = mt6797_wifi_boot_transaction_stop(&ops, &tx);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_ASSERT_GE(test, context.event_count, 3U);
	KUNIT_EXPECT_EQ(test, context.event[context.event_count - 3],
			TEST_RESTORE_BLOCK);
	KUNIT_EXPECT_EQ(test, context.event[context.event_count - 2],
			TEST_RESTORE_FUNCTION);
	KUNIT_EXPECT_EQ(test, context.event[context.event_count - 1], TEST_FENCE);
}

static struct kunit_case mt6797_wifi_boot_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_boot_success_test),
	KUNIT_CASE(mt6797_wifi_boot_predirty_failure_test),
	KUNIT_CASE(mt6797_wifi_boot_dirty_failure_test),
	KUNIT_CASE(mt6797_wifi_boot_unclean_input_test),
	KUNIT_CASE(mt6797_wifi_boot_cleanup_failure_test),
	{ }
};

static struct kunit_suite mt6797_wifi_boot_test_suite = {
	.name = "mt6797-wifi-boot",
	.test_cases = mt6797_wifi_boot_test_cases,
};

kunit_test_suite(mt6797_wifi_boot_test_suite);

MODULE_LICENSE("GPL");
