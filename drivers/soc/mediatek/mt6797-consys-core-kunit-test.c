// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 CONSYS lifecycle core. */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <kunit/test.h>

enum mt6797_consys_test_op {
	TEST_OP_NONE,
	TEST_ENABLE_SUPPLIES,
	TEST_DISABLE_SUPPLIES,
	TEST_ENABLE_CLOCK,
	TEST_DISABLE_CLOCK,
	TEST_POWER_ON,
	TEST_POWER_OFF,
	TEST_RELEASE_RESET,
	TEST_ASSERT_RESET,
	TEST_POLL_POWER_ON,
	TEST_POLL_MCU_READY,
	TEST_POLL_POWER_OFF,
};

struct mt6797_consys_test_backend {
	enum mt6797_consys_test_op calls[32];
	u32 call_count;
	u32 pending[3];
	u32 delay_count;
	int fail_op;
};

static int mt6797_consys_test_record(
	struct mt6797_consys_test_backend *backend,
	enum mt6797_consys_test_op op)
{
	backend->calls[backend->call_count++] = op;
	return backend->fail_op == op ? -EIO : 0;
}

static int mt6797_consys_test_enable_supplies(void *context)
{
	return mt6797_consys_test_record(context, TEST_ENABLE_SUPPLIES);
}

static int mt6797_consys_test_disable_supplies(void *context)
{
	return mt6797_consys_test_record(context, TEST_DISABLE_SUPPLIES);
}

static int mt6797_consys_test_enable_clock(void *context)
{
	return mt6797_consys_test_record(context, TEST_ENABLE_CLOCK);
}

static int mt6797_consys_test_disable_clock(void *context)
{
	return mt6797_consys_test_record(context, TEST_DISABLE_CLOCK);
}

static int mt6797_consys_test_power_on(void *context)
{
	return mt6797_consys_test_record(context, TEST_POWER_ON);
}

static int mt6797_consys_test_power_off(void *context)
{
	return mt6797_consys_test_record(context, TEST_POWER_OFF);
}

static int mt6797_consys_test_release_reset(void *context)
{
	return mt6797_consys_test_record(context, TEST_RELEASE_RESET);
}

static int mt6797_consys_test_assert_reset(void *context)
{
	return mt6797_consys_test_record(context, TEST_ASSERT_RESET);
}

static int mt6797_consys_test_poll(void *context,
				   enum mt6797_consys_poll poll)
{
	struct mt6797_consys_test_backend *backend = context;
	enum mt6797_consys_test_op op = TEST_POLL_POWER_ON + poll;

	mt6797_consys_test_record(backend, op);
	if (backend->fail_op == op)
		return -EIO;
	if (backend->pending[poll]) {
		backend->pending[poll]--;
		return -EAGAIN;
	}
	return 0;
}

static void mt6797_consys_test_delay(void *context, u32 min, u32 max)
{
	struct mt6797_consys_test_backend *backend = context;

	backend->delay_count++;
}

static const struct mt6797_consys_ops mt6797_consys_test_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_supplies = mt6797_consys_test_enable_supplies,
	.disable_supplies = mt6797_consys_test_disable_supplies,
	.enable_clock = mt6797_consys_test_enable_clock,
	.disable_clock = mt6797_consys_test_disable_clock,
	.power_on = mt6797_consys_test_power_on,
	.power_off = mt6797_consys_test_power_off,
	.release_reset = mt6797_consys_test_release_reset,
	.assert_reset = mt6797_consys_test_assert_reset,
	.poll = mt6797_consys_test_poll,
	.delay_us = mt6797_consys_test_delay,
};

static const struct mt6797_consys_timeouts mt6797_consys_test_timeouts = {
	.poll_interval_us = 1,
	.power_on_timeout_us = 3,
	.mcu_ready_timeout_us = 3,
	.power_off_timeout_us = 3,
};

