// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 one-way secure EMI-MPU core. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-fence.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>

struct mt6797_md1_emi_mpu_test_smc {
	u64 starts[MT6797_MD1_MPU_PLAN_ENTRIES];
	u64 ends[MT6797_MD1_MPU_PLAN_ENTRIES];
	u32 region_permissions[MT6797_MD1_MPU_PLAN_ENTRIES];
	unsigned int calls;
	int fail_at;
	unsigned int begin_calls;
};

static int mt6797_md1_emi_mpu_test_begin(void *context)
{
	struct mt6797_md1_emi_mpu_test_smc *smc = context;

	smc->begin_calls++;
	return 0;
}

static int mt6797_md1_emi_mpu_test_set(void *context, u64 start, u64 end,
					u32 region_permission)
{
	struct mt6797_md1_emi_mpu_test_smc *smc = context;
	unsigned int call = smc->calls++;

	smc->starts[call] = start;
	smc->ends[call] = end;
	smc->region_permissions[call] = region_permission;
	return call == smc->fail_at ? -EIO : 0;
}

static struct mt6797_md1_mpu_remap_plan mt6797_md1_emi_mpu_test_plan(void)
{
	static const u8 regions[] = { 11, 12, 13, 10, 14, 7 };
	struct mt6797_md1_mpu_remap_plan plan = { };
	unsigned int i;

	plan.lk_reported_load_complete = true;
	plan.firmware.header_version = 6;
	plan.firmware.bind_sys_id = 1;
	plan.firmware.region_count = MT6797_MD1_MPU_PLAN_HEADER_ENTRIES;
	plan.firmware.raw_image_size = 0x100000;
	plan.firmware.md_memory_size = 0x200000;
	for (i = 0; i < MT6797_MD1_MPU_PLAN_ENTRIES; i++) {
		plan.mpu[i].start = 0x80000000ULL + i * SZ_64K;
		plan.mpu[i].end = plan.mpu[i].start + SZ_64K - 1;
		plan.mpu[i].access_permission = 0x00010000 + i;
		plan.mpu[i].emi_mpu_region = regions[i];
		plan.mpu[i].check_header_region =
			i == MT6797_MD1_MPU_SHARED_ENTRY ? U8_MAX : i;
	}
	return plan;
}

static void mt6797_md1_emi_mpu_test_entries(
	const struct mt6797_md1_mpu_remap_plan *plan,
	struct mt6797_md1_mpu_plan_entry initial[],
	struct mt6797_md1_mpu_plan_entry stage1[])
{
	unsigned int i;

	initial[0] = plan->mpu[0];
	initial[1] = plan->mpu[MT6797_MD1_MPU_SHARED_ENTRY];
	for (i = 0; i < MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES; i++)
		stage1[i] = plan->mpu[i + 1];
	stage1[1].access_permission = MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION;
}

static void mt6797_md1_emi_mpu_exact_order_and_permission(struct kunit *test)
{
	struct mt6797_md1_emi_mpu_core core;
	struct mt6797_md1_emi_mpu_test_smc smc = { .fail_at = -1 };
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
	struct mt6797_md1_mpu_plan_entry initial[2], stage1[4];
	static const u8 order[] = { 11, 7, 12, 13, 10, 14 };
	unsigned int i;

	mt6797_md1_emi_mpu_test_entries(&plan, initial, stage1);
	mt6797_md1_emi_mpu_core_init(&core);
	KUNIT_ASSERT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
		mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
		&plan, MT6797_MD1_EMI_MPU_INITIAL, initial, ARRAY_SIZE(initial)), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
		mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
		&plan, MT6797_MD1_EMI_MPU_STAGE1, stage1, ARRAY_SIZE(stage1)), 0);
	KUNIT_EXPECT_EQ(test, smc.begin_calls, 1U);
	KUNIT_ASSERT_EQ(test, smc.calls, ARRAY_SIZE(order));
	for (i = 0; i < ARRAY_SIZE(order); i++)
		KUNIT_EXPECT_EQ(test,
			smc.region_permissions[i] >> MT6797_MD1_EMI_MPU_REGION_SHIFT,
			(u32)order[i]);
	KUNIT_EXPECT_EQ(test, smc.region_permissions[0],
		plan.mpu[0].access_permission | (11U << MT6797_MD1_EMI_MPU_REGION_SHIFT));
	KUNIT_EXPECT_EQ(test, smc.region_permissions[1],
		plan.mpu[5].access_permission | (7U << MT6797_MD1_EMI_MPU_REGION_SHIFT));
	KUNIT_EXPECT_EQ(test, smc.region_permissions[3],
		MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION |
		(13U << MT6797_MD1_EMI_MPU_REGION_SHIFT));
	KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_state(&core),
		MT6797_MD1_EMI_MPU_STAGE1_COMMITTED);
}

