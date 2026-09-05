// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "auxadc_thermal_internal.h"

#define MT6797_TEST_BANKS 6
#define MT6797_TEST_FALLIBLE_CALLS 31

enum mt6797_test_operation {
	MT6797_TEST_AUXADC_CLOCK_ON,
	MT6797_TEST_AUXADC_CLOCK_OFF,
	MT6797_TEST_THERMAL_CLOCK_ON,
	MT6797_TEST_THERMAL_CLOCK_OFF,
	MT6797_TEST_RESET,
	MT6797_TEST_ASSERT_RESET,
	MT6797_TEST_APMIXED,
	MT6797_TEST_RESTORE_APMIXED,
	MT6797_TEST_IDLE,
	MT6797_TEST_PAUSE_DISABLE,
	MT6797_TEST_CLEAR_CHANNEL,
	MT6797_TEST_DISABLE_CHANNEL,
	MT6797_TEST_PREPARE_BANK,
	MT6797_TEST_COMMIT_CHANNEL,
	MT6797_TEST_ENABLE_BANK,
	MT6797_TEST_RELEASE_BANK,
	MT6797_TEST_FIRST_SAMPLE,
};

struct mt6797_test_event {
	enum mt6797_test_operation operation;
	int bank;
};

struct mt6797_test_context {
	struct mt6797_test_event events[64];
	unsigned int event_count;
	int fallible_count;
	int fail_at;
};

static void
mt6797_test_record(struct mt6797_test_context *context,
		   enum mt6797_test_operation operation, int bank)
{
	context->events[context->event_count].operation = operation;
	context->events[context->event_count].bank = bank;
	context->event_count++;
}

static int
mt6797_test_fallible(struct mt6797_test_context *context,
		     enum mt6797_test_operation operation, int bank)
{
	int ordinal = context->fallible_count++;

	mt6797_test_record(context, operation, bank);
	return ordinal == context->fail_at ? -EIO : 0;
}

static int mt6797_test_auxadc_on(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_AUXADC_CLOCK_ON, -1);
}

static void mt6797_test_auxadc_off(void *data)
{
	mt6797_test_record(data, MT6797_TEST_AUXADC_CLOCK_OFF, -1);
}

static int mt6797_test_thermal_on(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_THERMAL_CLOCK_ON, -1);
}

static void mt6797_test_thermal_off(void *data)
{
	mt6797_test_record(data, MT6797_TEST_THERMAL_CLOCK_OFF, -1);
}

static int mt6797_test_reset(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_RESET, -1);
}

static void mt6797_test_assert_reset(void *data)
{
	mt6797_test_record(data, MT6797_TEST_ASSERT_RESET, -1);
}

static int mt6797_test_apmixed(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_APMIXED, -1);
}

static void mt6797_test_restore_apmixed(void *data)
{
	mt6797_test_record(data, MT6797_TEST_RESTORE_APMIXED, -1);
}

static int mt6797_test_idle(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_IDLE, -1);
}

static void mt6797_test_pause_disable(void *data)
{
	mt6797_test_record(data, MT6797_TEST_PAUSE_DISABLE, -1);
}

static int mt6797_test_clear_channel(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_CLEAR_CHANNEL, -1);
}

static void mt6797_test_disable_channel(void *data)
{
	mt6797_test_record(data, MT6797_TEST_DISABLE_CHANNEL, -1);
}

static int mt6797_test_commit_channel(void *data)
{
	return mt6797_test_fallible(data, MT6797_TEST_COMMIT_CHANNEL, -1);
}

static int mt6797_test_prepare_bank(void *data, unsigned int bank)
{
	return mt6797_test_fallible(data, MT6797_TEST_PREPARE_BANK, bank);
}

static int mt6797_test_enable_bank(void *data, unsigned int bank)
{
	return mt6797_test_fallible(data, MT6797_TEST_ENABLE_BANK, bank);
}

static int mt6797_test_release_bank(void *data, unsigned int bank)
{
	return mt6797_test_fallible(data, MT6797_TEST_RELEASE_BANK, bank);
}

static int mt6797_test_first_sample(void *data, unsigned int bank)
{
	return mt6797_test_fallible(data, MT6797_TEST_FIRST_SAMPLE, bank);
}

static const struct mtk_thermal_transaction_ops mt6797_test_ops = {
	.enable_auxadc_clock = mt6797_test_auxadc_on,
	.disable_auxadc_clock = mt6797_test_auxadc_off,
	.enable_thermal_clock = mt6797_test_thermal_on,
	.disable_thermal_clock = mt6797_test_thermal_off,
	.reset_thermal = mt6797_test_reset,
	.assert_thermal_reset = mt6797_test_assert_reset,
	.configure_apmixed = mt6797_test_apmixed,
	.restore_apmixed = mt6797_test_restore_apmixed,
	.wait_for_idle = mt6797_test_idle,
	.pause_disable_banks = mt6797_test_pause_disable,
	.clear_auxadc_channel = mt6797_test_clear_channel,
	.disable_auxadc_channel = mt6797_test_disable_channel,
	.prepare_bank = mt6797_test_prepare_bank,
	.commit_auxadc_channel = mt6797_test_commit_channel,
	.enable_bank = mt6797_test_enable_bank,
	.release_bank = mt6797_test_release_bank,
	.first_sample = mt6797_test_first_sample,
};

static void
mt6797_test_expect_event(struct kunit *test,
			 const struct mt6797_test_context *context,
			 unsigned int ordinal,
			 enum mt6797_test_operation operation, int bank)
{
	KUNIT_ASSERT_LT(test, ordinal, context->event_count);
	KUNIT_EXPECT_EQ(test, context->events[ordinal].operation, operation);
	KUNIT_EXPECT_EQ(test, context->events[ordinal].bank, bank);
}

