// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free tests for the MT6797 single-owner CSPM access gate.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-dvfsp-handoff.h>

#include "mt6797-dvfsp-cspm-access.h"

struct mt6797_dvfsp_cspm_test_context {
	struct kunit *test;
	struct mt6797_dvfsp_cspm_access *access;
	void __iomem *expected_base;
	unsigned int calls;
	int result;
	unsigned int terminal_calls;
	bool terminal_faulted;
	const char *terminal_reason;
	unsigned int terminal_fault_count;
};

struct mt6797_dvfsp_cspm_concurrent_context {
	struct mt6797_dvfsp_cspm_access *access;
	void __iomem *expected_base;
	atomic_t calls;
	atomic_t terminal_calls;
	atomic_t terminal_saw_faulted;
	struct completion first_entered;
	struct completion release_first;
};

struct mt6797_dvfsp_cspm_thread_context {
	struct mt6797_dvfsp_cspm_concurrent_context *shared;
	struct completion started;
	struct completion done;
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

static void mt6797_dvfsp_cspm_test_terminal(void *context)
{
	struct mt6797_dvfsp_cspm_test_context *ctx = context;

	ctx->terminal_calls++;
	KUNIT_EXPECT_FALSE(ctx->test, mutex_trylock(&ctx->access->lock));
	KUNIT_EXPECT_EQ(ctx->test,
			mt6797_dvfsp_cspm_access_read(ctx->access),
			MT6797_DVFSP_PERMISSION_FAULTED);
	if (!ctx->terminal_faulted) {
		ctx->terminal_faulted = true;
		ctx->terminal_reason = "cspm-transaction-terminal-failure";
		ctx->terminal_fault_count++;
	}
}

static int
mt6797_dvfsp_cspm_blocking_operation(void __iomem *base, void *context)
{
	struct mt6797_dvfsp_cspm_concurrent_context *ctx = context;

	if (base != ctx->expected_base)
		return -EINVAL;
	if (atomic_inc_return(&ctx->calls) != 1)
		return 0;

	complete(&ctx->first_entered);
	wait_for_completion(&ctx->release_first);

	return -EUCLEAN;
}

static void mt6797_dvfsp_cspm_concurrent_terminal(void *context)
{
	struct mt6797_dvfsp_cspm_concurrent_context *ctx = context;

	atomic_inc(&ctx->terminal_calls);
	atomic_set(&ctx->terminal_saw_faulted,
		   mt6797_dvfsp_cspm_access_read(ctx->access) ==
			   MT6797_DVFSP_PERMISSION_FAULTED);
}

static int mt6797_dvfsp_cspm_transaction_thread(void *data)
{
	struct mt6797_dvfsp_cspm_thread_context *ctx = data;

	complete(&ctx->started);
	ctx->result = mt6797_dvfsp_cspm_access_transaction(ctx->shared->access,
			ctx->shared->expected_base,
			mt6797_dvfsp_cspm_blocking_operation, ctx->shared,
			mt6797_dvfsp_cspm_concurrent_terminal, ctx->shared);
	complete(&ctx->done);

	return 0;
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
				mt6797_dvfsp_cspm_test_operation, &ctx, NULL, NULL),
			-EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 0U);

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_cspm_access_set(
			&access, MT6797_DVFSP_PERMISSION_READY),
		0);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base,
				mt6797_dvfsp_cspm_test_operation, &ctx, NULL, NULL),
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
				mt6797_dvfsp_cspm_test_operation, &ctx, NULL, NULL),
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
				mt6797_dvfsp_cspm_test_operation, &ctx, NULL, NULL),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1U);
	KUNIT_ASSERT_TRUE(test, mutex_trylock(&access.lock));
	mutex_unlock(&access.lock);
}

static void mt6797_dvfsp_cspm_terminal_callback_faults_owner(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_test_context ctx = {
		.test = test,
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
		.result = -EUCLEAN,
	};
	int ret;

	mt6797_dvfsp_cspm_access_init(&access);
	ret = mt6797_dvfsp_cspm_access_set(&access,
					 MT6797_DVFSP_PERMISSION_READY);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_dvfsp_cspm_access_transaction(&access, ctx.expected_base,
		mt6797_dvfsp_cspm_test_operation,
		&ctx, mt6797_dvfsp_cspm_test_terminal, &ctx);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx.terminal_calls, 1U);
	KUNIT_EXPECT_TRUE(test, ctx.terminal_faulted);
	KUNIT_EXPECT_STREQ(test, ctx.terminal_reason,
			  "cspm-transaction-terminal-failure");
	KUNIT_EXPECT_EQ(test, ctx.terminal_fault_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_cspm_access_read(&access),
			MT6797_DVFSP_PERMISSION_FAULTED);
	ret = mt6797_dvfsp_cspm_access_transaction(&access, ctx.expected_base,
		mt6797_dvfsp_cspm_test_operation,
		&ctx, mt6797_dvfsp_cspm_test_terminal, &ctx);
	KUNIT_EXPECT_EQ(test, ret, -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx.terminal_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx.terminal_fault_count, 1U);
}

