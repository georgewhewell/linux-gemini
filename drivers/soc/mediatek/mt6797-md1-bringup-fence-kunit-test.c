// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 source-order bring-up fence. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-fence.h>
#include <linux/string.h>

enum mt6797_md1_fence_test_call {
	MD1_FENCE_REMAP,
	MD1_FENCE_REMAP_RESTORE,
	MD1_FENCE_INITIAL_MPU,
	MD1_FENCE_MPU_FATAL,
	MD1_FENCE_CCIF_BEFORE,
	MD1_FENCE_CCIF_CLEAR,
	MD1_FENCE_CCIF_AFTER,
	MD1_FENCE_CCIF_VERIFY,
	MD1_FENCE_POWER_ON_RESET_HELD,
	MD1_FENCE_SRC_CLK_ENABLED,
	MD1_FENCE_PLL_POLL,
	MD1_FENCE_STAGE1_MPU,
	MD1_FENCE_POWER_OFF_RESET_HELD,
};

struct mt6797_md1_fence_test_backend {
	enum mt6797_md1_fence_test_call calls[128];
	u32 count;
	u32 pll_pending;
	enum mt6797_md1_fence_test_call fail;
	bool ccif_verified;
};

static int mt6797_md1_fence_test_record(struct mt6797_md1_fence_test_backend *backend,
		enum mt6797_md1_fence_test_call call)
{
	backend->calls[backend->count++] = call;
	return backend->fail == call ? -EIO : 0;
}

static int mt6797_md1_fence_test_remap(void *context,
	const struct mt6797_md1_remap_values *values)
{
	struct mt6797_md1_fence_test_backend *backend = context;

	if (!values)
		return -EINVAL;
	return mt6797_md1_fence_test_record(backend, MD1_FENCE_REMAP);
}

static int mt6797_md1_fence_test_remap_restore(void *context)
{
	return mt6797_md1_fence_test_record(context, MD1_FENCE_REMAP_RESTORE);
}

static int mt6797_md1_fence_test_initial_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	struct mt6797_md1_fence_test_backend *backend = context;

	if (!plan || !entries || count != MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES ||
	    entries[0].emi_mpu_region != 11 || entries[1].emi_mpu_region != 7)
		return -EINVAL;
	return mt6797_md1_fence_test_record(backend, MD1_FENCE_INITIAL_MPU);
}

static void mt6797_md1_fence_test_mpu_fatal(void *context)
{
	mt6797_md1_fence_test_record(context, MD1_FENCE_MPU_FATAL);
}

static int mt6797_md1_fence_test_ccif_observe(void *context,
	struct mt6797_md1_ccif_observation *observation)
{
	struct mt6797_md1_fence_test_backend *backend = context;
	bool before = backend->count == 2;
	int ret;

	ret = mt6797_md1_fence_test_record(backend,
		before ? MD1_FENCE_CCIF_BEFORE : MD1_FENCE_CCIF_AFTER);
	if (ret)
		return ret;
	memset(observation, 0, sizeof(*observation));
	if (before)
		observation->ap_busy = 0x3;
	return 0;
}

static int mt6797_md1_fence_test_ccif_clear(void *context)
{
	return mt6797_md1_fence_test_record(context, MD1_FENCE_CCIF_CLEAR);
}

static int mt6797_md1_fence_test_ccif_verify(void *context,
	const struct mt6797_md1_ccif_observation *before,
	const struct mt6797_md1_ccif_observation *after)
{
	struct mt6797_md1_fence_test_backend *backend = context;
	int ret;

	ret = mt6797_md1_fence_test_record(backend, MD1_FENCE_CCIF_VERIFY);
	if (ret)
		return ret;
	if (!backend->ccif_verified || !before->ap_busy || after->ap_busy ||
	    after->md_busy || after->ap_sram_nonzero_words ||
	    after->md_sram_nonzero_words)
		return -EUCLEAN;
	return 0;
}

static int mt6797_md1_fence_test_power_on(void *context)
{
	return mt6797_md1_fence_test_record(context,
		MD1_FENCE_POWER_ON_RESET_HELD);
}

static int mt6797_md1_fence_test_srcclk(void *context)
{
	return mt6797_md1_fence_test_record(context, MD1_FENCE_SRC_CLK_ENABLED);
}

static int mt6797_md1_fence_test_pll_poll(void *context)
{
	struct mt6797_md1_fence_test_backend *backend = context;
	int ret;

	ret = mt6797_md1_fence_test_record(backend, MD1_FENCE_PLL_POLL);
	if (ret)
		return ret;
	if (backend->pll_pending) {
		backend->pll_pending--;
		return -EAGAIN;
	}
	return 0;
}

