// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 CONSYS lifecycle core. */

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <kunit/test.h>

enum mt6797_consys_test_op {
	TEST_OP_NONE,
	TEST_ENABLE_VCN18,
	TEST_DISABLE_VCN18,
	TEST_ENABLE_VCN28,
	TEST_DISABLE_VCN28,
	TEST_CONFIGURE_SLEEP_MASK,
	TEST_ASSERT_RESET,
	TEST_CONFIGURE_PWRON,
	TEST_ENABLE_SCP_SYS_CONN,
	TEST_DISABLE_SCP_SYS_CONN,
	TEST_POLL_CHIP_READY,
	TEST_DELAY,
};

struct mt6797_consys_test_backend {
	enum mt6797_consys_test_op calls[32];
	u32 call_count;
	u32 pending;
	u32 delay_min[8];
	u32 delay_count;
	int fail_op;
};

static int mt6797_consys_test_record(struct mt6797_consys_test_backend *backend,
				     enum mt6797_consys_test_op op)
{
	backend->calls[backend->call_count++] = op;
	return backend->fail_op == op ? -EIO : 0;
}

static int mt6797_consys_test_enable_vcn18(void *context)
{
	return mt6797_consys_test_record(context, TEST_ENABLE_VCN18);
}

static int mt6797_consys_test_disable_vcn18(void *context)
{
	return mt6797_consys_test_record(context, TEST_DISABLE_VCN18);
}

static int mt6797_consys_test_enable_vcn28(void *context)
{
	return mt6797_consys_test_record(context, TEST_ENABLE_VCN28);
}

static int mt6797_consys_test_disable_vcn28(void *context)
{
	return mt6797_consys_test_record(context, TEST_DISABLE_VCN28);
}

static int mt6797_consys_test_configure_sleep_mask(void *context)
{
	return mt6797_consys_test_record(context, TEST_CONFIGURE_SLEEP_MASK);
}

static int mt6797_consys_test_assert_reset(void *context)
{
	return mt6797_consys_test_record(context, TEST_ASSERT_RESET);
}

static int mt6797_consys_test_configure_pwron(void *context)
{
	return mt6797_consys_test_record(context, TEST_CONFIGURE_PWRON);
}

static int mt6797_consys_test_enable_scp_sys_conn(void *context)
{
	return mt6797_consys_test_record(context, TEST_ENABLE_SCP_SYS_CONN);
}

static int mt6797_consys_test_disable_scp_sys_conn(void *context)
{
	return mt6797_consys_test_record(context, TEST_DISABLE_SCP_SYS_CONN);
}

static int mt6797_consys_test_poll(void *context,
				   enum mt6797_consys_poll poll)
{
	struct mt6797_consys_test_backend *backend = context;
	enum mt6797_consys_test_op op = TEST_POLL_CHIP_READY;

	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;
	mt6797_consys_test_record(backend, op);
	if (backend->fail_op == op)
		return -EIO;
	if (backend->pending) {
		backend->pending--;
		return -EAGAIN;
	}
	return 0;
}

static void mt6797_consys_test_delay(void *context, u32 min, u32 max)
{
	struct mt6797_consys_test_backend *backend = context;

	mt6797_consys_test_record(backend, TEST_DELAY);
	backend->delay_min[backend->delay_count] = min;
	backend->delay_count++;
}

static const struct mt6797_consys_ops mt6797_consys_test_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_test_enable_vcn18,
	.disable_vcn18 = mt6797_consys_test_disable_vcn18,
	.enable_vcn28 = mt6797_consys_test_enable_vcn28,
	.disable_vcn28 = mt6797_consys_test_disable_vcn28,
	.configure_sleep_mask = mt6797_consys_test_configure_sleep_mask,
	.assert_reset = mt6797_consys_test_assert_reset,
	.configure_pwron = mt6797_consys_test_configure_pwron,
	.enable_scp_sys_conn = mt6797_consys_test_enable_scp_sys_conn,
	.disable_scp_sys_conn = mt6797_consys_test_disable_scp_sys_conn,
	.poll = mt6797_consys_test_poll,
	.delay_us = mt6797_consys_test_delay,
};

static const struct mt6797_consys_timeouts mt6797_consys_test_timeouts = {
	.poll_interval_us = 1,
	.chip_ready_timeout_us = 3,
};

static void mt6797_consys_test_init(struct kunit *test,
				    struct mt6797_consys_lifecycle *lifecycle,
				    struct mt6797_consys_test_backend *backend)
{
	int ret;

