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
	allowed = mt6797_cpuclk_secure_read_allowed(fid, MT6797_CPUCLK_MP2_SYNC_DCM);
	KUNIT_EXPECT_TRUE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid, MT6797_CPUCLK_MP2_SYNC_DCM + 4);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid, MT6797_CPUCLK_MP2_SYNC_DCM - 4);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = mt6797_cpuclk_secure_read_allowed(fid + 1, MT6797_CPUCLK_MP2_SYNC_DCM);
	KUNIT_EXPECT_FALSE(test, allowed);
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
	struct mt6797_cpuclk_transition_plan plan;
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
	KUNIT_EXPECT_TRUE(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP2,
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
	/* Full rail removal clears PLL words but retains the divider. */
	for (divider = 0; divider < 32; divider++) {
		bool valid = divider == 0 || (divider >= 8 && divider <= 11) ||
			(divider >= 16 && divider <= 20) ||
			(divider >= 24 && divider <= 29);

		snapshot.clkdiv = divider;
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
			&snapshot, MT6797_TEST_CLK26M_RATE, &state), valid ? 0 : -ERANGE);
		if (valid)
			KUNIT_EXPECT_EQ(test, state.armpll_rate, (u64)0);
	}

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
	/* A warm reset observation is not permission to take the cold path. */
	snapshot.clkdiv = 8;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&snapshot,
		26000000), -EBUSY);
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

/* Validate every retained MP2 bit against a saved running observation. */
static void mt6797_cpuclk_stopped_mp2_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot running = {
		.mp2_armpll_con0 = 0x00ff1101,
		.mp2_armpll_con1 = 0xb9b13b14,
		.muxsel = 0x55,
		.clkdiv = 8,
	};
	struct mt6797_cpuclk_snapshot stopped = running, changed, corrupt;
	unsigned int bit;

	stopped.mp2_armpll_con0 &= ~BIT(0);
	stopped.muxsel &= ~BIT(0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 26000000), 0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&stopped,
		26000000), -EBUSY);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(NULL,
		&stopped, 26000000), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		NULL, 26000000), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 25000000), -ERANGE);
	for (bit = 0; bit < 32; bit++) {
		changed = stopped;
		changed.mp2_armpll_con0 ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&changed, 26000000), -EBUSY);
		changed = stopped;
		changed.mp2_armpll_con1 ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&changed, 26000000), -EBUSY);
		changed = stopped;
		changed.muxsel ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&changed, 26000000), bit < 2 ? -EBUSY : 0);
		changed = stopped;
		changed.clkdiv ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&changed, 26000000), bit < 5 ? -EBUSY : 0);
	}
	changed = stopped;
	memset(changed.armpll_con1, 0xff, sizeof(changed.armpll_con1));
	changed.sync_dcm = 0xffffffff;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&changed, 26000000), 0);
	/* Trial04: mux=0x54, divider=0x42108, both PLL words reset by rail-off. */
	changed = stopped;
	changed.muxsel = 0x54;
	changed.clkdiv = 0x42108;
	changed.mp2_armpll_con0 = 0;
	changed.mp2_armpll_con1 = 0;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&changed, 26000000), 0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_cold_mp2(&changed,
		26000000), -EBUSY);
	for (bit = 0; bit < 32; bit++) {
		corrupt = changed;
		corrupt.mp2_armpll_con0 ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&corrupt, 26000000), -EBUSY);
		corrupt = changed;
		corrupt.mp2_armpll_con1 ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&corrupt, 26000000), -EBUSY);
		corrupt = changed;
		corrupt.muxsel ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&corrupt, 26000000), bit < 2 ? -EBUSY : 0);
		corrupt = changed;
		corrupt.clkdiv ^= BIT(bit);
		KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
			&corrupt, 26000000), bit < 5 ? -EBUSY : 0);
	}
	/* A disabled PLL, another parent or malformed programming is no receipt. */
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&stopped,
		&stopped, 26000000), -EBUSY);
	running.mp2_armpll_con0 &= ~BIT(0);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 26000000), -EBUSY);
	running.mp2_armpll_con0 |= BIT(0);
	running.muxsel = 0x56;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 26000000), -EBUSY);
	running.muxsel = 0x55;
	running.clkdiv = 1;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 26000000), -ERANGE);
	running.clkdiv = 8;
	running.mp2_armpll_con1 = 0;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_validate_stopped_mp2(&running,
		&stopped, 26000000), -ERANGE);
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
		mt6797_cpuclk_plan_dcm(MT6797_CPUCLK_CCI, 988000000, BIT(22),
				       &dcm),
		0);
	KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(5, 1), dcm.value[0]), 14U);
	KUNIT_EXPECT_FALSE(test, dcm.value[1] & BIT(6));
	KUNIT_EXPECT_TRUE(test, dcm.value[2] & BIT(6));
	KUNIT_EXPECT_TRUE(test, dcm.value[2] & BIT(22));
}

