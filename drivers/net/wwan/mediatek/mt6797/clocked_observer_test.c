// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-independent failure tests for MT6797 MD1 clock observation. */

#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/errno.h>

#include "clocked_observer_core.h"

struct mt6797_md1_test_context {
	struct mt6797_md1_snapshot snapshots[2];
	unsigned int enabled;
	unsigned int disable_order[MT6797_MD1_OBSERVER_CLOCKS];
	unsigned int enable_calls;
	unsigned int disable_calls;
	unsigned int snapshot_calls;
	unsigned int verify_calls;
	int fail_enable;
	int fail_snapshot;
	int verify_ret;
};

static void mt6797_md1_clocked_test_init(struct mt6797_md1_test_context *context)
{
	memset(context, 0, sizeof(*context));
	context->fail_enable = -1;
	context->fail_snapshot = -1;
}

static int mt6797_md1_clocked_test_enable(void *data, unsigned int index)
{
	struct mt6797_md1_test_context *context = data;

	context->enable_calls++;
	if (index == context->fail_enable)
		return -EIO;
	context->enabled |= BIT(index);
	return 0;
}

static void mt6797_md1_clocked_test_disable(void *data, unsigned int index)
{
	struct mt6797_md1_test_context *context = data;

	context->disable_order[context->disable_calls++] = index;
	context->enabled &= ~BIT(index);
}

static int mt6797_md1_clocked_test_snapshot(void *data, unsigned int index,
					    struct mt6797_md1_snapshot *snapshot)
{
	struct mt6797_md1_test_context *context = data;

	context->snapshot_calls++;
	if (index == context->fail_snapshot)
		return -EIO;
	*snapshot = context->snapshots[index];
	return 0;
}

static int mt6797_md1_clocked_test_verify(void *data)
{
	struct mt6797_md1_test_context *context = data;

	context->verify_calls++;
	if (context->verify_ret)
		return context->verify_ret;
	return context->enabled ? -EBUSY : 0;
}

static const struct mt6797_md1_clocked_ops mt6797_md1_clocked_test_ops = {
	.enable_clock = mt6797_md1_clocked_test_enable,
	.disable_clock = mt6797_md1_clocked_test_disable,
	.take_snapshot = mt6797_md1_clocked_test_snapshot,
	.verify_unwind = mt6797_md1_clocked_test_verify,
};

static int test_observe(struct mt6797_md1_test_context *context,
			enum mt6797_md1_inherited_state *state)
{
	return mt6797_md1_clocked_observe(&mt6797_md1_clocked_test_ops,
					  context, state);
}

static void mt6797_md1_clocked_invalid_arguments_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	struct mt6797_md1_clocked_ops ops;
	enum mt6797_md1_inherited_state state;
	int ret;

	mt6797_md1_clocked_test_init(&context);
	state = MT6797_MD1_INHERITED_POWER_OFF;
	ret = mt6797_md1_clocked_observe(NULL, &context, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = test_observe(NULL, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = test_observe(&context, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ops = mt6797_md1_clocked_test_ops;
	ops.enable_clock = NULL;
	ret = mt6797_md1_clocked_observe(&ops, &context, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ops = mt6797_md1_clocked_test_ops;
	ops.disable_clock = NULL;
	ret = mt6797_md1_clocked_observe(&ops, &context, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ops = mt6797_md1_clocked_test_ops;
	ops.take_snapshot = NULL;
	ret = mt6797_md1_clocked_observe(&ops, &context, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ops = mt6797_md1_clocked_test_ops;
	ops.verify_unwind = NULL;
	ret = mt6797_md1_clocked_observe(&ops, &context, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_POWER_OFF);
	KUNIT_EXPECT_EQ(test, context.enable_calls, 0U);
	KUNIT_EXPECT_EQ(test, context.disable_calls, 0U);
	KUNIT_EXPECT_EQ(test, context.snapshot_calls, 0U);
	KUNIT_EXPECT_EQ(test, context.verify_calls, 0U);
}

static void mt6797_md1_clocked_expect_unwind(struct kunit *test,
					     struct mt6797_md1_test_context *context,
					     unsigned int count)
{
	unsigned int i;

	KUNIT_EXPECT_EQ(test, context->enabled, 0U);
	KUNIT_EXPECT_EQ(test, context->disable_calls, count);
	KUNIT_EXPECT_EQ(test, context->verify_calls, 1U);
	for (i = 0; i < count; i++)
		KUNIT_EXPECT_EQ(test, context->disable_order[i], count - i - 1);
}

static void mt6797_md1_clocked_enable_failure_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	enum mt6797_md1_inherited_state state;
	unsigned int failure;
	int ret;

	for (failure = 0; failure < MT6797_MD1_OBSERVER_CLOCKS; failure++) {
		mt6797_md1_clocked_test_init(&context);
		context.fail_enable = failure;
		state = MT6797_MD1_INHERITED_POWER_OFF;

		ret = test_observe(&context, &state);

		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test, context.enable_calls, failure + 1);
		KUNIT_EXPECT_EQ(test, context.snapshot_calls, 0U);
		KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_POWER_OFF);
		mt6797_md1_clocked_expect_unwind(test, &context, failure);
	}
}

static void mt6797_md1_clocked_snapshot_failure_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	enum mt6797_md1_inherited_state state;
	unsigned int failure;
	int ret;

	for (failure = 0; failure < 2; failure++) {
		mt6797_md1_clocked_test_init(&context);
		context.fail_snapshot = failure;
		state = MT6797_MD1_INHERITED_POWER_OFF;

		ret = test_observe(&context, &state);

		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test, context.enable_calls,
				MT6797_MD1_OBSERVER_CLOCKS);
		KUNIT_EXPECT_EQ(test, context.snapshot_calls, failure + 1);
		KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_POWER_OFF);
		mt6797_md1_clocked_expect_unwind(test, &context,
						 MT6797_MD1_OBSERVER_CLOCKS);
	}
}

