// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for direct-address DA921x register transactions. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "da9213-legacy-transaction.h"

struct da9213_legacy_fake {
	u8 value;
	unsigned int reads;
	unsigned int writes;
	int fail_read_at;
	int fail_write_at;
	bool ignore_first_write;
};

static int da9213_legacy_fake_read(void *context, u8 reg, u8 *value)
{
	struct da9213_legacy_fake *fake = context;
	unsigned int call = fake->reads++;

	if (reg != 0x5e)
		return -EINVAL;
	if (call == fake->fail_read_at)
		return -EIO;

	*value = fake->value;
	return 0;
}

static int da9213_legacy_fake_write(void *context, u8 reg, u8 value)
{
	struct da9213_legacy_fake *fake = context;
	unsigned int call = fake->writes++;

	if (reg != 0x5e)
		return -EINVAL;
	if (call == fake->fail_write_at)
		return -EREMOTEIO;
	if (fake->ignore_first_write && !call)
		return 0;

	fake->value = value;
	return 0;
}

static const struct da9213_legacy_transaction_io da9213_legacy_fake_io = {
	.read = da9213_legacy_fake_read,
	.write = da9213_legacy_fake_write,
};

static struct da9213_legacy_fake da9213_legacy_new_fake(void)
{
	return (struct da9213_legacy_fake) {
		.value = 0xa0,
		.fail_read_at = -1,
		.fail_write_at = -1,
	};
}

static int da9213_legacy_fake_update(struct da9213_legacy_fake *fake, u8 mask,
				     u8 value)
{
	return da9213_legacy_update_bits_transaction(&da9213_legacy_fake_io,
						     fake, 0x5e, mask, value);
}

static void da9213_legacy_transaction_commits_verified_value(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	ret = da9213_legacy_fake_update(&fake, 0x01, 0x01);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fake.value, (u8)0xa1);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.writes, 1U);
}

static void da9213_legacy_transaction_skips_equal_value(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	ret = da9213_legacy_fake_update(&fake, 0x01, 0x00);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fake.value, (u8)0xa0);
	KUNIT_EXPECT_EQ(test, fake.reads, 1U);
	KUNIT_EXPECT_EQ(test, fake.writes, 0U);
}

static void da9213_legacy_transaction_restores_after_write_error(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	fake.fail_write_at = 0;
	ret = da9213_legacy_fake_update(&fake, 0x01, 0x01);

	KUNIT_EXPECT_EQ(test, ret, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, fake.value, (u8)0xa0);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.writes, 2U);
}

static void da9213_legacy_transaction_restores_after_mismatch(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	fake.ignore_first_write = true;
	ret = da9213_legacy_fake_update(&fake, 0x01, 0x01);

	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, fake.value, (u8)0xa0);
	KUNIT_EXPECT_EQ(test, fake.reads, 3U);
	KUNIT_EXPECT_EQ(test, fake.writes, 2U);
}

static void da9213_legacy_transaction_reports_uncertain_rollback(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	fake.ignore_first_write = true;
	fake.fail_write_at = 1;
	ret = da9213_legacy_fake_update(&fake, 0x01, 0x01);

	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, fake.writes, 2U);
}

static void da9213_legacy_transaction_rejects_bad_masks(struct kunit *test)
{
	struct da9213_legacy_fake fake = da9213_legacy_new_fake();
	int ret;

	ret = da9213_legacy_fake_update(&fake, 0, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = da9213_legacy_fake_update(&fake, 0x01, 0x02);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, fake.reads, 0U);
	KUNIT_EXPECT_EQ(test, fake.writes, 0U);
}

static struct kunit_case da9213_legacy_transaction_test_cases[] = {
	KUNIT_CASE(da9213_legacy_transaction_commits_verified_value),
	KUNIT_CASE(da9213_legacy_transaction_skips_equal_value),
	KUNIT_CASE(da9213_legacy_transaction_restores_after_write_error),
	KUNIT_CASE(da9213_legacy_transaction_restores_after_mismatch),
	KUNIT_CASE(da9213_legacy_transaction_reports_uncertain_rollback),
	KUNIT_CASE(da9213_legacy_transaction_rejects_bad_masks),
	{ }
};

static struct kunit_suite da9213_legacy_transaction_test_suite = {
	.name = "da9213-legacy-transaction",
	.test_cases = da9213_legacy_transaction_test_cases,
};

kunit_test_suite(da9213_legacy_transaction_test_suite);

MODULE_LICENSE("GPL");