	ret = mt6797_consys_lifecycle_init(lifecycle, &mt6797_consys_test_ops,
					   backend, &mt6797_consys_test_timeouts);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void mt6797_consys_test_expect_calls(struct kunit *test,
					    struct mt6797_consys_test_backend *backend,
					    const enum mt6797_consys_test_op *expected,
					    u32 expected_count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->call_count, expected_count);
	for (i = 0; i < expected_count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_consys_test_reference_counting(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_VCN18, TEST_DELAY, TEST_ENABLE_VCN28,
		TEST_CONFIGURE_SLEEP_MASK, TEST_ASSERT_RESET, TEST_CONFIGURE_PWRON,
		TEST_ENABLE_SCP_SYS_CONN, TEST_DELAY, TEST_POLL_CHIP_READY,
		TEST_ASSERT_RESET, TEST_DISABLE_SCP_SYS_CONN,
		TEST_DISABLE_VCN28, TEST_DISABLE_VCN18,
	};
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_identity_get(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_IDENTITY_HELD);
	KUNIT_EXPECT_EQ(test, backend.delay_min[0],
			MT6797_CONSYS_VCN18_SETTLE_US);
	KUNIT_EXPECT_EQ(test, backend.delay_min[1],
			MT6797_CONSYS_DOMAIN_SETTLE_US);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_identity_get(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 2U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_identity_put(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 1U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_identity_put(&lifecycle), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_unwinds_in_reverse_order(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_VCN18, TEST_DELAY, TEST_ENABLE_VCN28,
		TEST_CONFIGURE_SLEEP_MASK, TEST_ASSERT_RESET, TEST_CONFIGURE_PWRON,
		TEST_ENABLE_SCP_SYS_CONN, TEST_DELAY, TEST_POLL_CHIP_READY,
		TEST_ASSERT_RESET, TEST_DISABLE_SCP_SYS_CONN,
		TEST_DISABLE_VCN28, TEST_DISABLE_VCN18,
	};
	struct mt6797_consys_test_backend backend = {
		.fail_op = TEST_POLL_CHIP_READY,
	};
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 0U);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

/* A rail operation may have changed hardware before it returns its error. */
static void mt6797_consys_test_unwinds_failed_vcn28_setup(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_VCN18, TEST_DELAY, TEST_ENABLE_VCN28,
		TEST_DISABLE_VCN28, TEST_DISABLE_VCN18,
	};
	struct mt6797_consys_test_backend backend = {
		.fail_op = TEST_ENABLE_VCN28,
	};
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_times_out_with_bounded_polls(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_VCN18, TEST_DELAY, TEST_ENABLE_VCN28,
		TEST_CONFIGURE_SLEEP_MASK, TEST_ASSERT_RESET, TEST_CONFIGURE_PWRON,
		TEST_ENABLE_SCP_SYS_CONN, TEST_DELAY, TEST_POLL_CHIP_READY, TEST_DELAY,
		TEST_POLL_CHIP_READY, TEST_DELAY, TEST_POLL_CHIP_READY,
		TEST_DELAY, TEST_POLL_CHIP_READY, TEST_ASSERT_RESET,
		TEST_DISABLE_SCP_SYS_CONN, TEST_DISABLE_VCN28, TEST_DISABLE_VCN18,
	};
	struct mt6797_consys_test_backend backend = {
		.pending = 4,
	};
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, backend.delay_count, 5U);
	KUNIT_EXPECT_EQ(test, backend.delay_min[0],
			MT6797_CONSYS_VCN18_SETTLE_US);
	KUNIT_EXPECT_EQ(test, backend.delay_min[4],
			mt6797_consys_test_timeouts.poll_interval_us);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_one_interval_reaches_deadline(struct kunit *test)
{
	static const enum mt6797_consys_test_op expected[] = {
		TEST_ENABLE_VCN18, TEST_DELAY, TEST_ENABLE_VCN28,
		TEST_CONFIGURE_SLEEP_MASK, TEST_ASSERT_RESET, TEST_CONFIGURE_PWRON,
		TEST_ENABLE_SCP_SYS_CONN, TEST_DELAY, TEST_POLL_CHIP_READY, TEST_DELAY,
		TEST_POLL_CHIP_READY, TEST_ASSERT_RESET, TEST_DISABLE_SCP_SYS_CONN,
		TEST_DISABLE_VCN28, TEST_DISABLE_VCN18,
	};
	struct mt6797_consys_test_backend backend = {
		.pending = 2,
	};
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_timeouts timeouts = mt6797_consys_test_timeouts;
	int ret;

	timeouts.chip_ready_timeout_us = timeouts.poll_interval_us;
	ret = mt6797_consys_lifecycle_init(&lifecycle, &mt6797_consys_test_ops,
					   &backend, &timeouts);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, backend.delay_count, 3U);
	KUNIT_EXPECT_EQ(test, backend.delay_min[2], timeouts.poll_interval_us);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_OFF);
	mt6797_consys_test_expect_calls(test, &backend, expected,
					ARRAY_SIZE(expected));
}

