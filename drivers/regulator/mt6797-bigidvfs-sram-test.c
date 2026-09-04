// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 secure SRAM regulator transaction. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "mt6797-bigidvfs-sram.h"

static const unsigned int mt6797_bigidvfs_sram_test_voltages[] = {
	1050000, 600000, 700000, 900000,
	925000, 950000, 975000, 1000000,
	1025000, 1050000, 1075000, 1100000,
	1125000, 1150000, 1175000, 1200000,
};

struct mt6797_bigidvfs_sram_fake {
	unsigned int selector;
	unsigned int reads;
	unsigned int sets;
	int fail_read_at;
	int fail_set_at;
	int fail_set_ret;
	bool ignore_first_set;
};

static int mt6797_bigidvfs_sram_fake_read(void *context,
					  unsigned int *selector)
{
	struct mt6797_bigidvfs_sram_fake *fake = context;
	unsigned int call = fake->reads++;

	if (call == fake->fail_read_at)
		return -EIO;

	*selector = fake->selector;
	return 0;
}

static int mt6797_bigidvfs_sram_fake_set(void *context,
					 unsigned int firmware_uv_div10)
{
	struct mt6797_bigidvfs_sram_fake *fake = context;
	unsigned int call = fake->sets++;
	unsigned int selector;

	if (call == fake->fail_set_at)
		return fake->fail_set_ret;
	if (fake->ignore_first_set && !call)
		return 0;

	for (selector = 0;
	     selector < ARRAY_SIZE(mt6797_bigidvfs_sram_test_voltages);
	     selector++) {
		if (mt6797_bigidvfs_sram_test_voltages[selector] / 10 ==
		    firmware_uv_div10) {
			fake->selector = selector;
			return 0;
		}
	}

	return -EINVAL;
}

static const struct mt6797_sram_io mt6797_bigidvfs_sram_fake_io = {
	.read_selector = mt6797_bigidvfs_sram_fake_read,
	.set_voltage = mt6797_bigidvfs_sram_fake_set,
};

static struct mt6797_bigidvfs_sram_fake mt6797_bigidvfs_sram_new_fake(void)
{
	return (struct mt6797_bigidvfs_sram_fake) {
		.selector = 7,
		.fail_read_at = -1,
		.fail_set_at = -1,
		.fail_set_ret = -EIO,
	};
}

static int mt6797_sram_fake_transition(struct mt6797_bigidvfs_sram_fake *fake,
				       unsigned int selector)
{
	return mt6797_sram_set_transaction(&mt6797_bigidvfs_sram_fake_io, fake,
		mt6797_bigidvfs_sram_test_voltages,
		ARRAY_SIZE(mt6797_bigidvfs_sram_test_voltages), selector);
}

static void mt6797_bigidvfs_sram_commits_verified_selector(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fake.selector, 11U);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.sets, 1U);
}

static void mt6797_bigidvfs_sram_skips_equal_selector(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	ret = mt6797_sram_fake_transition(&fake, 7);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, fake.sets, 0U);
}

static void mt6797_bigidvfs_sram_rejects_zero_target_without_io(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	ret = mt6797_sram_fake_transition(&fake, 0);

	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, fake.selector, 7U);
	KUNIT_EXPECT_EQ(test, fake.reads, 0U);
	KUNIT_EXPECT_EQ(test, fake.sets, 0U);
}

static void mt6797_bigidvfs_sram_rejects_zero_initial_without_write(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.selector = 0;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fake.selector, 0U);
	KUNIT_EXPECT_EQ(test, fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, fake.sets, 0U);
}

static void mt6797_bigidvfs_sram_rolls_back_mismatch(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.ignore_first_set = true;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake.selector, 7U);
	KUNIT_EXPECT_EQ(test, fake.reads, 3U);
	KUNIT_EXPECT_EQ(test, fake.sets, 2U);
}

static void mt6797_bigidvfs_sram_rejects_equivalent_selector(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	ret = mt6797_sram_fake_transition(&fake, 9);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake.selector, 7U);
	KUNIT_EXPECT_EQ(test, fake.reads, 3U);
	KUNIT_EXPECT_EQ(test, fake.sets, 2U);
}

static void mt6797_bigidvfs_sram_rolls_back_set_error(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.fail_set_at = 0;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake.selector, 7U);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.sets, 2U);
}

static void mt6797_bigidvfs_sram_propagates_unsupported_set(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.fail_set_at = 0;
	fake.fail_set_ret = -EOPNOTSUPP;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fake.selector, 7U);
	KUNIT_EXPECT_EQ(test, fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, fake.sets, 1U);
}

static void mt6797_bigidvfs_sram_reports_uncertain_rollback(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.ignore_first_set = true;
	fake.fail_set_at = 1;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.sets, 2U);
}

static void mt6797_bigidvfs_sram_propagates_initial_read_error(struct kunit *test)
{
	struct mt6797_bigidvfs_sram_fake fake =
		mt6797_bigidvfs_sram_new_fake();
	int ret;

	fake.fail_read_at = 0;
	ret = mt6797_sram_fake_transition(&fake, 11);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, fake.sets, 0U);
}

static struct kunit_case mt6797_bigidvfs_sram_test_cases[] = {
	KUNIT_CASE(mt6797_bigidvfs_sram_commits_verified_selector),
	KUNIT_CASE(mt6797_bigidvfs_sram_skips_equal_selector),
	KUNIT_CASE(mt6797_bigidvfs_sram_rejects_zero_target_without_io),
	KUNIT_CASE(mt6797_bigidvfs_sram_rejects_zero_initial_without_write),
	KUNIT_CASE(mt6797_bigidvfs_sram_rolls_back_mismatch),
	KUNIT_CASE(mt6797_bigidvfs_sram_rejects_equivalent_selector),
	KUNIT_CASE(mt6797_bigidvfs_sram_rolls_back_set_error),
	KUNIT_CASE(mt6797_bigidvfs_sram_propagates_unsupported_set),
	KUNIT_CASE(mt6797_bigidvfs_sram_reports_uncertain_rollback),
	KUNIT_CASE(mt6797_bigidvfs_sram_propagates_initial_read_error),
	{ }
};

static struct kunit_suite mt6797_bigidvfs_sram_test_suite = {
	.name = "mt6797-bigidvfs-sram",
	.test_cases = mt6797_bigidvfs_sram_test_cases,
};

kunit_test_suite(mt6797_bigidvfs_sram_test_suite);

MODULE_LICENSE("GPL");
