// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free tests for the MT6797 single-owner CSPM access gate.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "mt6797-dvfsp-cspm-access.h"

struct mt6797_dvfsp_cspm_test_context {
	struct kunit *test;
	struct mt6797_dvfsp_cspm_access *access;
	void __iomem *expected_base;
	unsigned int calls;
	int result;
};

static int mt6797_dvfsp_cspm_test_operation(void __iomem *base, void *context)
{
	struct mt6797_dvfsp_cspm_test_context *ctx = context;

	ctx->calls++;
	KUNIT_EXPECT_PTR_EQ(ctx->test, base, ctx->expected_base);
	/* A competing CSPM/lifecycle user cannot enter during the callback. */
	KUNIT_EXPECT_FALSE(ctx->test, mutex_trylock(&ctx->access->lock));
	return ctx->result;
}

static void mt6797_dvfsp_cspm_blocks_until_owner_ready(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_test_context ctx = {
		.test = test,
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
	};

	mt6797_dvfsp_cspm_access_init(&access);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx),
			-EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 0U);

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_READY),
		0);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx),
			0);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1U);
}

static void mt6797_dvfsp_cspm_owner_failure_is_sticky(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_test_context ctx = {
		.test = test,
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
	};

	mt6797_dvfsp_cspm_access_init(&access);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_READY),
		0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_FAULTED),
		0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_READY),
		-EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_cspm_access_read(&access),
			MT6797_DVFSP_PERMISSION_FAULTED);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx),
			-EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 0U);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_cmpxchg(
				&access, MT6797_DVFSP_PERMISSION_BLOCKED,
				MT6797_DVFSP_PERMISSION_READY),
			MT6797_DVFSP_PERMISSION_FAULTED);
}

static void mt6797_dvfsp_cspm_serializes_and_unlocks(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_test_context ctx = {
		.test = test,
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
		.result = -EAGAIN,
	};

	mt6797_dvfsp_cspm_access_init(&access);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_READY),
		0);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1U);
	KUNIT_ASSERT_TRUE(test, mutex_trylock(&access.lock));
	mutex_unlock(&access.lock);
}

static void mt6797_dvfsp_cspm_rejects_invalid_requests(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_test_context ctx = {
		.test = test,
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
	};

	mt6797_dvfsp_cspm_access_init(&access);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				NULL, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, NULL, mt6797_dvfsp_cspm_test_operation,
				&ctx),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base, NULL, &ctx),
			-EINVAL);
}

static struct kunit_case mt6797_dvfsp_cspm_access_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_cspm_blocks_until_owner_ready),
	KUNIT_CASE(mt6797_dvfsp_cspm_owner_failure_is_sticky),
	KUNIT_CASE(mt6797_dvfsp_cspm_serializes_and_unlocks),
	KUNIT_CASE(mt6797_dvfsp_cspm_rejects_invalid_requests),
	{}
};

static struct kunit_suite mt6797_dvfsp_cspm_access_test_suite = {
	.name = "mt6797-dvfsp-cspm-access",
	.test_cases = mt6797_dvfsp_cspm_access_test_cases,
};

kunit_test_suite(mt6797_dvfsp_cspm_access_test_suite);

MODULE_LICENSE("GPL");
