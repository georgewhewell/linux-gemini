// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bits.h>

#include "power.h"

static void mt6797_md1_power_off_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_decode(0, 0),
			MT6797_MD1_POWER_OFF);
}

static void mt6797_md1_powered_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_decode(BIT(0), BIT(0)),
			MT6797_MD1_POWERED);
}

static void mt6797_md1_power_transition_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_decode(BIT(0), 0),
			MT6797_MD1_POWER_UNSTABLE);
	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_decode(0, BIT(0)),
			MT6797_MD1_POWER_UNSTABLE);
}

static void mt6797_md1_power_ignores_other_domains_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_power_state_decode(GENMASK(31, 1),
						      GENMASK(31, 1)),
			MT6797_MD1_POWER_OFF);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_power_state_decode(~0U, ~0U),
			MT6797_MD1_POWERED);
}

static struct kunit_case mt6797_md1_power_test_cases[] = {
	KUNIT_CASE(mt6797_md1_power_off_test),
	KUNIT_CASE(mt6797_md1_powered_test),
	KUNIT_CASE(mt6797_md1_power_transition_test),
	KUNIT_CASE(mt6797_md1_power_ignores_other_domains_test),
	{}
};

static struct kunit_suite mt6797_md1_power_test_suite = {
	.name = "mt6797-md1-power",
	.test_cases = mt6797_md1_power_test_cases,
};

kunit_test_suite(mt6797_md1_power_test_suite);
