// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free ordering and fatal-latch tests for the MT6797 MD1 cold owner. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>

enum mt6797_md1_cold_owner_test_op {
	MD1_COLD_TEST_COLD,
	MD1_COLD_TEST_CCIF_BEFORE,
	MD1_COLD_TEST_CCIF_CLEAR,
	MD1_COLD_TEST_CCIF_AFTER,
	MD1_COLD_TEST_CCIF_VERIFY,
	MD1_COLD_TEST_POWER,
	MD1_COLD_TEST_SRC_CLK,
	MD1_COLD_TEST_PLL_START,
	MD1_COLD_TEST_PLL_POLL,
};

struct mt6797_md1_cold_owner_test_backend {
	enum mt6797_md1_cold_owner_test_op calls[128];
	u32 count;
	u32 pll_pending;
	bool fail;
	enum mt6797_md1_cold_owner_test_op fail_op;
};

static int mt6797_md1_cold_owner_test_record(
	struct mt6797_md1_cold_owner_test_backend *backend,
	enum mt6797_md1_cold_owner_test_op op)
{
	backend->calls[backend->count++] = op;
	return backend->fail && backend->fail_op == op ? -EIO : 0;
}

static int mt6797_md1_cold_owner_test_cold(void *context,
	struct mt6797_md1_cold_state *state)
{
	memset(state, 0, sizeof(*state));
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_COLD);
}

static int mt6797_md1_cold_owner_test_ccif_observe(void *context,
	struct mt6797_md1_cold_owner_ccif *state)
{
	struct mt6797_md1_cold_owner_test_backend *backend = context;

	memset(state, 0, sizeof(*state));
	return mt6797_md1_cold_owner_test_record(backend,
		backend->count && backend->calls[backend->count - 1] ==
		MD1_COLD_TEST_CCIF_CLEAR ? MD1_COLD_TEST_CCIF_AFTER :
		MD1_COLD_TEST_CCIF_BEFORE);
}

static int mt6797_md1_cold_owner_test_ccif_clear(void *context)
{
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_CCIF_CLEAR);
}

static int mt6797_md1_cold_owner_test_ccif_verify(void *context,
	const struct mt6797_md1_cold_owner_ccif *before,
	const struct mt6797_md1_cold_owner_ccif *after)
{
	if (!before || !after)
		return -EINVAL;
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_CCIF_VERIFY);
}

static int mt6797_md1_cold_owner_test_power(void *context)
{
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_POWER);
}

static int mt6797_md1_cold_owner_test_srcclk(void *context)
{
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_SRC_CLK);
}

static int mt6797_md1_cold_owner_test_pll_start(void *context)
{
	return mt6797_md1_cold_owner_test_record(context, MD1_COLD_TEST_PLL_START);
}

static int mt6797_md1_cold_owner_test_pll_poll(void *context,
	struct mt6797_md1_cold_owner_pll_status *status)
{
	struct mt6797_md1_cold_owner_test_backend *backend = context;
	int ret;

	status->stage = MT6797_MD1_COLD_OWNER_PLL_L1SYS;
	status->offset = 0x30;
	status->mask = 0x80800000;
	status->last_value = backend->pll_pending ? 0 : 0x80800000;
	ret = mt6797_md1_cold_owner_test_record(backend, MD1_COLD_TEST_PLL_POLL);
	if (ret)
		return ret;
	if (backend->pll_pending) {
		backend->pll_pending--;
		return -EAGAIN;
	}
	return 0;
}

static const struct mt6797_md1_cold_owner_ops mt6797_md1_cold_owner_test_ops = {
	.abi = MT6797_MD1_COLD_OWNER_ABI,
	.verify_cold_reset_held = mt6797_md1_cold_owner_test_cold,
	.ccif_observe = mt6797_md1_cold_owner_test_ccif_observe,
	.ccif_clear = mt6797_md1_cold_owner_test_ccif_clear,
	.ccif_verify_cleared = mt6797_md1_cold_owner_test_ccif_verify,
	.power_on = mt6797_md1_cold_owner_test_power,
	.enable_srcclk = mt6797_md1_cold_owner_test_srcclk,
	.pll_start = mt6797_md1_cold_owner_test_pll_start,
	.pll_poll = mt6797_md1_cold_owner_test_pll_poll,
};

static const struct mt6797_md1_boot_status_preflight mt6797_md1_cold_owner_ok = {
	.lk_image_loaded = true,
	.boot_status_observed = true,
	.stats0_samples = 3,
	.stats1_samples = 3,
};

static const enum mt6797_md1_cold_owner_test_op
	mt6797_md1_cold_owner_transaction_ops[] = {
	MD1_COLD_TEST_COLD,
	MD1_COLD_TEST_CCIF_BEFORE,
	MD1_COLD_TEST_CCIF_CLEAR,
	MD1_COLD_TEST_CCIF_AFTER,
	MD1_COLD_TEST_CCIF_VERIFY,
	MD1_COLD_TEST_POWER,
	MD1_COLD_TEST_SRC_CLK,
	MD1_COLD_TEST_PLL_START,
	MD1_COLD_TEST_PLL_POLL,
};