static void mt6797_md1_emi_mpu_initial_partial_failure_is_fatal(struct kunit *test)
{
	int fail_at;

	for (fail_at = 0; fail_at < MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES;
	     fail_at++) {
		struct mt6797_md1_emi_mpu_core core;
		struct mt6797_md1_emi_mpu_test_smc smc = { .fail_at = fail_at };
		struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
		struct mt6797_md1_mpu_plan_entry initial[2], stage1[4];

		mt6797_md1_emi_mpu_test_entries(&plan, initial, stage1);
		mt6797_md1_emi_mpu_core_init(&core);
		KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
			mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
			&plan, MT6797_MD1_EMI_MPU_INITIAL, initial,
			ARRAY_SIZE(initial)), -EIO);
		KUNIT_EXPECT_EQ(test, smc.calls, (unsigned int)fail_at + 1);
		KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_state(&core),
			MT6797_MD1_EMI_MPU_FATAL);
		KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
			mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
			&plan, MT6797_MD1_EMI_MPU_STAGE1, stage1,
			ARRAY_SIZE(stage1)), -EIO);
		KUNIT_EXPECT_EQ(test, smc.calls, (unsigned int)fail_at + 1);
	}
}

static void mt6797_md1_emi_mpu_stage1_partial_failure_is_fatal(struct kunit *test)
{
	int fail_at;

	for (fail_at = 0; fail_at < MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES;
	     fail_at++) {
		struct mt6797_md1_emi_mpu_core core;
		struct mt6797_md1_emi_mpu_test_smc smc = {
			.fail_at = MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES + fail_at,
		};
		struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
		struct mt6797_md1_mpu_plan_entry initial[2], stage1[4];

		mt6797_md1_emi_mpu_test_entries(&plan, initial, stage1);
		mt6797_md1_emi_mpu_core_init(&core);
		KUNIT_ASSERT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
			mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
			&plan, MT6797_MD1_EMI_MPU_INITIAL, initial,
			ARRAY_SIZE(initial)), 0);
		KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
			mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
			&plan, MT6797_MD1_EMI_MPU_STAGE1, stage1,
			ARRAY_SIZE(stage1)), -EIO);
		KUNIT_EXPECT_EQ(test, smc.calls,
			MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES + (unsigned int)fail_at + 1);
		KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_state(&core),
			MT6797_MD1_EMI_MPU_FATAL);
	}
}

static void mt6797_md1_emi_mpu_rejects_unpinned_handoff_before_smc(struct kunit *test)
{
	struct mt6797_md1_emi_mpu_core core;
	struct mt6797_md1_emi_mpu_test_smc smc = { .fail_at = -1 };
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
	struct mt6797_md1_mpu_plan_entry initial[2], stage1[4];

	mt6797_md1_emi_mpu_test_entries(&plan, initial, stage1);
	plan.firmware.raw_image_size = 0;
	mt6797_md1_emi_mpu_core_init(&core);
	KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
		mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
		&plan, MT6797_MD1_EMI_MPU_INITIAL, initial, ARRAY_SIZE(initial)), -EINVAL);
	KUNIT_EXPECT_EQ(test, smc.calls, 0U);
	KUNIT_EXPECT_EQ(test, smc.begin_calls, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_state(&core),
		MT6797_MD1_EMI_MPU_IDLE);
}

