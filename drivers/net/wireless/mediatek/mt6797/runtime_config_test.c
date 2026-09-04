// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "runtime_config.h"

enum mt6797_wifi_config_test_event {
	TEST_PRECHECK,
	TEST_SEND,
	TEST_TEARDOWN,
	TEST_FENCE,
};

struct mt6797_wifi_config_test_context {
	struct kunit *test;
	struct mt6797_wifi_config_transaction *transaction;
	enum mt6797_wifi_config_test_event events[4];
	unsigned int event_count;
	int precheck_result;
	int send_result;
	int teardown_result;
};

static int
mt6797_wifi_config_test_record(struct mt6797_wifi_config_test_context *context,
			       enum mt6797_wifi_config_test_event event)
{
	KUNIT_ASSERT_LT(context->test, context->event_count,
			(unsigned int)ARRAY_SIZE(context->events));
	context->events[context->event_count++] = event;

	return 0;
}

static int mt6797_wifi_config_test_precheck(void *data)
{
	struct mt6797_wifi_config_test_context *context = data;

	KUNIT_EXPECT_FALSE(context->test, context->transaction->dirty);
	mt6797_wifi_config_test_record(context, TEST_PRECHECK);
	return context->precheck_result;
}

static int mt6797_wifi_config_test_send(void *data)
{
	struct mt6797_wifi_config_test_context *context = data;

	KUNIT_EXPECT_TRUE(context->test, context->transaction->dirty);
	mt6797_wifi_config_test_record(context, TEST_SEND);
	return context->send_result;
}

static int mt6797_wifi_config_test_teardown(void *data)
{
	struct mt6797_wifi_config_test_context *context = data;

	mt6797_wifi_config_test_record(context, TEST_TEARDOWN);
	return context->teardown_result;
}

static void mt6797_wifi_config_test_fence(void *data)
{
	mt6797_wifi_config_test_record(data, TEST_FENCE);
}

static void
mt6797_wifi_config_test_init(struct mt6797_wifi_config_test_context *context,
			     struct mt6797_wifi_config_transaction *transaction,
			     struct mt6797_wifi_config_ops *ops)
{
	*transaction = (struct mt6797_wifi_config_transaction) {};
	context->transaction = transaction;
	*ops = (struct mt6797_wifi_config_ops) {
		.precheck = mt6797_wifi_config_test_precheck,
		.send = mt6797_wifi_config_test_send,
		.teardown = mt6797_wifi_config_test_teardown,
		.fence = mt6797_wifi_config_test_fence,
		.context = context,
	};
}

static void
mt6797_wifi_config_test_expect(struct kunit *test,
			       struct mt6797_wifi_config_test_context *context,
			       const enum mt6797_wifi_config_test_event *expected,
			       size_t count)
{
	unsigned int index;

	KUNIT_ASSERT_EQ(test, context->event_count, (unsigned int)count);
	for (index = 0; index < count; index++)
		KUNIT_EXPECT_EQ(test, context->events[index], expected[index]);
}

static void mt6797_wifi_config_success_test(struct kunit *test)
{
	static const enum mt6797_wifi_config_test_event expected[] = {
		TEST_PRECHECK, TEST_SEND,
	};
	struct mt6797_wifi_config_test_context context = { .test = test };
	struct mt6797_wifi_config_transaction transaction;
	struct mt6797_wifi_config_ops ops;
	int ret;

	mt6797_wifi_config_test_init(&context, &transaction, &ops);
	ret = mt6797_wifi_config_transaction_run(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, transaction.dirty);
	KUNIT_EXPECT_TRUE(test, transaction.configured);
	mt6797_wifi_config_test_expect(test, &context, expected,
				       ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test,
			mt6797_wifi_config_transaction_run(&ops, &transaction),
			-EALREADY);
}

static void mt6797_wifi_config_precheck_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_config_test_event expected[] = {
		TEST_PRECHECK, TEST_TEARDOWN,
	};
	struct mt6797_wifi_config_test_context context = {
		.test = test,
		.precheck_result = -EIO,
	};
	struct mt6797_wifi_config_transaction transaction;
	struct mt6797_wifi_config_ops ops;
	int ret;

	mt6797_wifi_config_test_init(&context, &transaction, &ops);
	ret = mt6797_wifi_config_transaction_run(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_FALSE(test, transaction.dirty);
	KUNIT_EXPECT_FALSE(test, transaction.configured);
	mt6797_wifi_config_test_expect(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_config_send_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_config_test_event expected[] = {
		TEST_PRECHECK, TEST_SEND, TEST_FENCE, TEST_TEARDOWN,
	};
	struct mt6797_wifi_config_test_context context = {
		.test = test,
		.send_result = -EIO,
	};
	struct mt6797_wifi_config_transaction transaction;
	struct mt6797_wifi_config_ops ops;
	int ret;

	mt6797_wifi_config_test_init(&context, &transaction, &ops);
	ret = mt6797_wifi_config_transaction_run(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_TRUE(test, transaction.dirty);
	KUNIT_EXPECT_FALSE(test, transaction.configured);
	mt6797_wifi_config_test_expect(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static void mt6797_wifi_config_unclean_precheck_test(struct kunit *test)
{
	static const enum mt6797_wifi_config_test_event expected[] = {
		TEST_PRECHECK, TEST_FENCE, TEST_TEARDOWN,
	};
	struct mt6797_wifi_config_test_context context = {
		.test = test,
		.precheck_result = -EUCLEAN,
	};
	struct mt6797_wifi_config_transaction transaction;
	struct mt6797_wifi_config_ops ops;
	int ret;

	mt6797_wifi_config_test_init(&context, &transaction, &ops);
	ret = mt6797_wifi_config_transaction_run(&ops, &transaction);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	KUNIT_EXPECT_FALSE(test, transaction.dirty);
	KUNIT_EXPECT_FALSE(test, transaction.configured);
	mt6797_wifi_config_test_expect(test, &context, expected,
				       ARRAY_SIZE(expected));
}

static struct kunit_case mt6797_wifi_config_cases[] = {
	KUNIT_CASE(mt6797_wifi_config_success_test),
	KUNIT_CASE(mt6797_wifi_config_precheck_failure_test),
	KUNIT_CASE(mt6797_wifi_config_send_failure_test),
	KUNIT_CASE(mt6797_wifi_config_unclean_precheck_test),
	{}
};

static struct kunit_suite mt6797_wifi_config_suite = {
	.name = "mt6797-wifi-runtime-config",
	.test_cases = mt6797_wifi_config_cases,
};

kunit_test_suite(mt6797_wifi_config_suite);

MODULE_LICENSE("GPL");
