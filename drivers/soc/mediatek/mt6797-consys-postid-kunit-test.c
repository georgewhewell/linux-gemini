// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free KUnit tests for the MT6797 CONSYS post-identity owner. */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-consys-postid.h>
#include <kunit/test.h>

enum mt6797_consys_postid_test_op {
	POSTID_TEST_NONE,
	POSTID_TEST_MBIST,
	POSTID_TEST_AFE,
	POSTID_TEST_RELEASE_RESET,
	POSTID_TEST_ASSERT_RESET,
};

struct mt6797_consys_postid_test_backend {
	bool identity_fails;
	enum mt6797_consys_postid_test_op fail_post_op;
	enum mt6797_consys_postid_test_op post_calls[8];
	u32 post_call_count;
};

static int mt6797_consys_postid_test_record(
		struct mt6797_consys_postid_test_backend *backend,
		enum mt6797_consys_postid_test_op op)
{
	if (backend->post_call_count < ARRAY_SIZE(backend->post_calls))
		backend->post_calls[backend->post_call_count++] = op;
	return backend->fail_post_op == op ? -EIO : 0;
}

static int mt6797_consys_postid_test_enable_vcn18(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_disable_vcn18(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_enable_vcn28(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_disable_vcn28(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_configure_sleep_mask(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_assert_identity_reset(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_configure_pwron(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_enable_scp_sys_conn(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_disable_scp_sys_conn(void *context)
{
	return 0;
}

static int mt6797_consys_postid_test_poll(void *context,
			enum mt6797_consys_poll poll)
{
	struct mt6797_consys_postid_test_backend *backend = context;

	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;
	return backend->identity_fails ? -EIO : 0;
}

static void mt6797_consys_postid_test_delay(void *context, u32 min, u32 max)
{
}

static const struct mt6797_consys_ops mt6797_consys_postid_test_lifecycle_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_postid_test_enable_vcn18,
	.disable_vcn18 = mt6797_consys_postid_test_disable_vcn18,
	.enable_vcn28 = mt6797_consys_postid_test_enable_vcn28,
	.disable_vcn28 = mt6797_consys_postid_test_disable_vcn28,
	.configure_sleep_mask = mt6797_consys_postid_test_configure_sleep_mask,
	.assert_reset = mt6797_consys_postid_test_assert_identity_reset,
	.configure_pwron = mt6797_consys_postid_test_configure_pwron,
	.enable_scp_sys_conn = mt6797_consys_postid_test_enable_scp_sys_conn,
	.disable_scp_sys_conn = mt6797_consys_postid_test_disable_scp_sys_conn,
	.poll = mt6797_consys_postid_test_poll,
	.delay_us = mt6797_consys_postid_test_delay,
};

static const struct mt6797_consys_timeouts mt6797_consys_postid_test_timeouts = {
	.poll_interval_us = 1,
	.chip_ready_timeout_us = 1,
};

static int mt6797_consys_postid_test_mbist(void *context)
{
	return mt6797_consys_postid_test_record(context, POSTID_TEST_MBIST);
}

static int mt6797_consys_postid_test_afe(void *context)
{
	return mt6797_consys_postid_test_record(context, POSTID_TEST_AFE);
}

static int mt6797_consys_postid_test_release_reset(void *context)
{
	return mt6797_consys_postid_test_record(context,
						POSTID_TEST_RELEASE_RESET);
}

static int mt6797_consys_postid_test_assert_reset(void *context)
{
	return mt6797_consys_postid_test_record(context,
						POSTID_TEST_ASSERT_RESET);
}

static const struct mt6797_consys_postid_ops mt6797_consys_postid_test_ops = {
	.abi = MT6797_CONSYS_POSTID_ABI,
	.configure_mbist = mt6797_consys_postid_test_mbist,
	.configure_afe = mt6797_consys_postid_test_afe,
	.release_reset = mt6797_consys_postid_test_release_reset,
	.assert_reset = mt6797_consys_postid_test_assert_reset,
};

static int mt6797_consys_postid_test_setup(
		struct mt6797_consys_postid_test_backend *backend,
		struct mt6797_consys_lifecycle *lifecycle,
		struct mt6797_consys_runtime_lease *lease,
		struct mt6797_consys_postid_owner *owner)
{
	int ret;

	ret = mt6797_consys_lifecycle_init(lifecycle,
			&mt6797_consys_postid_test_lifecycle_ops, backend,
			&mt6797_consys_postid_test_timeouts);
	if (ret)
		return ret;
	ret = mt6797_consys_postid_init(owner, lifecycle,
			&mt6797_consys_postid_test_ops, backend);
	if (ret)
		return ret;
	return mt6797_consys_runtime_lease_acquire(lifecycle, lease);
}

static void mt6797_consys_postid_test_expect_calls(struct kunit *test,
		struct mt6797_consys_postid_test_backend *backend,
		const enum mt6797_consys_postid_test_op *expected, u32 count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->post_call_count, count);
	for (i = 0; i < count; i++)
		KUNIT_EXPECT_EQ(test, backend->post_calls[i], expected[i]);
}

static void mt6797_consys_postid_test_exact_order_and_teardown(
		struct kunit *test)
{
	static const enum mt6797_consys_postid_test_op expected[] = {
		POSTID_TEST_MBIST, POSTID_TEST_AFE, POSTID_TEST_RELEASE_RESET,
		POSTID_TEST_ASSERT_RESET,
	};
	struct mt6797_consys_postid_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_test_setup(&backend,
			&lifecycle, &lease, &owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, false),
			-EPERM);
	KUNIT_EXPECT_EQ(test, backend.post_call_count, 0U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_ACTIVE);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true),
			-EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_stop(&owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_IDLE);
	mt6797_consys_postid_test_expect_calls(test, &backend, expected,
			ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static void mt6797_consys_postid_test_gate_and_identity_failure(
		struct kunit *test)
{
	struct mt6797_consys_postid_test_backend backend = {
		.identity_fails = true,
	};
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;
	int ret;

	ret = mt6797_consys_postid_test_setup(&backend, &lifecycle,
			&lease, &owner);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, false),
			-EPERM);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true),
			-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, backend.post_call_count, 0U);
}

static void mt6797_consys_postid_test_mbist_failure_is_closed(
		struct kunit *test)
{
	static const enum mt6797_consys_postid_test_op expected[] = {
		POSTID_TEST_MBIST, POSTID_TEST_ASSERT_RESET,
	};
	struct mt6797_consys_postid_test_backend backend = {
		.fail_post_op = POSTID_TEST_MBIST,
	};
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_test_setup(&backend,
			&lifecycle, &lease, &owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true),
			-EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_IDLE);
	mt6797_consys_postid_test_expect_calls(test, &backend, expected,
			ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static void mt6797_consys_postid_test_afe_failure_is_sticky(
		struct kunit *test)
{
	static const enum mt6797_consys_postid_test_op expected[] = {
		POSTID_TEST_MBIST, POSTID_TEST_AFE, POSTID_TEST_ASSERT_RESET,
	};
	struct mt6797_consys_postid_test_backend backend = {
		.fail_post_op = POSTID_TEST_AFE,
	};
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_test_setup(&backend,
			&lifecycle, &lease, &owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true),
			-EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_ERROR);
	mt6797_consys_postid_test_expect_calls(test, &backend, expected,
			ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), -EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_stop(&owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static void mt6797_consys_postid_test_release_failure_asserts(
		struct kunit *test)
{
	static const enum mt6797_consys_postid_test_op expected[] = {
		POSTID_TEST_MBIST, POSTID_TEST_AFE, POSTID_TEST_RELEASE_RESET,
		POSTID_TEST_ASSERT_RESET,
	};
	struct mt6797_consys_postid_test_backend backend = {
		.fail_post_op = POSTID_TEST_RELEASE_RESET,
	};
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_test_setup(&backend,
			&lifecycle, &lease, &owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true),
			-EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_ERROR);
	mt6797_consys_postid_test_expect_calls(test, &backend, expected,
			ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), -EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_stop(&owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static void mt6797_consys_postid_test_stop_retry_completes_teardown(
		struct kunit *test)
{
	struct mt6797_consys_postid_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_postid_owner owner;

	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_test_setup(&backend,
			&lifecycle, &lease, &owner), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_start(&owner, &lease, true), 0);
	backend.fail_post_op = POSTID_TEST_ASSERT_RESET;
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_stop(&owner), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_postid_state(&owner),
			MT6797_CONSYS_POSTID_ERROR);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), -EBUSY);
	backend.fail_post_op = 0;
	KUNIT_ASSERT_EQ(test, mt6797_consys_postid_stop(&owner), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static struct kunit_case mt6797_consys_postid_test_cases[] = {
	KUNIT_CASE(mt6797_consys_postid_test_exact_order_and_teardown),
	KUNIT_CASE(mt6797_consys_postid_test_gate_and_identity_failure),
	KUNIT_CASE(mt6797_consys_postid_test_mbist_failure_is_closed),
	KUNIT_CASE(mt6797_consys_postid_test_afe_failure_is_sticky),
	KUNIT_CASE(mt6797_consys_postid_test_release_failure_asserts),
	KUNIT_CASE(mt6797_consys_postid_test_stop_retry_completes_teardown),
	{}
};

static struct kunit_suite mt6797_consys_postid_test_suite = {
	.name = "mt6797-consys-postid",
	.test_cases = mt6797_consys_postid_test_cases,
};

kunit_test_suite(mt6797_consys_postid_test_suite);

MODULE_LICENSE("GPL");