static void mt6797_md1_emi_mpu_invalid_continuation_latches_fatal(struct kunit *test)
{
	struct mt6797_md1_emi_mpu_core core;
	struct mt6797_md1_emi_mpu_test_smc smc = { .fail_at = -1 };
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
	struct mt6797_md1_mpu_plan_entry initial[2], stage1[4];

	mt6797_md1_emi_mpu_test_entries(&plan, initial, stage1);
	mt6797_md1_emi_mpu_core_init(&core);
	KUNIT_ASSERT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
		mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
		&plan, MT6797_MD1_EMI_MPU_INITIAL, initial, ARRAY_SIZE(initial)), 0);
	stage1[1].access_permission = plan.mpu[2].access_permission;
	KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_apply(&core,
		mt6797_md1_emi_mpu_test_begin, mt6797_md1_emi_mpu_test_set, &smc,
		&plan, MT6797_MD1_EMI_MPU_STAGE1, stage1, ARRAY_SIZE(stage1)), -EINVAL);
	KUNIT_EXPECT_EQ(test, smc.calls, 2U);
	KUNIT_EXPECT_EQ(test, mt6797_md1_emi_mpu_core_state(&core),
		MT6797_MD1_EMI_MPU_FATAL);
}

static void mt6797_md1_emi_mpu_plan_match_exact_lk_values(struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_emi_mpu_test_plan();
	u32 region_permission = plan.mpu[0].access_permission |
		((u32)plan.mpu[0].emi_mpu_region << MT6797_MD1_EMI_MPU_REGION_SHIFT);

	KUNIT_EXPECT_TRUE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[0].start, plan.mpu[0].end, region_permission));
	KUNIT_EXPECT_TRUE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[5].start, plan.mpu[5].end,
		plan.mpu[5].access_permission |
		((u32)plan.mpu[5].emi_mpu_region << MT6797_MD1_EMI_MPU_REGION_SHIFT)));
	/* The stage-one temporary region 13 permission is not an exact match. */
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[2].start, plan.mpu[2].end,
		MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION |
		(13U << MT6797_MD1_EMI_MPU_REGION_SHIFT)));
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[0].start + SZ_64K, plan.mpu[0].end, region_permission));
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[0].start, plan.mpu[0].end + SZ_64K, region_permission));
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[0].start, plan.mpu[0].end,
		(plan.mpu[0].access_permission ^ 1) |
		((u32)plan.mpu[0].emi_mpu_region << MT6797_MD1_EMI_MPU_REGION_SHIFT)));
	/* An unknown region or an absent plan can never match. */
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(&plan,
		plan.mpu[0].start, plan.mpu[0].end,
		plan.mpu[0].access_permission |
		(9U << MT6797_MD1_EMI_MPU_REGION_SHIFT)));
	KUNIT_EXPECT_FALSE(test, mt6797_md1_emi_mpu_plan_entry_matches(NULL,
		plan.mpu[0].start, plan.mpu[0].end, region_permission));
}

static struct kunit_case mt6797_md1_emi_mpu_test_cases[] = {
	KUNIT_CASE(mt6797_md1_emi_mpu_exact_order_and_permission),
	KUNIT_CASE(mt6797_md1_emi_mpu_initial_partial_failure_is_fatal),
	KUNIT_CASE(mt6797_md1_emi_mpu_stage1_partial_failure_is_fatal),
	KUNIT_CASE(mt6797_md1_emi_mpu_rejects_unpinned_handoff_before_smc),
	KUNIT_CASE(mt6797_md1_emi_mpu_invalid_continuation_latches_fatal),
	KUNIT_CASE(mt6797_md1_emi_mpu_plan_match_exact_lk_values),
	{}
};

static struct kunit_suite mt6797_md1_emi_mpu_test_suite = {
	.name = "mt6797-md1-emi-mpu",
	.test_cases = mt6797_md1_emi_mpu_test_cases,
};

kunit_test_suite(mt6797_md1_emi_mpu_test_suite);

MODULE_LICENSE("GPL");