static int mt6797_md1_fence_test_stage1_mpu(void *context,
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	if (!plan || !entries || count != MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES ||
	    entries[0].emi_mpu_region != 12 || entries[1].emi_mpu_region != 13 ||
	    entries[2].emi_mpu_region != 10 || entries[3].emi_mpu_region != 14 ||
	    entries[1].access_permission !=
		MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION)
		return -EINVAL;
	return mt6797_md1_fence_test_record(context, MD1_FENCE_STAGE1_MPU);
}

static int mt6797_md1_fence_test_power_off(void *context)
{
	return mt6797_md1_fence_test_record(context,
		MD1_FENCE_POWER_OFF_RESET_HELD);
}

static const struct mt6797_md1_bringup_ops mt6797_md1_fence_test_ops = {
	.abi = MT6797_MD1_BRINGUP_FENCE_ABI,
	.apply_remap = mt6797_md1_fence_test_remap,
	.restore_remap = mt6797_md1_fence_test_remap_restore,
	.apply_initial_mpu = mt6797_md1_fence_test_initial_mpu,
	.mpu_latch_fatal = mt6797_md1_fence_test_mpu_fatal,
	.ccif_observe = mt6797_md1_fence_test_ccif_observe,
	.ccif_clear = mt6797_md1_fence_test_ccif_clear,
	.ccif_verify_cleared = mt6797_md1_fence_test_ccif_verify,
	.power_on_reset_held = mt6797_md1_fence_test_power_on,
	.enable_srcclk = mt6797_md1_fence_test_srcclk,
	.pll_poll = mt6797_md1_fence_test_pll_poll,
	.apply_stage1_mpu = mt6797_md1_fence_test_stage1_mpu,
	.power_off_reset_held = mt6797_md1_fence_test_power_off,
};

static struct mt6797_md1_mpu_remap_plan mt6797_md1_fence_test_plan(void)
{
	static const u8 regions[] = { 11, 12, 13, 10, 14, 7 };
	struct mt6797_md1_mpu_remap_plan plan = { };
	u32 i;

	plan.lk_reported_load_complete = true;
	plan.firmware.header_version = 6;
	plan.firmware.bind_sys_id = 1;
	plan.firmware.region_count = MT6797_MD1_MPU_PLAN_HEADER_ENTRIES;
	plan.firmware.raw_image_size = 0x100000;
	plan.firmware.md_memory_size = 0x200000;
	for (i = 0; i < MT6797_MD1_MPU_PLAN_ENTRIES; i++) {
		plan.mpu[i].emi_mpu_region = regions[i];
		plan.mpu[i].check_header_region =
			i == MT6797_MD1_MPU_SHARED_ENTRY ? U8_MAX : i;
		plan.mpu[i].start = 0x80000000ULL + i * 0x10000;
		plan.mpu[i].end = plan.mpu[i].start + 0xffff;
	}
	plan.remap[MT6797_MD1_REMAP_PHYS32].bank0_map0 = 0xbbb9b7b5;
	return plan;
}

static const struct mt6797_md1_bringup_authorization mt6797_md1_fence_authorized = {
	.lk_firmware_contract_valid = true,
	.mpu_owner_and_failure_fence = true,
	.remap_owner_and_failure_fence = true,
	.power_reset_owner_and_failure_fence = true,
	.clock_owner_and_bounded_pll = true,
	.ccif_owner_and_destructive_clear_fence = true,
};

static void mt6797_md1_fence_expect_calls(struct kunit *test,
	struct mt6797_md1_fence_test_backend *backend,
	const enum mt6797_md1_fence_test_call *expected, u32 count)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, backend->count, count);
	for (i = 0; i < count; i++)
		KUNIT_EXPECT_EQ(test, backend->calls[i], expected[i]);
}

static void mt6797_md1_fence_requires_all_authorizations(struct kunit *test)
{
	struct mt6797_md1_bringup_authorization authorization =
		mt6797_md1_fence_authorized;
	struct mt6797_md1_fence_test_backend backend = { };
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	authorization.remap_owner_and_failure_fence = false;
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &authorization), -EPERM);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_state(&fence),
		MT6797_MD1_BRINGUP_OFF);
	KUNIT_EXPECT_EQ(test, backend.count, 0U);
}

