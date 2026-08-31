// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free order and one-way-failure tests for the MD1 WDT key gate. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-wdt-gate.h>

enum mt6797_md1_wdt_gate_test_op {
	MD1_WDT_TEST_OBSERVE_PRE,
	MD1_WDT_TEST_MD_KEY,
	MD1_WDT_TEST_L1_KEY,
	MD1_WDT_TEST_OBSERVE_POST,
};

struct mt6797_md1_wdt_gate_test_backend {
	enum mt6797_md1_wdt_gate_test_op calls[8];
	u32 count;
	bool fail;
	enum mt6797_md1_wdt_gate_test_op fail_op;
	u32 observe_count;
};

static int mt6797_md1_wdt_gate_test_record(
	struct mt6797_md1_wdt_gate_test_backend *backend,
	enum mt6797_md1_wdt_gate_test_op op)
{
	backend->calls[backend->count++] = op;
	return backend->fail && backend->fail_op == op ? -EIO : 0;
}

static int mt6797_md1_wdt_gate_test_observe(void *context,
	struct mt6797_md1_wdt_status_observation *observation)
{
	struct mt6797_md1_wdt_gate_test_backend *backend = context;
	enum mt6797_md1_wdt_gate_test_op op;

	op = backend->observe_count++ ? MD1_WDT_TEST_OBSERVE_POST :
		MD1_WDT_TEST_OBSERVE_PRE;
	observation->md_status = op == MD1_WDT_TEST_OBSERVE_PRE ?
		0x12345678 : 0x87654321;
	observation->l1_status = op == MD1_WDT_TEST_OBSERVE_PRE ?
		0xabcdef01 : 0x10fedcba;
	return mt6797_md1_wdt_gate_test_record(backend, op);
}

static int mt6797_md1_wdt_gate_test_md_key(void *context)
{
	return mt6797_md1_wdt_gate_test_record(context, MD1_WDT_TEST_MD_KEY);
}

static int mt6797_md1_wdt_gate_test_l1_key(void *context)
{
	return mt6797_md1_wdt_gate_test_record(context, MD1_WDT_TEST_L1_KEY);
}

static const struct mt6797_md1_wdt_gate_ops mt6797_md1_wdt_gate_test_ops = {
	.abi = MT6797_MD1_WDT_GATE_ABI,
	.observe_status = mt6797_md1_wdt_gate_test_observe,
	.write_md_mode_key = mt6797_md1_wdt_gate_test_md_key,
	.write_l1_mode_key = mt6797_md1_wdt_gate_test_l1_key,
};

static const struct mt6797_md1_wdt_gate_preflight mt6797_md1_wdt_gate_ready = {
	.cold_owner_pll_ready_wdt_unowned = true,
};

static const enum mt6797_md1_wdt_gate_test_op mt6797_md1_wdt_gate_order[] = {
	MD1_WDT_TEST_OBSERVE_PRE,
	MD1_WDT_TEST_MD_KEY,
	MD1_WDT_TEST_L1_KEY,
	MD1_WDT_TEST_OBSERVE_POST,
};

static void mt6797_md1_wdt_gate_expect_prefix(struct kunit *test,
	struct mt6797_md1_wdt_gate_test_backend *backend, u32 count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->count, count);
	for (i = 0; i < count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], mt6797_md1_wdt_gate_order[i]);
}

static void mt6797_md1_wdt_gate_success_has_pre_and_post_status_observations(
	struct kunit *test)
{
	struct mt6797_md1_wdt_gate_test_backend backend = { };
	struct mt6797_md1_wdt_gate gate;