static void mt6797_md1_clocked_unwind_failure_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	enum mt6797_md1_inherited_state state;
	int ret;

	mt6797_md1_clocked_test_init(&context);
	context.verify_ret = -ETIMEDOUT;
	state = MT6797_MD1_INHERITED_POWER_OFF;

	ret = test_observe(&context, &state);

	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_POWER_OFF);
	mt6797_md1_clocked_expect_unwind(test, &context,
					 MT6797_MD1_OBSERVER_CLOCKS);
}

static void mt6797_md1_clocked_unstable_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	enum mt6797_md1_inherited_state state;
	int ret;

	mt6797_md1_clocked_test_init(&context);
	context.snapshots[1].tx_start[0] = 1;
	state = MT6797_MD1_INHERITED_POWER_OFF;

	ret = test_observe(&context, &state);

	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_POWER_OFF);
	mt6797_md1_clocked_expect_unwind(test, &context,
					 MT6797_MD1_OBSERVER_CLOCKS);
}

static void mt6797_md1_clocked_classification_test(struct kunit *test)
{
	struct mt6797_md1_test_context context;
	enum mt6797_md1_inherited_state state;
	int ret;

	mt6797_md1_clocked_test_init(&context);
	ret = test_observe(&context, &state);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_QUIESCENT);
	mt6797_md1_clocked_expect_unwind(test, &context,
					 MT6797_MD1_OBSERVER_CLOCKS);

	mt6797_md1_clocked_test_init(&context);
	context.snapshots[0].ccif_pending = 1;
	context.snapshots[1].ccif_pending = 1;
	ret = test_observe(&context, &state);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_CCIF_PENDING);
	mt6797_md1_clocked_expect_unwind(test, &context,
					 MT6797_MD1_OBSERVER_CLOCKS);

	mt6797_md1_clocked_test_init(&context);
	context.snapshots[0].rx_start[7] = 0x1234;
	context.snapshots[1].rx_start[7] = 0x1234;
	ret = test_observe(&context, &state);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, MT6797_MD1_INHERITED_FOREIGN_RINGS);
	mt6797_md1_clocked_expect_unwind(test, &context,
					 MT6797_MD1_OBSERVER_CLOCKS);
}

static struct kunit_case mt6797_md1_clocked_observer_cases[] = {
	KUNIT_CASE(mt6797_md1_clocked_invalid_arguments_test),
	KUNIT_CASE(mt6797_md1_clocked_enable_failure_test),
	KUNIT_CASE(mt6797_md1_clocked_snapshot_failure_test),
	KUNIT_CASE(mt6797_md1_clocked_unwind_failure_test),
	KUNIT_CASE(mt6797_md1_clocked_unstable_test),
	KUNIT_CASE(mt6797_md1_clocked_classification_test),
	{}
};

static struct kunit_suite mt6797_md1_clocked_observer_suite = {
	.name = "mt6797-md1-clocked-observer",
	.test_cases = mt6797_md1_clocked_observer_cases,
};

kunit_test_suite(mt6797_md1_clocked_observer_suite);

MODULE_LICENSE("GPL");
