// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free ordering tests for the MT6797 MD1 pre-release core. */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-prerelease.h>
#include <kunit/test.h>

enum mt6797_md1_test_op {
	MD1_TEST_NONE,
	MD1_TEST_PMIC_ON,
	MD1_TEST_DOMAIN_ON,
	MD1_TEST_POLL,
	MD1_TEST_DOMAIN_OFF,
	MD1_TEST_PMIC_OFF,
};

struct mt6797_md1_test_backend {
	enum mt6797_md1_test_op calls[16];
	u32 count;
	u32 pending;
	int fail;
};

static int mt6797_md1_test_record(struct mt6797_md1_test_backend *backend,
		enum mt6797_md1_test_op op)
{
	backend->calls[backend->count++] = op;
	return backend->fail == op ? -EIO : 0;
}

static int mt6797_md1_test_pmic_on(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_PMIC_ON);
}

static int mt6797_md1_test_pmic_off(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_PMIC_OFF);
}

static int mt6797_md1_test_domain_on(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_DOMAIN_ON);
}

static int mt6797_md1_test_domain_off(void *context)
{
	return mt6797_md1_test_record(context, MD1_TEST_DOMAIN_OFF);
}

static int mt6797_md1_test_poll(void *context)
{
	struct mt6797_md1_test_backend *backend = context;
	int ret;

	ret = mt6797_md1_test_record(backend, MD1_TEST_POLL);
	if (ret)
		return ret;
	if (backend->pending) {
		backend->pending--;
		return -EAGAIN;
	}
	return 0;
}

static const struct mt6797_md1_prerelease_ops mt6797_md1_test_ops = {
	.abi = MT6797_MD1_PRERELEASE_ABI,
	.pmic_on = mt6797_md1_test_pmic_on,
	.pmic_off = mt6797_md1_test_pmic_off,
	.domain_on = mt6797_md1_test_domain_on,
	.domain_off = mt6797_md1_test_domain_off,
	.poll = mt6797_md1_test_poll,
};

static void mt6797_md1_test_expect_calls(struct kunit *test,
		struct mt6797_md1_test_backend *backend,
		const enum mt6797_md1_test_op *expected, u32 expected_count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->count, expected_count);
	for (i = 0; i < expected_count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_md1_test_success_unwinds(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_DOMAIN_ON, MD1_TEST_POLL,
		MD1_TEST_DOMAIN_OFF, MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_timeout_is_bounded_and_unwinds(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_DOMAIN_ON,
		MD1_TEST_POLL, MD1_TEST_POLL, MD1_TEST_POLL,
		MD1_TEST_DOMAIN_OFF, MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { .pending = 3 };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_failed_domain_setup_is_unwound(struct kunit *test)
{
	static const enum mt6797_md1_test_op expected[] = {
		MD1_TEST_PMIC_ON, MD1_TEST_DOMAIN_ON,
		MD1_TEST_DOMAIN_OFF, MD1_TEST_PMIC_OFF,
	};
	struct mt6797_md1_test_backend backend = { .fail = MD1_TEST_DOMAIN_ON };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_OFF);
	mt6797_md1_test_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_test_cleanup_failure_is_sticky(struct kunit *test)
{
	struct mt6797_md1_test_backend backend = { .fail = MD1_TEST_DOMAIN_OFF };
	struct mt6797_md1_prerelease prerelease;

	KUNIT_ASSERT_EQ(test, mt6797_md1_prerelease_init(&prerelease,
						 &mt6797_md1_test_ops, &backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_state(&prerelease),
			MT6797_MD1_PRERELEASE_ERROR);
	KUNIT_EXPECT_EQ(test, mt6797_md1_prerelease_run(&prerelease), -EIO);
}

static struct kunit_case mt6797_md1_prerelease_test_cases[] = {
	KUNIT_CASE(mt6797_md1_test_success_unwinds),
	KUNIT_CASE(mt6797_md1_test_timeout_is_bounded_and_unwinds),
	KUNIT_CASE(mt6797_md1_test_failed_domain_setup_is_unwound),
	KUNIT_CASE(mt6797_md1_test_cleanup_failure_is_sticky),
	{ }
};

static struct kunit_suite mt6797_md1_prerelease_test_suite = {
	.name = "mt6797-md1-prerelease",
	.test_cases = mt6797_md1_prerelease_test_cases,
};
kunit_test_suite(mt6797_md1_prerelease_test_suite);

MODULE_LICENSE("GPL");
