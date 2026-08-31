// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitfield.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-state.h>

#define MT6797_TEST_PCW_1001MHZ	0x0009a000U

static void mt6797_clock_test_input(
	struct mt6797_dvfsp_clock_readback *clock,
	struct mt6797_bigidvfs_readback *big)
{
	*clock = (struct mt6797_dvfsp_clock_readback) {
		.abi = MT6797_DVFSP_CLOCK_BACKEND_ABI,
		.sample_generation = 1,
		.armplldiv_muxsel =
			FIELD_PREP(GENMASK(1, 0),
				   MT6797_DVFSP_CLOCK_STATE_MUX_ARMPLL) |
			FIELD_PREP(GENMASK(3, 2),
				   MT6797_DVFSP_CLOCK_STATE_MUX_ARMPLL) |
			FIELD_PREP(GENMASK(5, 4),
				   MT6797_DVFSP_CLOCK_STATE_MUX_ARMPLL) |
			FIELD_PREP(GENMASK(7, 6),
				   MT6797_DVFSP_CLOCK_STATE_MUX_ARMPLL),
		.armplldiv_ckdiv = FIELD_PREP(GENMASK(4, 0), 8) |
			FIELD_PREP(GENMASK(9, 5), 8) |
			FIELD_PREP(GENMASK(14, 10), 8) |
			FIELD_PREP(GENMASK(19, 15), 8),
		.pll_ll = { 0, MT6797_TEST_PCW_1001MHZ, 0 },
		.pll_l = { 0, MT6797_TEST_PCW_1001MHZ, 0 },
		.pll_cci = { 0, MT6797_TEST_PCW_1001MHZ, 0 },
	};
	*big = (struct mt6797_bigidvfs_readback) {
		.abi = MT6797_BIGIDVFS_BACKEND_ABI,
		.sample_generation = 1,
		.pll_pcw = MT6797_TEST_PCW_1001MHZ,
	};
}

static void mt6797_clock_decode_unity_encodings(struct kunit *test)
{
	static const u32 selector[] = { 8, 16, 24 };
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(selector); i++) {
		mt6797_clock_test_input(&clock, &big);
		clock.armplldiv_ckdiv &= ~GENMASK(9, 5);
		clock.armplldiv_ckdiv |= FIELD_PREP(GENMASK(9, 5), selector[i]);

		KUNIT_ASSERT_EQ(test, mt6797_dvfsp_clock_state_decode(
			&clock, &big, &state), 0);
		KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_LL].frequency_khz,
			1001000U);
	}
}

static void mt6797_clock_decode_posdiv_and_ckdiv(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_test_input(&clock, &big);
	clock.pll_l[1] |= FIELD_PREP(MT6797_DVFSP_CLOCK_STATE_POSDIV_MASK, 1);
	clock.armplldiv_ckdiv &= ~GENMASK(14, 10);
	clock.armplldiv_ckdiv |= FIELD_PREP(GENMASK(14, 10), 10);

	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_clock_state_decode(
		&clock, &big, &state), 0);
	KUNIT_EXPECT_EQ(test,
		state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_L].frequency_khz,
		250250U);
}

static void mt6797_clock_decode_rejects_temporary_mux(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_test_input(&clock, &big);
	clock.armplldiv_muxsel &= ~GENMASK(3, 2);
	clock.armplldiv_muxsel |= FIELD_PREP(GENMASK(3, 2),
					 MT6797_DVFSP_CLOCK_STATE_MUX_MAINPLL);

	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_clock_state_decode(
		&clock, &big, &state), -EAGAIN);
}

static struct kunit_case mt6797_clock_state_cases[] = {
	KUNIT_CASE(mt6797_clock_decode_unity_encodings),
	KUNIT_CASE(mt6797_clock_decode_posdiv_and_ckdiv),
	KUNIT_CASE(mt6797_clock_decode_rejects_temporary_mux),
	{}
};

static struct kunit_suite mt6797_clock_state_suite = {
	.name = "mt6797-dvfsp-clock-state",
	.test_cases = mt6797_clock_state_cases,
};

kunit_test_suite(mt6797_clock_state_suite);

MODULE_DESCRIPTION("KUnit tests for MT6797 protected clock-state decoding");
MODULE_LICENSE("GPL");
