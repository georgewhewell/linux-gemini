// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitfield.h>

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

static void mt6797_cpuclk_read_fence_test(struct kunit *test)
{
	const unsigned long fid = MT6797_CPUCLK_SMC_REG_READ;
	const u32 con0 = MT6797_CPUCLK_MP2_PLL_CON0;
	const u32 con1 = MT6797_CPUCLK_MP2_PLL_CON1;
	bool allowed;

	allowed = mt6797_cpuclk_secure_read_allowed(fid, con0);
	KUNIT_EXPECT_TRUE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid, con1);
	KUNIT_EXPECT_TRUE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid + 1, con0);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid, con1 + 4);
	KUNIT_EXPECT_FALSE(test, allowed);
}

static void mt6797_cpuclk_snapshot_equality_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot first = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_snapshot second = first;

	KUNIT_EXPECT_TRUE(test, mt6797_cpuclk_snapshots_equal(&first, &second));
	second.clkdiv ^= 1;
	KUNIT_EXPECT_FALSE(test,
			   mt6797_cpuclk_snapshots_equal(&first, &second));
	KUNIT_EXPECT_FALSE(test, mt6797_cpuclk_snapshots_equal(NULL, &second));
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
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(1200, 0), 1200UL);
}

static void mt6797_cpuclk_decode_rejects_invalid_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_test_snapshot();
	struct mt6797_cpuclk_state state;
	int ret;

	snapshot.clkdiv &= ~(0x1f << 5);
	snapshot.clkdiv |= 1 << 5;
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

/* M6 cold capture, then the upstream post-CPU_ON 750MHz PLL tuple. */
static void mt6797_cpuclk_cold_mp2_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = {
		.armpll_con1 = { 0xc1114000, 0x400c4000, 0, 0xc10c1d89 },
		.muxsel = 0x54,
	};
	struct mt6797_cpuclk_state state;
	struct mt6797_cpuclk_hop_plan plan;
	const u64 expected_rates[] = { 897000000, 1274000000, 0, 629999328 };
	unsigned int source, divider;

	KUNIT_ASSERT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, &state), 0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_CLK26M);
	KUNIT_EXPECT_EQ(test, state.divider, (u8)0);
	KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(
		MT6797_TEST_CLK26M_RATE, state.divider), 26000000UL);
	/* Observation of the cold state must not authorize a rate mutation. */
	KUNIT_EXPECT_TRUE(test, mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, 750000000, &plan) != 0);

	for (source = 1; source < MT6797_CPUCLK_NR_SOURCES; source++) {
		snapshot.muxsel = 0x54 | source;
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
			&snapshot, MT6797_TEST_CLK26M_RATE, &state), -ERANGE);
	}
	snapshot.muxsel = 0x54;
	snapshot.mp2_armpll_con0 = 1;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, &state), -ERANGE);
	snapshot.mp2_armpll_con0 = 0;
	snapshot.mp2_armpll_con1 = BIT(31);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, &state), -ERANGE);
	snapshot.mp2_armpll_con1 = 0;
	snapshot.clkdiv = 8;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, &state), -ERANGE);

	/* Unknown selectors remain rejected, including with a running PLL. */
	snapshot.muxsel = 0x55;
	snapshot.mp2_armpll_con0 = 0x00ff1101;
	snapshot.mp2_armpll_con1 = 0xb9b13b14;
	for (divider = 0; divider < 32; divider++) {
		bool valid = divider == 0 || (divider >= 8 && divider <= 11) ||
			(divider >= 16 && divider <= 20) ||
			(divider >= 24 && divider <= 29);

		snapshot.clkdiv = divider;
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
			&snapshot, MT6797_TEST_CLK26M_RATE, &state), valid ? 0 : -ERANGE);
	}
	snapshot.clkdiv = 8;
	KUNIT_ASSERT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
		&snapshot, MT6797_TEST_CLK26M_RATE, &state), 0);
	KUNIT_EXPECT_EQ(test, state.source, (u8)MT6797_CPUCLK_ARMPLL);
	KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)750000000);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_apply_divider(state.armpll_rate,
		state.divider), 750000000UL);
	for (source = MT6797_CPUCLK_MP0; source < MT6797_CPUCLK_NR_DOMAINS;
	     source++) {
		if (source == MT6797_CPUCLK_MP2)
			continue;
		KUNIT_ASSERT_EQ(test, mt6797_cpuclk_decode_state(source, &snapshot,
			MT6797_TEST_CLK26M_RATE, &state), 0);
		KUNIT_EXPECT_EQ(test, state.divider, (u8)0);
		KUNIT_EXPECT_EQ(test, state.armpll_rate, expected_rates[source]);
	}
}