static void mt6797_consys_test_cleanup_failure_is_sticky(struct kunit *test)
{
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_identity_get(&lifecycle), 0);
	backend.fail_op = TEST_ASSERT_RESET;
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_put(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
			MT6797_CONSYS_ERROR);
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_users(&lifecycle), 0U);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_put(&lifecycle), -EIO);
}

static void mt6797_consys_test_rejects_invalid_contract(struct kunit *test)
{
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_ops ops = mt6797_consys_test_ops;
	struct mt6797_consys_timeouts timeouts = mt6797_consys_test_timeouts;
	int ret;

	ops.abi++;
	ret = mt6797_consys_lifecycle_init(&lifecycle, &ops, NULL, &timeouts);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ops = mt6797_consys_test_ops;
	timeouts.chip_ready_timeout_us = 0;
	ret = mt6797_consys_lifecycle_init(&lifecycle, &ops, NULL, &timeouts);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_consys_test_shared_memory_geometry(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mt6797_consys_shared_mem_valid(0x40000000,
						       MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_TRUE(test, mt6797_consys_shared_mem_valid(0xbfa00000,
						       MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_TRUE(test, mt6797_consys_shared_mem_valid(
				MT6797_CONSYS_SHARED_MEM_END -
				MT6797_CONSYS_SHARED_MEM_SIZE,
				MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_FALSE(test, mt6797_consys_shared_mem_valid(0x3fe00000,
							MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_FALSE(test, mt6797_consys_shared_mem_valid(
				MT6797_CONSYS_SHARED_MEM_END,
				MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_FALSE(test, mt6797_consys_shared_mem_valid(0xbfa01000,
							MT6797_CONSYS_SHARED_MEM_SIZE));
	KUNIT_EXPECT_FALSE(test, mt6797_consys_shared_mem_valid(0xbfa00000,
							MT6797_CONSYS_SHARED_MEM_SIZE / 2));
}

static void mt6797_consys_test_runtime_lease_is_exclusive_and_pinned(
									struct kunit *test)
{
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_runtime_lease competing = { };
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
									  &lease), 0);
	KUNIT_EXPECT_TRUE(test, mt6797_consys_runtime_lease_active(&lease));
	KUNIT_EXPECT_EQ(test, mt6797_consys_identity_get(&lifecycle), -EBUSY);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
									  &competing), -EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_pin(&lease), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&lease), -EBUSY);
	mt6797_consys_runtime_lease_unpin(&lease);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
	KUNIT_EXPECT_FALSE(test, mt6797_consys_runtime_lease_active(&lease));
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
				MT6797_CONSYS_OFF);
}

static void mt6797_consys_test_runtime_lease_generation_rejects_stale_copy(
										 struct kunit *test)
{
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_runtime_lease stale;
	struct mt6797_consys_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
									  &lease), 0);
	stale = lease;
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
									  &lease), 0);
	KUNIT_EXPECT_FALSE(test, mt6797_consys_runtime_lease_active(&stale));
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_pin(&stale), -ESHUTDOWN);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
}

static struct kunit_case mt6797_consys_core_test_cases[] = {
	KUNIT_CASE(mt6797_consys_test_reference_counting),
	KUNIT_CASE(mt6797_consys_test_unwinds_in_reverse_order),
	KUNIT_CASE(mt6797_consys_test_unwinds_failed_vcn28_setup),
	KUNIT_CASE(mt6797_consys_test_times_out_with_bounded_polls),
	KUNIT_CASE(mt6797_consys_test_one_interval_reaches_deadline),
	KUNIT_CASE(mt6797_consys_test_cleanup_failure_is_sticky),
	KUNIT_CASE(mt6797_consys_test_rejects_invalid_contract),
	KUNIT_CASE(mt6797_consys_test_shared_memory_geometry),
	KUNIT_CASE(mt6797_consys_test_runtime_lease_is_exclusive_and_pinned),
	KUNIT_CASE(mt6797_consys_test_runtime_lease_generation_rejects_stale_copy),
	{ }
};

static struct kunit_suite mt6797_consys_core_test_suite = {
	.name = "mt6797-consys-core",
	.test_cases = mt6797_consys_core_test_cases,
};

kunit_test_suite(mt6797_consys_core_test_suite);

MODULE_LICENSE("GPL");
