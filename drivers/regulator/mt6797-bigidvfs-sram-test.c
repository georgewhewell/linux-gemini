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

struct mt6797_sram_first_fake {
	struct mt6797_sram_first_state *state;
	struct mt6797_sram_sample samples[2];
	unsigned int sets, waits, reads, argument, delay;
	bool latched_at_set;
	int set_error;
	int fail_read;
};

static int mt6797_sram_first_fake_set(void *context, unsigned int argument)
{
	struct mt6797_sram_first_fake *fake = context;

	fake->sets++;
	fake->argument = argument;
	fake->latched_at_set = fake->state->attempted;
	return fake->set_error;
}

static void mt6797_sram_first_fake_wait(void *context, unsigned int delay)
{
	struct mt6797_sram_first_fake *fake = context;

	fake->waits++;
	fake->delay = delay;
}

static int mt6797_sram_first_fake_read(void *context,
				     struct mt6797_sram_sample *sample)
{
	struct mt6797_sram_first_fake *fake = context;
	unsigned int index = fake->reads++;

	if (fake->sets != 1 || fake->waits != 1 || index > 1)
		return -EPROTO;
	if (index == fake->fail_read)
		return -EIO;
	*sample = fake->samples[index];
	return 0;
}

static const struct mt6797_sram_first_io mt6797_sram_first_fake_io = {
	.set_voltage = mt6797_sram_first_fake_set,
	.settle = mt6797_sram_first_fake_wait,
	.read_sample = mt6797_sram_first_fake_read,
};

static struct mt6797_sram_first_fake
mt6797_sram_first_new_fake(struct mt6797_sram_first_state *state)
{
	return (struct mt6797_sram_first_fake) {
		.state = state,
		.samples = { { 0x4008fb, 0x8967 }, { 0x4008fb, 0x8967 } },
		.fail_read = -1,
	};
}

static void mt6797_sram_first_commits_full_samples(struct kunit *test)
{
	struct mt6797_sram_first_state state = { };
	struct mt6797_sram_first_fake fake = mt6797_sram_first_new_fake(&state);

	KUNIT_ASSERT_EQ(test, mt6797_sram_first_start(&mt6797_sram_first_fake_io,
		&fake, &state), 0);
	KUNIT_EXPECT_TRUE(test, state.ready);
	KUNIT_EXPECT_FALSE(test, state.faulted);
	KUNIT_EXPECT_TRUE(test, fake.latched_at_set);
	KUNIT_EXPECT_EQ(test, fake.sets, 1U);
	KUNIT_EXPECT_EQ(test, fake.argument, 110000U);
	KUNIT_EXPECT_EQ(test, fake.delay, 240U);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
	KUNIT_EXPECT_EQ(test, state.samples[1].control, 0x4008fbU);
	KUNIT_EXPECT_EQ(test, state.samples[1].calibration, 0x8967U);
	KUNIT_EXPECT_EQ(test, mt6797_sram_first_start(&mt6797_sram_first_fake_io,
		&fake, &state), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, fake.sets, 1U);
	KUNIT_EXPECT_EQ(test, fake.reads, 2U);
}

static void mt6797_sram_first_failures_never_retry(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < 10; i++) {
		struct mt6797_sram_first_state state = { };
		struct mt6797_sram_first_fake fake = mt6797_sram_first_new_fake(&state);
		unsigned int reads;
		int expected = -EIO;

		switch (i) {
		case 0: fake.set_error = -EIO; break;
		case 1: fake.set_error = -EOPNOTSUPP; expected = -EOPNOTSUPP; break;
		case 2: fake.fail_read = 0; break;
		case 3: fake.fail_read = 1; break;
		case 4: fake.samples[1].control ^= BIT(22); expected = -EAGAIN; break;
		case 5: fake.samples[1].calibration ^= 1; expected = -EAGAIN; break;
		case 6: fake.samples[0].control = fake.samples[1].control = 0xb; break;
		case 7: fake.samples[0].calibration = fake.samples[1].calibration = 0; break;
		case 8: fake.samples[0].calibration = fake.samples[1].calibration = 0x10001; break;
		case 9: fake.samples[0].control = fake.samples[1].control = 0x8fa; break;
		}
		KUNIT_EXPECT_EQ(test, mt6797_sram_first_start(&mt6797_sram_first_fake_io,
			&fake, &state), -EUCLEAN);
		KUNIT_EXPECT_EQ(test, state.error, expected);
		KUNIT_EXPECT_TRUE(test, state.faulted);
		KUNIT_EXPECT_FALSE(test, state.ready);
		KUNIT_EXPECT_TRUE(test, fake.latched_at_set);
		KUNIT_EXPECT_EQ(test, fake.sets, 1U);
		KUNIT_EXPECT_EQ(test, fake.waits, i < 2 ? 0U : 1U);
		reads = fake.reads;
		KUNIT_EXPECT_EQ(test, reads, i < 2 ? 0U : i == 2 ? 1U : 2U);
		KUNIT_EXPECT_EQ(test, mt6797_sram_first_start(&mt6797_sram_first_fake_io,
			&fake, &state), -EUCLEAN);
		KUNIT_EXPECT_EQ(test, fake.sets, 1U);
		KUNIT_EXPECT_EQ(test, fake.reads, reads);
	}
}

static void mt6797_sram_first_requires_unused_state(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < 3; i++) {
		struct mt6797_sram_first_state state = { };
		struct mt6797_sram_first_fake fake = mt6797_sram_first_new_fake(&state);

		state.attempted = i == 0;
		state.ready = i == 1;
		state.faulted = i == 2;
		KUNIT_EXPECT_EQ(test, mt6797_sram_first_start(&mt6797_sram_first_fake_io,
			&fake, &state), -EUCLEAN);
		KUNIT_EXPECT_EQ(test, fake.sets, 0U);
		KUNIT_EXPECT_EQ(test, fake.reads, 0U);
	}
}

static void mt6797_sram_first_requires_complete_backend(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < 3; i++) {
		struct mt6797_sram_first_state state = { };
		struct mt6797_sram_first_fake fake = mt6797_sram_first_new_fake(&state);
		struct mt6797_sram_first_io io = mt6797_sram_first_fake_io;

		if (i == 0) io.set_voltage = NULL;
		if (i == 1) io.settle = NULL;
		if (i == 2) io.read_sample = NULL;
		KUNIT_EXPECT_EQ(test, mt6797_sram_first_start(&io, &fake, &state), -EINVAL);
		KUNIT_EXPECT_FALSE(test, state.attempted);
		KUNIT_EXPECT_EQ(test, fake.sets, 0U);
	}
}

static struct kunit_case mt6797_bigidvfs_sram_test_cases[] = {
	KUNIT_CASE(mt6797_sram_first_commits_full_samples),
	KUNIT_CASE(mt6797_sram_first_failures_never_retry),
	KUNIT_CASE(mt6797_sram_first_requires_unused_state),
	KUNIT_CASE(mt6797_sram_first_requires_complete_backend),
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