static void mt6797_transaction_success_order(struct kunit *test)
{
	struct mtk_thermal_transaction_state state = {};
	struct mt6797_test_context context = { .fail_at = -1 };
	unsigned int ordinal = 0;
	unsigned int bank;
	int ret;

	ret = mtk_thermal_transaction_execute(&context, &mt6797_test_ops,
					      &state, MT6797_TEST_BANKS);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, state.ready);

	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_AUXADC_CLOCK_ON, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_THERMAL_CLOCK_ON, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_RESET, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_APMIXED, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_IDLE, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_PAUSE_DISABLE, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_CLEAR_CHANNEL, -1);
	for (bank = 0; bank < MT6797_TEST_BANKS; bank++)
		mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_PREPARE_BANK, bank);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_COMMIT_CHANNEL, -1);
	for (bank = 0; bank < MT6797_TEST_BANKS; bank++)
		mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_ENABLE_BANK, bank);
	for (bank = 0; bank < MT6797_TEST_BANKS; bank++)
		mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_RELEASE_BANK, bank);
	for (bank = 0; bank < MT6797_TEST_BANKS; bank++)
		mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_FIRST_SAMPLE, bank);
	KUNIT_EXPECT_EQ(test, context.event_count, ordinal);

	mtk_thermal_transaction_close(&context, &mt6797_test_ops, &state);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_PAUSE_DISABLE, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_DISABLE_CHANNEL, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_RESTORE_APMIXED, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_ASSERT_RESET, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_THERMAL_CLOCK_OFF, -1);
	mt6797_test_expect_event(test, &context, ordinal++, MT6797_TEST_AUXADC_CLOCK_OFF, -1);
	KUNIT_EXPECT_EQ(test, context.event_count, ordinal);
	KUNIT_EXPECT_TRUE(test, mtk_thermal_transaction_state_is_closed(&state));
}

static void mt6797_transaction_all_failures_close(struct kunit *test)
{
	int fail_at;

	for (fail_at = 0; fail_at < MT6797_TEST_FALLIBLE_CALLS;
	     fail_at++) {
		struct mtk_thermal_transaction_state state = {};
		struct mt6797_test_context context = { .fail_at = fail_at };
		bool closed;
		int ret;

		ret = mtk_thermal_transaction_execute(&context,
						      &mt6797_test_ops, &state,
						      MT6797_TEST_BANKS);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EIO, "failure %d", fail_at);
		closed = mtk_thermal_transaction_state_is_closed(&state);
		KUNIT_EXPECT_TRUE(test, closed);
	}
}

static void mt6797_transaction_rejects_invalid_start(struct kunit *test)
{
	struct mtk_thermal_transaction_state state = {};
	struct mt6797_test_context context = { .fail_at = -1 };
	int ret;

	ret = mtk_thermal_transaction_execute(&context, &mt6797_test_ops,
					      &state, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	state.ready = true;
	ret = mtk_thermal_transaction_execute(&context, &mt6797_test_ops,
					      &state, MT6797_TEST_BANKS);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_transaction_apmixed_mask(struct kunit *test)
{
	u32 original = 0xa5a5a5f5;
	u32 expected = original & ~GENMASK(5, 4);

	KUNIT_EXPECT_EQ(test, mtk_thermal_mt6797_apmixed_value(original),
			expected);
	KUNIT_EXPECT_EQ(test, expected & GENMASK(5, 4), 0U);
	KUNIT_EXPECT_EQ(test, expected & ~GENMASK(5, 4),
			original & ~GENMASK(5, 4));
}

static void mt6797_transaction_idle_predicates(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mtk_thermal_mt6797_ahb_idle(0x0000ffff));
	KUNIT_EXPECT_FALSE(test, mtk_thermal_mt6797_ahb_idle(0x00010000));
	KUNIT_EXPECT_TRUE(test, mtk_thermal_mt6797_auxadc_idle(0x2));
	KUNIT_EXPECT_FALSE(test, mtk_thermal_mt6797_auxadc_idle(0x1));
}

static void mt6797_transaction_first_sample_gate(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, mtk_thermal_mt6797_first_sample_valid(0,
								       25000));
	KUNIT_EXPECT_FALSE(test, mtk_thermal_mt6797_first_sample_valid(1,
								       -20001));
	KUNIT_EXPECT_FALSE(test, mtk_thermal_mt6797_first_sample_valid(1,
								       150001));
	KUNIT_EXPECT_TRUE(test, mtk_thermal_mt6797_first_sample_valid(1,
								      -20000));
	KUNIT_EXPECT_TRUE(test, mtk_thermal_mt6797_first_sample_valid(0xfff,
								      150000));
}

static struct kunit_case mt6797_thermal_transaction_cases[] = {
	KUNIT_CASE(mt6797_transaction_success_order),
	KUNIT_CASE(mt6797_transaction_all_failures_close),
	KUNIT_CASE(mt6797_transaction_rejects_invalid_start),
	KUNIT_CASE(mt6797_transaction_apmixed_mask),
	KUNIT_CASE(mt6797_transaction_idle_predicates),
	KUNIT_CASE(mt6797_transaction_first_sample_gate),
	{}
};

static struct kunit_suite mt6797_thermal_transaction_suite = {
	.name = "mt6797-thermal-transaction",
	.test_cases = mt6797_thermal_transaction_cases,
};

kunit_test_suite(mt6797_thermal_transaction_suite);

MODULE_LICENSE("GPL");