static void mt6797_md1_fence_reaches_ticket_without_boot_release(struct kunit *test)
{
	static const enum mt6797_md1_fence_test_call expected[] = {
		MD1_FENCE_REMAP, MD1_FENCE_INITIAL_MPU, MD1_FENCE_CCIF_BEFORE,
		MD1_FENCE_CCIF_CLEAR, MD1_FENCE_CCIF_AFTER, MD1_FENCE_CCIF_VERIFY,
		MD1_FENCE_POWER_ON_RESET_HELD, MD1_FENCE_SRC_CLK_ENABLED,
		MD1_FENCE_PLL_POLL,
		MD1_FENCE_STAGE1_MPU,
	};
	struct mt6797_md1_fence_test_backend backend = { .ccif_verified = true };
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &mt6797_md1_fence_authorized), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_run(&fence), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_state(&fence),
		MT6797_MD1_BRINGUP_READY_FOR_BOOT_VECTOR);
	mt6797_md1_fence_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_fence_ccif_failure_stops_before_power(struct kunit *test)
{
	static const enum mt6797_md1_fence_test_call expected[] = {
		MD1_FENCE_REMAP, MD1_FENCE_INITIAL_MPU, MD1_FENCE_CCIF_BEFORE,
		MD1_FENCE_CCIF_CLEAR, MD1_FENCE_CCIF_AFTER, MD1_FENCE_CCIF_VERIFY,
		MD1_FENCE_REMAP_RESTORE, MD1_FENCE_MPU_FATAL,
	};
	struct mt6797_md1_fence_test_backend backend = { };
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &mt6797_md1_fence_authorized), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_run(&fence), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_state(&fence),
		MT6797_MD1_BRINGUP_FENCED);
	mt6797_md1_fence_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_fence_initial_mpu_error_latches_before_ccif(struct kunit *test)
{
	static const enum mt6797_md1_fence_test_call expected[] = {
		MD1_FENCE_REMAP, MD1_FENCE_INITIAL_MPU, MD1_FENCE_REMAP_RESTORE,
		MD1_FENCE_MPU_FATAL,
	};
	struct mt6797_md1_fence_test_backend backend = {
		.fail = MD1_FENCE_INITIAL_MPU,
	};
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &mt6797_md1_fence_authorized), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_run(&fence), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_state(&fence),
		MT6797_MD1_BRINGUP_FENCED);
	mt6797_md1_fence_expect_calls(test, &backend, expected, ARRAY_SIZE(expected));
}

static void mt6797_md1_fence_pll_timeout_powers_off_reset_held(struct kunit *test)
{
	struct mt6797_md1_fence_test_backend backend = {
		.ccif_verified = true,
		.pll_pending = MT6797_MD1_BRINGUP_MAX_PLL_POLLS,
	};
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &mt6797_md1_fence_authorized), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_run(&fence), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, backend.count,
		11U + MT6797_MD1_BRINGUP_MAX_PLL_POLLS);
	KUNIT_EXPECT_EQ(test, backend.calls[backend.count - 1], MD1_FENCE_MPU_FATAL);
	KUNIT_EXPECT_EQ(test, backend.calls[backend.count - 2], MD1_FENCE_REMAP_RESTORE);
	KUNIT_EXPECT_EQ(test, backend.calls[backend.count - 3],
		MD1_FENCE_POWER_OFF_RESET_HELD);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_state(&fence),
		MT6797_MD1_BRINGUP_FENCED);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_run(&fence), -EIO);
}

static void mt6797_md1_fence_rejects_wrong_mpu_contract(struct kunit *test)
{
	struct mt6797_md1_fence_test_backend backend = { };
	struct mt6797_md1_bringup_fence fence;
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_fence_test_plan();

	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_fence_init(&fence,
		&mt6797_md1_fence_test_ops, &backend), 0);
	plan.mpu[2].emi_mpu_region = 12;
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_fence_prepare(&fence, &plan,
		MT6797_MD1_REMAP_PHYS32, &mt6797_md1_fence_authorized), -EINVAL);
	KUNIT_EXPECT_EQ(test, backend.count, 0U);
}

static struct kunit_case mt6797_md1_bringup_fence_test_cases[] = {
	KUNIT_CASE(mt6797_md1_fence_requires_all_authorizations),
	KUNIT_CASE(mt6797_md1_fence_reaches_ticket_without_boot_release),
	KUNIT_CASE(mt6797_md1_fence_ccif_failure_stops_before_power),
	KUNIT_CASE(mt6797_md1_fence_initial_mpu_error_latches_before_ccif),
	KUNIT_CASE(mt6797_md1_fence_pll_timeout_powers_off_reset_held),
	KUNIT_CASE(mt6797_md1_fence_rejects_wrong_mpu_contract),
	{}
};

static struct kunit_suite mt6797_md1_bringup_fence_test_suite = {
	.name = "mt6797-md1-bringup-fence",
	.test_cases = mt6797_md1_bringup_fence_test_cases,
};

kunit_test_suite(mt6797_md1_bringup_fence_test_suite);

MODULE_LICENSE("GPL");