static void mt6797_cpuclk_transition_pairs_test(struct kunit *test)
{
	/* Independent vendor fixtures: CPU MHz, physical CKDIV divisor. */
	static const unsigned int rates[] = { 221, 338, 481, 624, 715, 806, 897 };
	static const unsigned int divs[] = { 4, 2, 2, 1, 1, 1, 1 };
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_transition_plan plan;
	unsigned long old_pll, new_pll, first, last, minimum, maximum;
	unsigned int i, j, dcm, hop_div, selector;

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		for (j = 0; j < ARRAY_SIZE(rates); j++) {
			for (dcm = 0; dcm < 32; dcm++) {
				snapshot = mt6797_cpuclk_boot_snapshot();
				old_pll = rates[i] * divs[i] * 1000000UL;
				new_pll = rates[j] * divs[j] * 1000000UL;
				snapshot.armpll_con1[0] = BIT(31) | BIT(24) |
					((u64)old_pll * 32768 / 26000000);
				selector = divs[i] == 4 ? 11 : divs[i] == 2 ? 10 : 8;
				snapshot.clkdiv = BIT(31) | (8 << 15) | (8 << 10) |
					(selector << 5) | 8;
				snapshot.sync_dcm = (snapshot.sync_dcm & ~GENMASK(13, 9)) |
					(dcm << 9);
				KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_transition(
					MT6797_CPUCLK_MP0, &snapshot, 26000000,
					rates[j] * 1000000UL, &plan), 0);
				KUNIT_EXPECT_EQ(test, plan.initial_rate, rates[i] * 1000000ULL);
				KUNIT_EXPECT_EQ(test, plan.rate, rates[j] * 1000000UL);
				KUNIT_EXPECT_EQ(test, plan.pll_rate, new_pll);
				KUNIT_EXPECT_EQ(test, (u64)plan.dds * 26000000 / 32768,
					(u64)new_pll);
				KUNIT_EXPECT_EQ(test, plan.divider_before_hop, divs[j] > divs[i]);
				hop_div = divs[i] > divs[j] ? divs[i] : divs[j];
				first = old_pll / hop_div;
				last = new_pll / hop_div;
				minimum = min(first, last);
				KUNIT_EXPECT_EQ(test, plan.minimum_rate, minimum);
				maximum = (rates[i] > rates[j] ? rates[i] : rates[j]) * 1000000UL;
				KUNIT_EXPECT_LE(test, first, maximum);
				KUNIT_EXPECT_LE(test, last, maximum);
				KUNIT_EXPECT_EQ(test,
					FIELD_GET(GENMASK(13, 9), plan.before.value[2]),
					min(dcm, minimum / 70000000));
				KUNIT_EXPECT_EQ(test,
					FIELD_GET(GENMASK(13, 9), plan.after.value[2]),
					rates[j] / 70);
				KUNIT_EXPECT_EQ(test,
					(plan.clkdiv ^ snapshot.clkdiv) & ~GENMASK(9, 5), 0U);
				selector = divs[j] == 4 ? 11 : divs[j] == 2 ? 10 : 8;
				KUNIT_EXPECT_EQ(test,
					FIELD_GET(GENMASK(9, 5), plan.clkdiv), selector);
				KUNIT_EXPECT_EQ(test, (plan.after.value[2] ^ snapshot.sync_dcm) &
					~(GENMASK(13, 9) | BIT(14)), 0U);
				KUNIT_EXPECT_FALSE(test, plan.before.value[1] & BIT(14));
				KUNIT_EXPECT_TRUE(test, plan.before.value[2] & BIT(14));
				KUNIT_EXPECT_FALSE(test, plan.after.value[1] & BIT(14));
				KUNIT_EXPECT_TRUE(test, plan.after.value[2] & BIT(14));
				KUNIT_EXPECT_TRUE(test,
					mt6797_cpuclk_snapshots_equal(&snapshot, &plan.snapshot));
			}
		}
	}
}

