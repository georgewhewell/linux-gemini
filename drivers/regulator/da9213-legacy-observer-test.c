// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the legacy DA921x read-only observer. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "da9213-legacy-observer.h"

struct da9213_legacy_observer_fake {
	int fail_at;
	unsigned int calls;
	unsigned int values[4];
};

static int da9213_legacy_observer_fake_read(
	void *context, unsigned int buck,
	enum da9213_legacy_observer_field field, unsigned int *value)
{
	struct da9213_legacy_observer_fake *fake = context;
	unsigned int expected_buck = fake->calls / 2;
	enum da9213_legacy_observer_field expected_field =
		fake->calls % 2 ? DA9213_LEGACY_OBSERVER_ENABLED :
				  DA9213_LEGACY_OBSERVER_SELECTOR;
	unsigned int call = fake->calls++;

	if (buck != expected_buck || field != expected_field)
		return -EINVAL;
	if (call == fake->fail_at)
		return -EIO;

	*value = fake->values[call];
	return 0;
}

static struct da9213_legacy_observer_fake
da9213_legacy_observer_valid_fake(void)
{
	struct da9213_legacy_observer_fake fake = {
		.fail_at = -1,
		.values = { 42, 1, 70, 0 },
	};

	return fake;
}

static void da9213_legacy_observer_records_both_bucks(struct kunit *test)
{
	struct da9213_legacy_observer_fake fake =
		da9213_legacy_observer_valid_fake();
	struct da9213_legacy_observation observation;
	int ret;

	ret = da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 14, 2,
		&observation);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, observation.valid);
	KUNIT_EXPECT_EQ(test, observation.identity_reads, 14U);
	KUNIT_EXPECT_EQ(test, observation.provider_count, 2U);
	KUNIT_EXPECT_EQ(test, observation.provider_read_attempts, 4U);
	KUNIT_EXPECT_EQ(test, observation.provider_read_completed, 4U);
	KUNIT_EXPECT_EQ(test, observation.register_data_writes, 0U);
	KUNIT_EXPECT_EQ(test, observation.buck[0].selector, 42U);
	KUNIT_EXPECT_EQ(test, observation.buck[0].microvolts, 720000U);
	KUNIT_EXPECT_EQ(test, observation.buck[0].enabled, 1U);
	KUNIT_EXPECT_EQ(test, observation.buck[1].selector, 70U);
	KUNIT_EXPECT_EQ(test, observation.buck[1].microvolts, 1000000U);
	KUNIT_EXPECT_EQ(test, observation.buck[1].enabled, 0U);
}

static void da9213_legacy_observer_bounds_read_failures(struct kunit *test)
{
	unsigned int fail_at;

	for (fail_at = 0; fail_at < 4; fail_at++) {
		struct da9213_legacy_observer_fake fake =
			da9213_legacy_observer_valid_fake();
		struct da9213_legacy_observation observation;
		int ret;

		fake.fail_at = fail_at;
		ret = da9213_legacy_observer_collect(
			da9213_legacy_observer_fake_read, &fake, 14, 2,
			&observation);

		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_FALSE(test, observation.valid);
		KUNIT_EXPECT_EQ(test, observation.provider_read_attempts,
				fail_at + 1);
		KUNIT_EXPECT_EQ(test, observation.provider_read_completed,
				fail_at);
		KUNIT_EXPECT_EQ(test, observation.register_data_writes, 0U);
	}
}

static void da9213_legacy_observer_rejects_incomplete_state(
	struct kunit *test)
{
	struct da9213_legacy_observer_fake fake =
		da9213_legacy_observer_valid_fake();
	struct da9213_legacy_observation observation;

	KUNIT_EXPECT_EQ(test, da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 13, 2,
		&observation), -EINVAL);
	KUNIT_EXPECT_EQ(test, fake.calls, 0U);
	KUNIT_EXPECT_FALSE(test, observation.valid);

	KUNIT_EXPECT_EQ(test, da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 14, 1,
		&observation), -EINVAL);
	KUNIT_EXPECT_EQ(test, fake.calls, 0U);
	KUNIT_EXPECT_FALSE(test, observation.valid);
}

static void da9213_legacy_observer_rejects_invalid_values(
	struct kunit *test)
{
	struct da9213_legacy_observer_fake fake =
		da9213_legacy_observer_valid_fake();
	struct da9213_legacy_observation observation;

	fake.values[0] = DA9213_LEGACY_VSEL_MASK + 1;
	KUNIT_EXPECT_EQ(test, da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 14, 2,
		&observation), -ERANGE);
	KUNIT_EXPECT_FALSE(test, observation.valid);
	KUNIT_EXPECT_EQ(test, observation.provider_read_attempts, 1U);
	KUNIT_EXPECT_EQ(test, observation.provider_read_completed, 1U);

	fake = da9213_legacy_observer_valid_fake();
	fake.values[1] = 2;
	KUNIT_EXPECT_EQ(test, da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 14, 2,
		&observation), -ERANGE);
	KUNIT_EXPECT_FALSE(test, observation.valid);
	KUNIT_EXPECT_EQ(test, observation.provider_read_attempts, 2U);
	KUNIT_EXPECT_EQ(test, observation.provider_read_completed, 2U);
	KUNIT_EXPECT_EQ(test, observation.register_data_writes, 0U);
}

static void da9213_legacy_observer_invalidates_on_cleanup(
	struct kunit *test)
{
	struct da9213_legacy_observer_fake fake =
		da9213_legacy_observer_valid_fake();
	struct da9213_legacy_observation observation;
	unsigned int provider_count = 2;

	KUNIT_ASSERT_EQ(test, da9213_legacy_observer_collect(
		da9213_legacy_observer_fake_read, &fake, 14, 2,
		&observation), 0);
	da9213_legacy_observer_cleanup_state(&observation, &provider_count);
	KUNIT_EXPECT_FALSE(test, observation.valid);
	KUNIT_EXPECT_EQ(test, observation.provider_count, 0U);
	KUNIT_EXPECT_EQ(test, provider_count, 0U);
	KUNIT_EXPECT_EQ(test, observation.register_data_writes, 0U);
}

static struct kunit_case da9213_legacy_observer_test_cases[] = {
	KUNIT_CASE(da9213_legacy_observer_records_both_bucks),
	KUNIT_CASE(da9213_legacy_observer_bounds_read_failures),
	KUNIT_CASE(da9213_legacy_observer_rejects_incomplete_state),
	KUNIT_CASE(da9213_legacy_observer_rejects_invalid_values),
	KUNIT_CASE(da9213_legacy_observer_invalidates_on_cleanup),
	{ }
};

static struct kunit_suite da9213_legacy_observer_test_suite = {
	.name = "da9213-legacy-observer",
	.test_cases = da9213_legacy_observer_test_cases,
};

kunit_test_suite(da9213_legacy_observer_test_suite);

MODULE_LICENSE("GPL");