static void
mt6797_dvfsp_cspm_serializes_concurrent_transactions(struct kunit *test)
{
	u32 fake_register;
	struct mt6797_dvfsp_cspm_access access;
	struct mt6797_dvfsp_cspm_concurrent_context shared = {
		.access = &access,
		.expected_base = (void __iomem *)&fake_register,
	};
	struct mt6797_dvfsp_cspm_thread_context first = {
		.shared = &shared,
	};
	struct mt6797_dvfsp_cspm_thread_context second = {
		.shared = &shared,
	};
	struct task_struct *first_task = NULL;
	struct task_struct *second_task = NULL;
	unsigned long waited;
	int ret;

	mt6797_dvfsp_cspm_access_init(&access);
	atomic_set(&shared.calls, 0);
	atomic_set(&shared.terminal_calls, 0);
	atomic_set(&shared.terminal_saw_faulted, 0);
	init_completion(&shared.first_entered);
	init_completion(&shared.release_first);
	init_completion(&first.started);
	init_completion(&first.done);
	init_completion(&second.started);
	init_completion(&second.done);
	ret = mt6797_dvfsp_cspm_access_set(&access,
					 MT6797_DVFSP_PERMISSION_READY);
	KUNIT_ASSERT_EQ(test, ret, 0);

	first_task = kthread_create(mt6797_dvfsp_cspm_transaction_thread, &first,
				    "mt6797-cspm-transaction-1");
	if (IS_ERR(first_task)) {
		KUNIT_FAIL(test, "failed to start first transaction thread: %ld",
			   PTR_ERR(first_task));
		first_task = NULL;
		goto out;
	}
	get_task_struct(first_task);
	wake_up_process(first_task);
	waited = wait_for_completion_timeout(&shared.first_entered,
					     msecs_to_jiffies(1000));
	if (!waited) {
		KUNIT_FAIL(test, "first transaction did not enter callback");
		goto out;
	}
	second_task = kthread_create(mt6797_dvfsp_cspm_transaction_thread, &second,
				     "mt6797-cspm-transaction-2");
	if (IS_ERR(second_task)) {
		KUNIT_FAIL(test, "failed to start second transaction thread: %ld",
			   PTR_ERR(second_task));
		second_task = NULL;
		goto out;
	}
	get_task_struct(second_task);
	wake_up_process(second_task);
	waited = wait_for_completion_timeout(&second.started,
					     msecs_to_jiffies(1000));
	if (!waited) {
		KUNIT_FAIL(test, "second transaction thread did not start");
		goto out;
	}
	/* The second callback must remain outside the shared CSPM critical section. */
	waited = wait_for_completion_timeout(&second.done,
					     msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&shared.calls), 1);

	complete(&shared.release_first);
	waited = wait_for_completion_timeout(&first.done,
					     msecs_to_jiffies(1000));
	if (!waited) {
		KUNIT_FAIL(test, "first transaction thread did not finish");
		goto out;
	}
	waited = wait_for_completion_timeout(&second.done,
					     msecs_to_jiffies(1000));
	if (!waited) {
		KUNIT_FAIL(test, "second transaction thread did not finish");
		goto out;
	}
	KUNIT_EXPECT_EQ(test, first.result, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, second.result, -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, atomic_read(&shared.calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&shared.terminal_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&shared.terminal_saw_faulted), 1);

out:
	/* Join every worker before its stack-backed context leaves scope. */
	complete_all(&shared.release_first);
	if (first_task)
		KUNIT_EXPECT_EQ(test, kthread_stop_put(first_task), 0);
	if (second_task)
		KUNIT_EXPECT_EQ(test, kthread_stop_put(second_task), 0);
}

static void mt6797_dvfsp_i2c6_transfer_result_merge(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_i2c6_merge_transfer_result(2, 0), 2);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_i2c6_merge_transfer_result(2, -EPERM),
			-EPERM);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_i2c6_merge_transfer_result(0,
			-EHOSTDOWN), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_i2c6_merge_transfer_result(2, 1), 2);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_i2c6_merge_transfer_result(-ETIMEDOUT,
			-EPERM),
			-ETIMEDOUT);
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
				mt6797_dvfsp_cspm_test_operation, &ctx, NULL, NULL),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, NULL, mt6797_dvfsp_cspm_test_operation,
				&ctx, NULL, NULL),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_cspm_access_transaction(
				&access, ctx.expected_base, NULL, &ctx, NULL, NULL),
			-EINVAL);
}

static struct kunit_case mt6797_dvfsp_cspm_access_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_cspm_blocks_until_owner_ready),
	KUNIT_CASE(mt6797_dvfsp_cspm_owner_failure_is_sticky),
	KUNIT_CASE(mt6797_dvfsp_cspm_serializes_and_unlocks),
	KUNIT_CASE(mt6797_dvfsp_cspm_terminal_callback_faults_owner),
	KUNIT_CASE(mt6797_dvfsp_cspm_serializes_concurrent_transactions),
	KUNIT_CASE(mt6797_dvfsp_i2c6_transfer_result_merge),
	KUNIT_CASE(mt6797_dvfsp_cspm_rejects_invalid_requests),
	{}
};

static struct kunit_suite mt6797_dvfsp_cspm_access_test_suite = {
	.name = "mt6797-dvfsp-cspm-access",
	.test_cases = mt6797_dvfsp_cspm_access_test_cases,
};

kunit_test_suite(mt6797_dvfsp_cspm_access_test_suite);

MODULE_LICENSE("GPL");