	KUNIT_ASSERT_EQ(test, mt6797_md1_wdt_gate_init(&gate,
						&mt6797_md1_wdt_gate_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_wdt_gate_run(&gate,
						       &mt6797_md1_wdt_gate_ready), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_state(&gate),
		MT6797_MD1_WDT_GATE_KEYS_ATTEMPTED_WRITE_ONLY);
	KUNIT_EXPECT_EQ(test, gate.observation.md_status, 0x12345678U);
	KUNIT_EXPECT_EQ(test, gate.observation.l1_status, 0xabcdef01U);
	KUNIT_EXPECT_EQ(test, gate.post_observation.md_status, 0x87654321U);
	KUNIT_EXPECT_EQ(test, gate.post_observation.l1_status, 0x10fedcbaU);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_run(&gate,
						       &mt6797_md1_wdt_gate_ready), -EALREADY);
	mt6797_md1_wdt_gate_expect_prefix(test, &backend,
					     ARRAY_SIZE(mt6797_md1_wdt_gate_order));
}

static void mt6797_md1_wdt_gate_refuses_unattested_pll_without_touching_hw(
	struct kunit *test)
{
	struct mt6797_md1_wdt_gate_test_backend backend = { };
	struct mt6797_md1_wdt_gate_preflight preflight = { };
	struct mt6797_md1_wdt_gate gate;

	KUNIT_ASSERT_EQ(test, mt6797_md1_wdt_gate_init(&gate,
						&mt6797_md1_wdt_gate_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_run(&gate, &preflight), -EPERM);
	KUNIT_EXPECT_EQ(test, backend.count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_state(&gate),
		MT6797_MD1_WDT_GATE_OFF);
}

static void mt6797_md1_wdt_gate_observe_failure_is_non_destructive(struct kunit *test)
{
	struct mt6797_md1_wdt_gate_test_backend backend = {
		.fail = true,
		.fail_op = MD1_WDT_TEST_OBSERVE_PRE,
	};
	struct mt6797_md1_wdt_gate gate;

	KUNIT_ASSERT_EQ(test, mt6797_md1_wdt_gate_init(&gate,
						&mt6797_md1_wdt_gate_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_run(&gate,
						       &mt6797_md1_wdt_gate_ready), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_state(&gate),
		MT6797_MD1_WDT_GATE_OFF);
	mt6797_md1_wdt_gate_expect_prefix(test, &backend, 1);
}

static void mt6797_md1_wdt_gate_any_post_preflight_failure_is_fatal(struct kunit *test)
{
	u32 i;

	for (i = 1; i < ARRAY_SIZE(mt6797_md1_wdt_gate_order); i++) {
		struct mt6797_md1_wdt_gate_test_backend backend = {
			.fail = true,
			.fail_op = mt6797_md1_wdt_gate_order[i],
		};
		struct mt6797_md1_wdt_gate gate;

		KUNIT_ASSERT_EQ(test, mt6797_md1_wdt_gate_init(&gate,
							&mt6797_md1_wdt_gate_test_ops,
							&backend), 0);
		KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_run(&gate,
							       &mt6797_md1_wdt_gate_ready), -EIO);
		KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_state(&gate),
			MT6797_MD1_WDT_GATE_FATAL);
		KUNIT_EXPECT_EQ(test, gate.last_error, -EIO);
		KUNIT_EXPECT_EQ(test, mt6797_md1_wdt_gate_run(&gate,
							       &mt6797_md1_wdt_gate_ready), -EIO);
		mt6797_md1_wdt_gate_expect_prefix(test, &backend, i + 1);
	}
}

static struct kunit_case mt6797_md1_wdt_gate_test_cases[] = {
	KUNIT_CASE(mt6797_md1_wdt_gate_success_has_pre_and_post_status_observations),
	KUNIT_CASE(mt6797_md1_wdt_gate_refuses_unattested_pll_without_touching_hw),
	KUNIT_CASE(mt6797_md1_wdt_gate_observe_failure_is_non_destructive),
	KUNIT_CASE(mt6797_md1_wdt_gate_any_post_preflight_failure_is_fatal),
	{}
};

static struct kunit_suite mt6797_md1_wdt_gate_test_suite = {
	.name = "mt6797-md1-wdt-gate",
	.test_cases = mt6797_md1_wdt_gate_test_cases,
};
kunit_test_suite(mt6797_md1_wdt_gate_test_suite);

MODULE_LICENSE("GPL");
