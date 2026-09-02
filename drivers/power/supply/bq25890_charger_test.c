// SPDX-License-Identifier: GPL-2.0-or-later
/* Hardware-free tests for BQ25890 firmware-setting adoption. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "bq25890_charger_internal.h"

static void bq25890_mode_flags_are_unambiguous(struct kunit *test)
{
	enum bq25890_control_mode mode = BQ25890_CONTROL_NORMAL;

	KUNIT_ASSERT_EQ(test,
			bq25890_control_mode_from_flags(false, false, &mode), 0);
	KUNIT_EXPECT_EQ(test, mode, BQ25890_CONTROL_NORMAL);

	KUNIT_ASSERT_EQ(test,
			bq25890_control_mode_from_flags(true, false, &mode), 0);
	KUNIT_EXPECT_EQ(test, mode, BQ25890_CONTROL_READ_ONLY);

	KUNIT_ASSERT_EQ(test,
			bq25890_control_mode_from_flags(false, true, &mode), 0);
	KUNIT_EXPECT_EQ(test, mode, BQ25890_CONTROL_ADOPT_FIRMWARE);

	KUNIT_EXPECT_EQ(test,
			bq25890_control_mode_from_flags(true, true, &mode), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			bq25890_control_mode_from_flags(false, false, NULL), -EINVAL);
}

static void bq25890_locked_modes_gate_register_writes(struct kunit *test)
{
	enum bq25890_control_mode normal = BQ25890_CONTROL_NORMAL;
	enum bq25890_control_mode read_only = BQ25890_CONTROL_READ_ONLY;
	enum bq25890_control_mode adopt = BQ25890_CONTROL_ADOPT_FIRMWARE;
	bool allowed;

	KUNIT_EXPECT_FALSE(test, bq25890_control_mode_is_locked(normal));
	KUNIT_EXPECT_TRUE(test, bq25890_control_mode_is_locked(read_only));
	KUNIT_EXPECT_TRUE(test, bq25890_control_mode_is_locked(adopt));

	allowed = bq25890_control_mode_allows_write(normal, false);
	KUNIT_EXPECT_TRUE(test, allowed);
	allowed = bq25890_control_mode_allows_write(normal, true);
	KUNIT_EXPECT_TRUE(test, allowed);
	allowed = bq25890_control_mode_allows_write(read_only, false);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = bq25890_control_mode_allows_write(read_only, true);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = bq25890_control_mode_allows_write(adopt, false);
	KUNIT_EXPECT_FALSE(test, allowed);
	allowed = bq25890_control_mode_allows_write(adopt, true);
	KUNIT_EXPECT_TRUE(test, allowed);
}

static void bq25890_watchdog_cadence_has_fourfold_margin(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, bq25890_adopt_watchdog_delay_ms(0), 0U);
	KUNIT_EXPECT_EQ(test, bq25890_adopt_watchdog_delay_ms(1), 10000U);
	KUNIT_EXPECT_EQ(test, bq25890_adopt_watchdog_delay_ms(2), 20000U);
	KUNIT_EXPECT_EQ(test, bq25890_adopt_watchdog_delay_ms(3), 40000U);
	KUNIT_EXPECT_EQ(test, bq25890_adopt_watchdog_delay_ms(4), 0U);
}

static struct kunit_case bq25890_adopt_test_cases[] = {
	KUNIT_CASE(bq25890_mode_flags_are_unambiguous),
	KUNIT_CASE(bq25890_locked_modes_gate_register_writes),
	KUNIT_CASE(bq25890_watchdog_cadence_has_fourfold_margin),
	{ }
};

static struct kunit_suite bq25890_adopt_test_suite = {
	.name = "bq25890-adopt-firmware",
	.test_cases = bq25890_adopt_test_cases,
};

kunit_test_suite(bq25890_adopt_test_suite);

MODULE_LICENSE("GPL");