static void mt6797_cpuclk_cold_admission_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = { .muxsel = 0x54 };

	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), 0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(NULL,
		26000000), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot, 0), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		25000000), -ERANGE);
	/* Other clusters' divider/mux fields do not define the MP2 contract. */
	snapshot.clkdiv = 0xffffffe0;
	snapshot.muxsel = 0xfffffffc;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), 0);
	snapshot.muxsel = 0x55;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), -ERANGE);
	snapshot.clkdiv = 8;
	snapshot.mp2_armpll_con0 = 0x00ff1101;
	snapshot.mp2_armpll_con1 = 0xb9b13b14;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), -EBUSY);
	/* Even an unselected programmed PLL is outside the cold contract. */
	snapshot.muxsel = 0x54;
	snapshot.clkdiv = 0;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), -EBUSY);
}

/* Gemini LK hand-off: LL 897 MHz (/2), L 1274 MHz (/1), both on ARMPLL. */
static struct mt6797_cpuclk_snapshot mt6797_cpuclk_boot_snapshot(void)
{
	struct mt6797_cpuclk_snapshot snapshot = {
		.armpll_con1 = { 0xc1114000, 0x400c4000, 0, 0xc10c1d89 },
		.muxsel = 0x54,
		.clkdiv = 0,
		.sync_dcm = (12 << 9) | BIT(14) | (18 << 17) | BIT(22),
	};

	return snapshot;
}

static void mt6797_cpuclk_hop_plan_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_boot_snapshot();
	struct mt6797_cpuclk_hop_plan plan;
	int ret;

	ret = mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				     MT6797_TEST_CLK26M_RATE, 624000000, &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.dds, 0xc0000U);
	KUNIT_EXPECT_EQ(test, plan.rate, 624000000UL);
	KUNIT_EXPECT_EQ(test, plan.initial_rate, (u64)897000000);
	KUNIT_EXPECT_TRUE(test, plan.dcm_before_hop);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(13, 9), plan.dcm.value[0]), 8U);
	KUNIT_EXPECT_FALSE(test, plan.dcm.value[1] & BIT(14));
	KUNIT_EXPECT_TRUE(test, plan.dcm.value[2] & BIT(14));
	/* The other cluster's DCM field is preserved. */
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(21, 17), plan.dcm.value[2]),
			18U);
	KUNIT_EXPECT_TRUE(test, mt6797_cpuclk_snapshots_equal(&plan.snapshot,
							       &snapshot));

	/* Raising the rate programs the DCM divider after the hop. */
	ret = mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				     MT6797_TEST_CLK26M_RATE, 1014000000,
				     &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.dds, 0x138000U);
	KUNIT_EXPECT_FALSE(test, plan.dcm_before_hop);

	ret = mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP1, &snapshot,
				     MT6797_TEST_CLK26M_RATE, 1209000000,
				     &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.dds, 0xba000U);
	KUNIT_EXPECT_EQ(test, plan.rate, 1209000000UL);
	KUNIT_EXPECT_EQ(test, plan.initial_rate, (u64)1274000000);
	KUNIT_EXPECT_TRUE(test, plan.dcm_before_hop);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(21, 17), plan.dcm.value[0]),
			17U);

	ret = mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP1, &snapshot,
				     MT6797_TEST_CLK26M_RATE, 1092000000,
				     &plan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, plan.dds, 0xa8000U);
}

static void mt6797_cpuclk_hop_plan_rejects_invalid_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_boot_snapshot();
	struct mt6797_cpuclk_hop_plan plan;

	/* A post-divider change needs a clock switch, not a hop. */
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 1118000000,
				       &plan),
		-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP1, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 962000000,
				       &plan),
		-EOPNOTSUPP);
	/* Rates that need the CKDIV path divider are not in the rate table. */
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 481000000,
				       &plan),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP2, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 750000000,
				       &plan),
		-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_CCI, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 611000000,
				       &plan),
		-EOPNOTSUPP);

	/* The cluster must run from its ARMPLL through a 1/1 path divider. */
	snapshot.muxsel = (snapshot.muxsel & ~GENMASK(3, 2)) |
			  (MT6797_CPUCLK_MAINPLL << 2);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 624000000,
				       &plan),
		-EOPNOTSUPP);
	snapshot = mt6797_cpuclk_boot_snapshot();
	snapshot.clkdiv = 10 << 5;
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 624000000,
				       &plan),
		-EOPNOTSUPP);
	snapshot.clkdiv = 8 << 5;
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, &snapshot,
				       MT6797_TEST_CLK26M_RATE, 624000000,
				       &plan),
		0);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_hop(MT6797_CPUCLK_MP0, NULL,
				       MT6797_TEST_CLK26M_RATE, 624000000,
				       &plan),
		-EINVAL);
}