static void mt6797_md1_cold_owner_expect_calls(struct kunit *test,
	struct mt6797_md1_cold_owner_test_backend *backend,
	const enum mt6797_md1_cold_owner_test_op *expected, u32 count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->count, count);
	for (i = 0; i < count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_md1_cold_owner_success_stops_before_boot_vector(struct kunit *test)
{
	static const enum mt6797_md1_cold_owner_test_op expected[] = {
		MD1_COLD_TEST_COLD, MD1_COLD_TEST_CCIF_BEFORE,
		MD1_COLD_TEST_CCIF_CLEAR, MD1_COLD_TEST_CCIF_AFTER,
		MD1_COLD_TEST_CCIF_VERIFY, MD1_COLD_TEST_POWER,
		MD1_COLD_TEST_SRC_CLK, MD1_COLD_TEST_PLL_START,
		MD1_COLD_TEST_PLL_POLL,
	};
	struct mt6797_md1_cold_owner_test_backend backend = { };
	struct mt6797_md1_cold_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_init(&owner,
						&mt6797_md1_cold_owner_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_run(&owner,
						       &mt6797_md1_cold_owner_ok), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_state(&owner),
		MT6797_MD1_COLD_OWNER_PLL_READY_WDT_UNOWNED);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner,
						       &mt6797_md1_cold_owner_ok), -EALREADY);
	mt6797_md1_cold_owner_expect_calls(test, &backend, expected,
					    ARRAY_SIZE(expected));
}

static void mt6797_md1_cold_owner_refuses_missing_observation_without_touching_hw(
	struct kunit *test)
{
	struct mt6797_md1_cold_owner_test_backend backend = { };
	struct mt6797_md1_boot_status_preflight preflight = {
		.lk_image_loaded = true,
		.boot_status_observed = true,
		.stats0_samples = 2,
		.stats1_samples = 3,
	};
	struct mt6797_md1_cold_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_init(&owner,
						&mt6797_md1_cold_owner_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner, &preflight), -EPERM);
	KUNIT_EXPECT_EQ(test, backend.count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_state(&owner),
		MT6797_MD1_COLD_OWNER_OFF);
}

static void mt6797_md1_cold_owner_cold_rejection_is_non_destructive(struct kunit *test)
{
	struct mt6797_md1_cold_owner_test_backend backend = {
		.fail = true,
		.fail_op = MD1_COLD_TEST_COLD,
	};
	struct mt6797_md1_cold_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_init(&owner,
						&mt6797_md1_cold_owner_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner,
						       &mt6797_md1_cold_owner_ok), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_state(&owner),
		MT6797_MD1_COLD_OWNER_OFF);
	KUNIT_EXPECT_EQ(test, backend.count, 1U);
}

static void mt6797_md1_cold_owner_every_post_observation_failure_is_fatal(
	struct kunit *test)
{
	u32 i;

	for (i = 1; i < ARRAY_SIZE(mt6797_md1_cold_owner_transaction_ops); i++) {
		struct mt6797_md1_cold_owner_test_backend backend = {
			.fail = true,
			.fail_op = mt6797_md1_cold_owner_transaction_ops[i],
		};
		struct mt6797_md1_cold_owner owner;

		KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_init(&owner,
							&mt6797_md1_cold_owner_test_ops,
							&backend), 0);
		KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner,
							       &mt6797_md1_cold_owner_ok), -EIO);
		KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_state(&owner),
			MT6797_MD1_COLD_OWNER_FATAL);
		KUNIT_EXPECT_EQ(test, owner.last_error, -EIO);
		KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner,
							       &mt6797_md1_cold_owner_ok), -EIO);
		mt6797_md1_cold_owner_expect_calls(test, &backend,
			mt6797_md1_cold_owner_transaction_ops, i + 1);
	}
}

static void mt6797_md1_cold_owner_pll_timeout_captures_last_status_and_latches(
	struct kunit *test)
{
	struct mt6797_md1_cold_owner_test_backend backend = {
		.pll_pending = MT6797_MD1_COLD_OWNER_MAX_PLL_POLLS,
	};
	struct mt6797_md1_cold_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_md1_cold_owner_init(&owner,
						&mt6797_md1_cold_owner_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_run(&owner,
						       &mt6797_md1_cold_owner_ok), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cold_owner_state(&owner),
		MT6797_MD1_COLD_OWNER_FATAL);
	KUNIT_EXPECT_EQ(test, owner.last_pll.stage,
		MT6797_MD1_COLD_OWNER_PLL_L1SYS);
	KUNIT_EXPECT_EQ(test, owner.last_pll.offset, 0x30U);
	KUNIT_EXPECT_EQ(test, owner.last_pll.mask, 0x80800000U);
	KUNIT_EXPECT_EQ(test, owner.last_pll.last_value, 0U);
	KUNIT_EXPECT_EQ(test, backend.count,
		8U + MT6797_MD1_COLD_OWNER_MAX_PLL_POLLS);
}

static struct kunit_case mt6797_md1_cold_owner_test_cases[] = {
	KUNIT_CASE(mt6797_md1_cold_owner_success_stops_before_boot_vector),
	KUNIT_CASE(mt6797_md1_cold_owner_refuses_missing_observation_without_touching_hw),
	KUNIT_CASE(mt6797_md1_cold_owner_cold_rejection_is_non_destructive),
	KUNIT_CASE(mt6797_md1_cold_owner_every_post_observation_failure_is_fatal),
	KUNIT_CASE(mt6797_md1_cold_owner_pll_timeout_captures_last_status_and_latches),
	{}
};

static struct kunit_suite mt6797_md1_cold_owner_test_suite = {
	.name = "mt6797-md1-cold-owner",
	.test_cases = mt6797_md1_cold_owner_test_cases,
};
kunit_test_suite(mt6797_md1_cold_owner_test_suite);

MODULE_LICENSE("GPL");