static void mt6797_cpuclk_transition_admission_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_boot_snapshot();
	struct mt6797_cpuclk_transition_plan plan;

	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 26000000, 1118000000, &plan), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 962000000, &plan), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 481000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP2,
		&snapshot, 26000000, 750000000, &plan), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_CCI,
		&snapshot, 26000000, 630000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 0, 481000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		NULL, 26000000, 481000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 26000000, 481000000, NULL), -EINVAL);
	KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 1092000000, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.rate, 1092000000UL);
	KUNIT_EXPECT_EQ(test, plan.clkdiv, snapshot.clkdiv);
	KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 26000000, 624000000, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.clkdiv, snapshot.clkdiv);
	snapshot.clkdiv = 9 << 5; /* Fractional CKDIV is observation-only. */
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 26000000, 481000000, &plan), -EOPNOTSUPP);
	snapshot.clkdiv = 0;
	snapshot.muxsel &= ~GENMASK(3, 2);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition(MT6797_CPUCLK_MP0,
		&snapshot, 26000000, 481000000, &plan), -EOPNOTSUPP);
}

static void mt6797_cpuclk_postdivider_pairs_test(struct kunit *test)
{
	static const unsigned int rates[] = { 325, 468, 650, 832, 962, 1092, 1209 };
	static const unsigned int divs[] = { 2, 2, 1, 1, 1, 1, 1 };
	static const unsigned int postdivs[] = { 1, 1, 1, 1, 1, 0, 0 };
	struct mt6797_cpuclk_snapshot snapshot;
	struct mt6797_cpuclk_transition_plan plan;
	unsigned long old_pll, new_pll, minimum, maximum, stages[5];
	unsigned int i, j, k, dcm, selector, count, hop_div;

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		for (j = 0; j < ARRAY_SIZE(rates); j++) {
			for (dcm = 0; dcm < 32; dcm++) {
				snapshot = mt6797_cpuclk_boot_snapshot();
				old_pll = rates[i] * divs[i] * 1000000UL;
				new_pll = rates[j] * divs[j] * 1000000UL;
				snapshot.armpll_con1[1] = BIT(31) | (postdivs[i] << 24) |
					((u64)old_pll * (1U << (14 + postdivs[i])) / 26000000);
				selector = divs[i] == 2 ? 10 : 8;
				snapshot.clkdiv = BIT(31) | (8 << 15) | (selector << 10) |
					(10 << 5) | 8;
				snapshot.sync_dcm = (snapshot.sync_dcm & ~GENMASK(21, 17)) |
					(dcm << 17);
				KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(
					MT6797_CPUCLK_MP1, &snapshot, 26000000,
					rates[j] * 1000000UL, 1092000000, &plan), 0);
				KUNIT_EXPECT_EQ(test, plan.changes_postdiv,
					postdivs[i] != postdivs[j]);
				KUNIT_EXPECT_EQ(test, plan.initial_rate, rates[i] * 1000000ULL);
				KUNIT_EXPECT_EQ(test, plan.rate, rates[j] * 1000000UL);
				KUNIT_EXPECT_EQ(test, plan.pll_rate, new_pll);
				KUNIT_EXPECT_EQ(test, FIELD_GET(GENMASK(26, 24), plan.target_con1),
					postdivs[j]);
				KUNIT_EXPECT_EQ(test,
					(u64)plan.dds * 26000000 >> (14 + postdivs[j]),
					(u64)new_pll);
				stages[0] = rates[i] * 1000000UL;
				stages[4] = rates[j] * 1000000UL;
				if (plan.changes_postdiv) {
					stages[1] = old_pll / 4;
					stages[2] = 1092000000 / 4;
					stages[3] = new_pll / 4;
					KUNIT_EXPECT_EQ(test,
						FIELD_GET(GENMASK(14, 10), plan.temporary_clkdiv),
						11U);
					KUNIT_EXPECT_EQ(test,
						FIELD_GET(GENMASK(5, 4), plan.mainpll_mux), 2U);
					KUNIT_EXPECT_EQ(test, plan.mux_mask, (u32)GENMASK(5, 4));
					KUNIT_EXPECT_EQ(test,
						(plan.mainpll_mux ^ snapshot.muxsel) &
						~plan.mux_mask, 0U);
					count = 5;
				} else {
					hop_div = divs[i] > divs[j] ? divs[i] : divs[j];
					stages[1] = old_pll / hop_div;
					stages[2] = new_pll / hop_div;
					stages[3] = stages[4];
					count = 4;
				}
				minimum = stages[0];
				maximum = (rates[i] > rates[j] ? rates[i] : rates[j]) * 1000000UL;
				for (k = 0; k < count; k++) {
					minimum = min(minimum, stages[k]);
					KUNIT_EXPECT_LE(test, stages[k], maximum);
				}
				KUNIT_EXPECT_EQ(test, plan.minimum_rate, minimum);
				KUNIT_EXPECT_EQ(test,
					FIELD_GET(GENMASK(21, 17), plan.before.value[2]),
					min(dcm, minimum / 70000000));
				KUNIT_EXPECT_EQ(test,
					FIELD_GET(GENMASK(21, 17), plan.after.value[2]),
					rates[j] / 70);
				KUNIT_EXPECT_EQ(test,
					(plan.clkdiv ^ snapshot.clkdiv) & ~GENMASK(14, 10), 0U);
				KUNIT_EXPECT_EQ(test,
					(plan.temporary_clkdiv ^ snapshot.clkdiv) &
					~GENMASK(14, 10), 0U);
			}
		}
	}
}