static void mt6797_cpuclk_pll_rate_plan_test(struct kunit *test)
{
	unsigned long actual_rate;
	u32 con1;
	int ret;

	ret = mt6797_cpuclk_plan_pll_rate(MT6797_CPUCLK_MP0, 1014000000,
					  MT6797_TEST_CLK26M_RATE, BIT(29),
					  &con1, &actual_rate);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(26, 24), con1), 1U);
	KUNIT_EXPECT_TRUE(test, con1 & BIT(31));
	KUNIT_EXPECT_TRUE(test, con1 & BIT(29));
	KUNIT_EXPECT_LE(test, actual_rate, 1014000000UL);

	ret = mt6797_cpuclk_plan_pll_rate(MT6797_CPUCLK_MP1, 1950000000,
					  MT6797_TEST_CLK26M_RATE, 0,
					  &con1, &actual_rate);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(26, 24), con1), 0U);
	KUNIT_EXPECT_LE(test, actual_rate, 1950000000UL);

	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_pll_rate(MT6797_CPUCLK_MP0, 481000000,
					       MT6797_TEST_CLK26M_RATE, 0,
					       &con1, &actual_rate),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_pll_rate(MT6797_CPUCLK_MP2, 1950000000,
					       MT6797_TEST_CLK26M_RATE, 0,
					       &con1, &actual_rate),
		-EINVAL);
}

static void mt6797_cpuclk_mux_dcm_plan_test(struct kunit *test)
{
	struct mt6797_cpuclk_dcm_plan dcm;
	u32 value;

	KUNIT_ASSERT_EQ(test,
		mt6797_cpuclk_plan_mux(MT6797_CPUCLK_MP0,
				       MT6797_CPUCLK_MAINPLL, ~0U, &value),
		0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(3, 2), value),
			(u32)MT6797_CPUCLK_MAINPLL);

	KUNIT_ASSERT_EQ(test,
		mt6797_cpuclk_plan_dcm(MT6797_CPUCLK_MP1, 1950000000,
				       BIT(14), &dcm),
		0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(21, 17), dcm.value[0]), 27U);
	KUNIT_EXPECT_FALSE(test, dcm.value[1] & BIT(22));
	KUNIT_EXPECT_TRUE(test, dcm.value[2] & BIT(22));
	KUNIT_EXPECT_TRUE(test, dcm.value[2] & BIT(14));
	KUNIT_EXPECT_EQ(test,
		mt6797_cpuclk_plan_dcm(MT6797_CPUCLK_CCI, 988000000, 0,
				       &dcm),
		-EOPNOTSUPP);
}

static struct kunit_case mt6797_cpuclk_test_cases[] = {
	KUNIT_CASE(mt6797_cpuclk_read_fence_test),
	KUNIT_CASE(mt6797_cpuclk_snapshot_equality_test),
	KUNIT_CASE(mt6797_cpuclk_decode_fields_test),
	KUNIT_CASE(mt6797_cpuclk_pll_formats_test),
	KUNIT_CASE(mt6797_cpuclk_divider_test),
	KUNIT_CASE(mt6797_cpuclk_cold_mp2_test),
	KUNIT_CASE(mt6797_cpuclk_cold_admission_test),
	KUNIT_CASE(mt6797_cpuclk_decode_rejects_invalid_test),
	KUNIT_CASE(mt6797_cpuclk_hop_plan_test),
	KUNIT_CASE(mt6797_cpuclk_hop_plan_rejects_invalid_test),
	KUNIT_CASE(mt6797_cpuclk_pll_rate_plan_test),
	KUNIT_CASE(mt6797_cpuclk_mux_dcm_plan_test),
	{}
};

static struct kunit_suite mt6797_cpuclk_test_suite = {
	.name = "mt6797-cpuclk",
	.test_cases = mt6797_cpuclk_test_cases,
};

kunit_test_suite(mt6797_cpuclk_test_suite);

MODULE_LICENSE("GPL");
