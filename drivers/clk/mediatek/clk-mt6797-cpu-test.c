// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "clk-mt6797-cpu.h"

#define MT6797_TEST_CLK26M_RATE 26000000UL

static struct mt6797_cpuclk_snapshot mt6797_cpuclk_test_snapshot(void)
{
	struct mt6797_cpuclk_snapshot snapshot = {
		.armpll_con1 = {
			[MT6797_CPUCLK_MP0] = BIT(14),
			[MT6797_CPUCLK_MP1] = BIT(14),
			[MT6797_CPUCLK_CCI] = BIT(14),
		},
		.mp2_armpll_con1 = BIT(24),
		.clkdiv = (8 << 0) | (8 << 5) | (8 << 10) | (8 << 15),
	};

	return snapshot;
}

static void mt6797_cpuclk_decode_fields_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_state state;

	snapshot.muxsel =
		(MT6797_CPUCLK_ARMPLL << 2) | (MT6797_CPUCLK_MAINPLL << 4) |
		(MT6797_CPUCLK_UNIVPLL << 0) | (MT6797_CPUCLK_CLK26M << 6);

	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP0, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_ARMPLL);
	KUNIT_EXPECT_EQ(test, state.divider, (u8)8);
	KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)MT6797_TEST_CLK26M_RATE);

	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP1, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_MAINPLL);

	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_UNIVPLL);

	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_CCI, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_CLK26M);
}

static void mt6797_cpuclk_pll_formats_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_state state;

	snapshot.armpll_con1[MT6797_CPUCLK_MP0] = BIT(14) | BIT(24);
	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP0, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)13000000);

	snapshot.mp2_armpll_con0 = 2 << 12;
	snapshot.mp2_armpll_con1 = 2 << 24;
	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			0);
	KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)13000000);
}

static void mt6797_cpuclk_divider_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 8), 1200UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 9), 900UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 10), 600UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 11), 300UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 17), 960UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 25), 1000UL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 0), 0UL);
}

static void mt6797_cpuclk_decode_rejects_invalid_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_state state;
	int ret;

	snapshot.clkdiv &= ~(0x1f << 5);
	KUNIT_EXPECT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP0, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			-ERANGE);

	snapshot = mt6797_cpuclk_test_snapshot();
	snapshot.armpll_con1[MT6797_CPUCLK_MP0] |= 3 << 24;
	KUNIT_EXPECT_EQ(test,
			mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP0, &snapshot,
						   MT6797_TEST_CLK26M_RATE,
						   &state),
			-ERANGE);

	ret = mt6797_cpuclk_decode_state(MT6797_CPUCLK_NR_DOMAINS, &snapshot,
					 MT6797_TEST_CLK26M_RATE, &state);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_cpuclk_rate_plan_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	const struct mt6797_cpuclk_state target = {
		.armpll_rate = 52000000,
		.source = MT6797_CPUCLK_MAINPLL,
		.divider = 8,
	};
	struct mt6797_cpuclk_state rollback;
	struct mt6797_cpuclk_rate_plan plan;
	int ret;

	snapshot.muxsel = MT6797_CPUCLK_MAINPLL;
	ret = mt6797_cpuclk_build_rate_plan(MT6797_CPUCLK_MP2, &snapshot,
					    MT6797_TEST_CLK26M_RATE, &target,
					    &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.intermediate_source,
			(u8)MT6797_CPUCLK_MAINPLL);
	KUNIT_ASSERT_EQ(test,
			mt6797_cpuclk_get_rollback_state(&plan, &rollback), 0);
	KUNIT_EXPECT_EQ(test, rollback.armpll_rate,
			(u64)MT6797_TEST_CLK26M_RATE);
	KUNIT_EXPECT_EQ(test, rollback.source, (u8)MT6797_CPUCLK_MAINPLL);
	KUNIT_EXPECT_EQ(test, rollback.divider, (u8)8);
	KUNIT_EXPECT_EQ(test, plan.rollback_snapshot.muxsel, snapshot.muxsel);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_rate_plan(NULL, &plan),
			-EOPNOTSUPP);
}

static void mt6797_cpuclk_rate_plan_rejects_invalid_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_state target = {
		.armpll_rate = 52000000,
		.source = MT6797_CPUCLK_MAINPLL,
		.divider = 8,
	};
	struct mt6797_cpuclk_state rollback;
	struct mt6797_cpuclk_rate_plan plan;
	int ret;

	snapshot.muxsel = MT6797_CPUCLK_MAINPLL;
	target.divider = 9;
	ret = mt6797_cpuclk_build_rate_plan(MT6797_CPUCLK_MP2, &snapshot,
					    MT6797_TEST_CLK26M_RATE, &target,
					    &plan);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);

	target.divider = 8;
	ret = mt6797_cpuclk_build_rate_plan(MT6797_CPUCLK_MP2, &snapshot,
					    MT6797_TEST_CLK26M_RATE, &target,
					    &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	plan.rollback_snapshot.clkdiv = 10;
	KUNIT_EXPECT_EQ(test,
			mt6797_cpuclk_get_rollback_state(&plan, &rollback),
			-EINVAL);
}

static struct kunit_case mt6797_cpuclk_test_cases[] = {
	KUNIT_CASE(mt6797_cpuclk_decode_fields_test),
	KUNIT_CASE(mt6797_cpuclk_pll_formats_test),
	KUNIT_CASE(mt6797_cpuclk_divider_test),
	KUNIT_CASE(mt6797_cpuclk_decode_rejects_invalid_test),
	KUNIT_CASE(mt6797_cpuclk_rate_plan_test),
	KUNIT_CASE(mt6797_cpuclk_rate_plan_rejects_invalid_test),
	{}
};

static struct kunit_suite mt6797_cpuclk_test_suite = {
	.name = "mt6797-cpuclk",
	.test_cases = mt6797_cpuclk_test_cases,
};

kunit_test_suite(mt6797_cpuclk_test_suite);

MODULE_LICENSE("GPL");