static void mt6797_cpuclk_postdivider_admission_test(struct kunit *test)
{
	struct mt6797_cpuclk_snapshot snapshot = mt6797_cpuclk_boot_snapshot();
	struct mt6797_cpuclk_transition_plan plan;

	/* Zero fallback rate must retain the old postdivider prohibition. */
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 325000000, 0, &plan), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 325000000, 3, &plan), -ERANGE);
	/* A fallback that is too fast must not depend on extra supply voltage. */
	snapshot.armpll_con1[1] = BIT(31) | 0x64000; /* 650MHz, POS=1 */
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 325000000, 3000000000UL, &plan), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 326000000, 1092000000, &plan), -EINVAL);
	snapshot.muxsel &= ~GENMASK(5, 4);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_transition_mainpll(MT6797_CPUCLK_MP1,
		&snapshot, 26000000, 325000000, 1092000000, &plan), -EOPNOTSUPP);
}

static void mt6797_cpuclk_mp2_pll_plan_test(struct kunit *test)
{
	static const struct {
		u32 mhz, postdiv, pcw;
	} cases[] = {
		{ 250, 2, 0x26762762 }, { 500, 2, 0x4cec4ec4 },
		{ 501, 1, 0x2689d89d }, { 750, 1, 0x39b13b13 },
		{ 1000, 1, 0x4cec4ec4 }, { 1001, 0, 0x26800000 },
		{ 2587, 0, 0x63800000 }, { 3000, 0, 0x73627627 },
	};
	struct mt6797_cpuclk_snapshot initial, preserved;
	struct mt6797_cpuclk_mp2_pll_plan plan, sentinel;
	struct mt6797_cpuclk_state state;
	unsigned int i, bit, mhz;

	memset(&initial, 0xa5, sizeof(initial));
	initial.muxsel = (initial.muxsel & ~3U) | MT6797_CPUCLK_ARMPLL;
	initial.clkdiv = (initial.clkdiv & ~31U) | 8;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		for (bit = 0; bit < 32; bit++) {
			initial.mp2_armpll_con0 = BIT(bit);
			KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
				26000000, cases[i].mhz * 1000000UL, &plan), 0);
			KUNIT_EXPECT_EQ(test, plan.firmware_mhz, cases[i].mhz);
			KUNIT_EXPECT_EQ(test, plan.expected.mp2_armpll_con1, cases[i].pcw);
			KUNIT_EXPECT_EQ(test, plan.expected.mp2_armpll_con0,
				(initial.mp2_armpll_con0 & ~GENMASK(14, 12)) |
				(cases[i].postdiv << 12) | 1);
			preserved = plan.expected;
			preserved.mp2_armpll_con0 = initial.mp2_armpll_con0;
			preserved.mp2_armpll_con1 = initial.mp2_armpll_con1;
			KUNIT_EXPECT_TRUE(test, mt6797_cpuclk_snapshots_equal(&initial,
				&preserved));
		}
	}
	for (mhz = 250; mhz <= 3000; mhz++) {
		KUNIT_ASSERT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
			26000000, mhz * 1000000UL, &plan), 0);
		KUNIT_ASSERT_EQ(test, mt6797_cpuclk_decode_state(MT6797_CPUCLK_MP2,
			&plan.expected, 26000000, &state), 0);
		KUNIT_EXPECT_EQ(test, state.armpll_rate, plan.actual_rate);
		KUNIT_EXPECT_LE(test, plan.actual_rate, mhz * 1000000UL);
		KUNIT_EXPECT_LE(test, mhz * 1000000UL - plan.actual_rate, 2);
	}
	memset(&plan, 0x5a, sizeof(plan));
	sentinel = plan;
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(NULL,
		26000000, 750000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
		26000000, 750000000, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
		25000000, 750000000, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
		26000000, 750000001, &plan), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
		26000000, 249000000, &plan), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_cpuclk_plan_mp2_pll(&initial,
		26000000, 3001000000UL, &plan), -ERANGE);
	KUNIT_EXPECT_EQ(test, memcmp(&plan, &sentinel, sizeof(plan)), 0);
}

static struct kunit_case mt6797_cpuclk_test_cases[] = {
	KUNIT_CASE(mt6797_cpuclk_mp2_pll_plan_test),
	KUNIT_CASE(mt6797_cpuclk_postdivider_pairs_test),
	KUNIT_CASE(mt6797_cpuclk_postdivider_admission_test),
	KUNIT_CASE(mt6797_cpuclk_transition_pairs_test),
	KUNIT_CASE(mt6797_cpuclk_transition_admission_test),
	KUNIT_CASE(mt6797_cpuclk_read_fence_test),
	KUNIT_CASE(mt6797_cpuclk_snapshot_equality_test),
	KUNIT_CASE(mt6797_cpuclk_decode_fields_test),
	KUNIT_CASE(mt6797_cpuclk_pll_formats_test),
	KUNIT_CASE(mt6797_cpuclk_divider_test),
	KUNIT_CASE(mt6797_cpuclk_cold_mp2_test),
	KUNIT_CASE(mt6797_cpuclk_cold_admission_test),
	KUNIT_CASE(mt6797_cpuclk_stopped_mp2_test),
	KUNIT_CASE(mt6797_cpuclk_decode_rejects_invalid_test),
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