static void mt6797_consys_test_init(struct kunit *test,
				    struct mt6797_consys_lifecycle *lifecycle,
				    struct mt6797_consys_test_backend *backend)
{
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_lifecycle_init(lifecycle,
				&mt6797_consys_test_ops, backend,
				&mt6797_consys_test_timeouts), 0);
}

static void mt6797_consys_test_expect_calls(
	struct kunit *test, struct mt6797_consys_test_backend *backend,
	const enum mt6797_consys_test_op *expected, u32 expected_count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->call_count, expected_count);
	for (i = 0; i < expected_count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_consys_test_reference_counting(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_SUPPLIES, TEST_ENABLE_CLOCK, TEST_POWER_ON,
		TEST_POLL_POWER_ON, TEST_RELEASE_RESET, TEST_POLL_MCU_READY,
		TEST_ASSERT_RESET, TEST_POWER_OFF, TEST_POLL_POWER_OFF,
		TEST_DISABLE_CLOCK, TEST_DISABLE_SUPPLIES,
	};
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_get(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_get(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 2U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_put(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_put(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_unwinds_in_reverse_order(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_SUPPLIES, TEST_ENABLE_CLOCK, TEST_POWER_ON,
		TEST_POLL_POWER_ON, TEST_RELEASE_RESET, TEST_ASSERT_RESET,
		TEST_POWER_OFF, TEST_POLL_POWER_OFF, TEST_DISABLE_CLOCK,
		TEST_DISABLE_SUPPLIES,
	};
	struct mt6797_consys_test_backend backend = {
		.fail_op = TEST_RELEASE_RESET,
	};
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_EXPECT_EQ(test, mt6797_consys_get(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 0U);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_times_out_with_bounded_polls(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_SUPPLIES, TEST_ENABLE_CLOCK, TEST_POWER_ON,
		TEST_POLL_POWER_ON, TEST_POLL_POWER_ON, TEST_POLL_POWER_ON,
		TEST_POWER_OFF, TEST_POLL_POWER_OFF, TEST_DISABLE_CLOCK,
		TEST_DISABLE_SUPPLIES,
	};
	struct mt6797_consys_test_backend backend = {
		.pending[MT6797_CONSYS_POLL_POWER_ON] = 3,
	};
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_EXPECT_EQ(test, mt6797_consys_get(&lifecycle), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, backend.delay_count, 2U);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_cleanup_failure_is_sticky(struct kunit *test)
{
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_get(&lifecycle), 0);
	backend.fail_op = TEST_ASSERT_RESET;
	KUNIT_EXPECT_EQ(test, mt6797_consys_put(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_ERROR);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 0U);
	KUNIT_EXPECT_EQ(test, mt6797_consys_get(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_put(&lifecycle), -EIO);
}

static void mt6797_consys_test_rejects_invalid_contract(struct kunit *test)
{
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_ops ops = mt6797_consys_test_ops;
	struct mt6797_consys_timeouts timeouts = mt6797_consys_test_timeouts;

	ops.abi++;
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_lifecycle_init(&lifecycle, &ops, NULL,
				&timeouts), -EINVAL);
	ops = mt6797_consys_test_ops;
	timeouts.power_off_timeout_us = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_lifecycle_init(&lifecycle, &ops, NULL,
				&timeouts), -EINVAL);
}

static struct kunit_case mt6797_consys_core_test_cases[] = {
	KUNIT_CASE(mt6797_consys_test_reference_counting),
	KUNIT_CASE(mt6797_consys_test_unwinds_in_reverse_order),
	KUNIT_CASE(mt6797_consys_test_times_out_with_bounded_polls),
	KUNIT_CASE(mt6797_consys_test_cleanup_failure_is_sticky),
	KUNIT_CASE(mt6797_consys_test_rejects_invalid_contract),
	{ }
};

static struct kunit_suite mt6797_consys_core_test_suite = {
	.name = "mt6797-consys-core",
	.test_cases = mt6797_consys_core_test_cases,
};

kunit_test_suite(mt6797_consys_core_test_suite);

MODULE_LICENSE("GPL");
