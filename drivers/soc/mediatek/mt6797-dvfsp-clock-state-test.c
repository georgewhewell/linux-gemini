// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-state.h>

static void mt6797_clock_state_valid(
	struct mt6797_dvfsp_clock_readback *clock,
	struct mt6797_bigidvfs_readback *big)
{
	*clock = (struct mt6797_dvfsp_clock_readback) {
		.abi = MT6797_DVFSP_CLOCK_BACKEND_ABI,
		.sample_generation = 11,
		.armplldiv_muxsel = 0x55,
		.armplldiv_ckdiv = 0x42108,
		.pll_ll = { 0, 0xc1114000, 0 },
		.pll_l = { 0, 0x400c4000, 0 },
		.pll_cci = { 0, 0xc10c1d89, 0 },
	};
	*big = (struct mt6797_bigidvfs_readback) {
		.abi = MT6797_BIGIDVFS_BACKEND_ABI,
		.sample_generation = 13,
		.pll_pcw = 0xc1130000,
		.pll_enable_posdiv = 0x07001000,
	};
}

static void mt6797_clock_state_stable_strobes_test(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_state_valid(&clock, &big);
	KUNIT_ASSERT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state), 0);
	KUNIT_EXPECT_EQ(test, state.clock_sample_generation, 11ULL);
	KUNIT_EXPECT_EQ(test, state.big_sample_generation, 13ULL);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_LL].frequency_khz,
			897000U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_L].frequency_khz,
			1274000U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_CCI].frequency_khz,
			629500U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].pll_pcw,
			0x41130000U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].posdiv,
			1U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].frequency_khz,
			845000U);
}

static void mt6797_clock_state_big_fields_test(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_state_valid(&clock, &big);
	big.pll_pcw = 0xcc000000;
	big.pll_enable_posdiv = 0x07002000;
	clock.armplldiv_ckdiv = (clock.armplldiv_ckdiv & ~GENMASK(4, 0)) | 10;
	KUNIT_ASSERT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state), 0);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].pll_pcw,
			0x4c000000U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].posdiv,
			2U);
	KUNIT_EXPECT_EQ(test,
			state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].frequency_khz,
			247000U);
}

static void mt6797_clock_state_dividers_test(struct kunit *test)
{
	static const u32 selector[] = {
		8, 9, 10, 11, 17, 18, 19, 20, 25, 26, 27, 28, 29,
	};
	static const u32 expected[] = {
		1976000, 1482000, 988000, 494000, 1580800, 1185600,
		790400, 395200, 1646666, 1317333, 988000, 658666, 329333,
	};
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;
	int i;

	for (i = 0; i < ARRAY_SIZE(selector); i++) {
		mt6797_clock_state_valid(&clock, &big);
		big.pll_pcw = 0x4c000000;
		big.pll_enable_posdiv = 0;
		clock.armplldiv_ckdiv =
			(clock.armplldiv_ckdiv & ~GENMASK(4, 0)) | selector[i];
		KUNIT_ASSERT_EQ(test,
				mt6797_dvfsp_clock_state_decode(&clock, &big, &state), 0);
		KUNIT_EXPECT_EQ(test,
				state.cluster[MT6797_DVFSP_CLOCK_STATE_CLUSTER_B].frequency_khz,
				expected[i]);
	}
}

static void mt6797_clock_state_transport_guards_test(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_state_valid(&clock, &big);
	clock.sample_generation = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	big.sample_generation = 0;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	clock.reserved = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	big.reserved = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
}

static void mt6797_clock_state_zero_pcw_test(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_state_valid(&clock, &big);
	clock.pll_ll[1] = MT6797_DVFSP_CLOCK_STATE_PCW_STROBE;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	big.pll_pcw = MT6797_DVFSP_CLOCK_STATE_PCW_STROBE;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
}

static void mt6797_clock_state_encoding_guards_test(struct kunit *test)
{
	struct mt6797_dvfsp_clock_readback clock;
	struct mt6797_bigidvfs_readback big;
	struct mt6797_dvfsp_clock_state state;

	mt6797_clock_state_valid(&clock, &big);
	clock.pll_ll[1] = 0x83114000;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	clock.armplldiv_ckdiv &= ~GENMASK(4, 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-EPROTO);
	mt6797_clock_state_valid(&clock, &big);
	big.pll_pcw = 1;
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_clock_state_decode(&clock, &big, &state),
			-ERANGE);
}

static struct kunit_case mt6797_clock_state_cases[] = {
	KUNIT_CASE(mt6797_clock_state_stable_strobes_test),
	KUNIT_CASE(mt6797_clock_state_big_fields_test),
	KUNIT_CASE(mt6797_clock_state_dividers_test),
	KUNIT_CASE(mt6797_clock_state_transport_guards_test),
	KUNIT_CASE(mt6797_clock_state_zero_pcw_test),
	KUNIT_CASE(mt6797_clock_state_encoding_guards_test),
	{}
};

static struct kunit_suite mt6797_clock_state_suite = {
	.name = "mt6797-dvfsp-clock-state",
	.test_cases = mt6797_clock_state_cases,
};

kunit_test_suite(mt6797_clock_state_suite);

MODULE_LICENSE("GPL");
