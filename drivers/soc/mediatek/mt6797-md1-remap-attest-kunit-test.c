// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for MT6797 MD1 LK bank4/lock precondition matching. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-remap-attest.h>

static struct mt6797_md1_mpu_remap_plan mt6797_md1_remap_attest_test_plan(void)
{
	return (struct mt6797_md1_mpu_remap_plan) {
		.firmware = {
			.header_version = 4,
			.bind_sys_id = 1,
			.raw_image_size = 0x100000,
			.md_memory_size = 0x200000,
			.region_count = MT6797_MD1_MPU_PLAN_HEADER_ENTRIES,
		},
		.remap = {
			[MT6797_MD1_REMAP_PHYS32] = {
				.bank4_map0 = 0x13579bdf,
				.bank4_map1 = 0x2468ace1,
			},
			[MT6797_MD1_REMAP_KERNEL_EMI_40000000] = {
				.bank4_map0 = 0xdeadbeef,
				.bank4_map1 = 0x01234567,
			},
		},
		.lk_reported_load_complete = true,
	};
}

static void mt6797_md1_remap_attest_matches_phys32_bank4_and_lock(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_attest_test_plan();
	struct mt6797_md1_remap_snapshot snapshot = {
		.bank0_map0 = 0xaaaabbbb,
		.bank0_map1 = 0xccccdddd,
		.bank4_map0 = 0x13579bdf,
		.bank4_map1 = 0x2468ace1,
		.lock = MT6797_MD1_REMAP_LOCK_MD1,
	};
	struct mt6797_md1_remap_attestation attestation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), 0);
	KUNIT_EXPECT_EQ(test, attestation.mode, MT6797_MD1_REMAP_ATTEST_PHYS32);
	KUNIT_EXPECT_TRUE(test, attestation.lk_lock_observed);
	KUNIT_EXPECT_TRUE(test, attestation.secure_mpu_unobservable);
	KUNIT_EXPECT_EQ(test, attestation.observed.bank0_map0, 0xaaaabbbbU);
}

static void mt6797_md1_remap_attest_matches_kernel_emi_bank4_and_lock(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_attest_test_plan();
	struct mt6797_md1_remap_snapshot snapshot = {
		.bank4_map0 = 0xdeadbeef,
		.bank4_map1 = 0x01234567,
		.lock = MT6797_MD1_REMAP_LOCK_MD1,
	};
	struct mt6797_md1_remap_attestation attestation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), 0);
	KUNIT_EXPECT_EQ(test, attestation.mode,
		MT6797_MD1_REMAP_ATTEST_KERNEL_EMI_40000000);
}

static void mt6797_md1_remap_attest_refuses_no_lock_or_mismatched_bank4(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_attest_test_plan();
	struct mt6797_md1_remap_snapshot snapshot = {
		.bank4_map0 = 0x13579bdf,
		.bank4_map1 = 0x2468ace1,
	};
	struct mt6797_md1_remap_attestation attestation = {
		.mode = MT6797_MD1_REMAP_ATTEST_PHYS32,
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, attestation.mode, MT6797_MD1_REMAP_ATTEST_NONE);
	snapshot.lock = MT6797_MD1_REMAP_LOCK_MD1;
	snapshot.bank4_map0 = 0;
	KUNIT_EXPECT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, attestation.mode, MT6797_MD1_REMAP_ATTEST_NONE);
}

static void mt6797_md1_remap_attest_refuses_unvalidated_lk_plan(struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_attest_test_plan();
	struct mt6797_md1_remap_snapshot snapshot = {
		.bank4_map0 = 0x13579bdf,
		.bank4_map1 = 0x2468ace1,
		.lock = MT6797_MD1_REMAP_LOCK_MD1,
	};
	struct mt6797_md1_remap_attestation attestation;

	plan.lk_reported_load_complete = false;
	KUNIT_EXPECT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), -ENODATA);
	KUNIT_EXPECT_EQ(test, attestation.mode, MT6797_MD1_REMAP_ATTEST_NONE);
	plan.lk_reported_load_complete = true;
	plan.firmware.raw_image_size = plan.firmware.md_memory_size + 1;
	KUNIT_EXPECT_EQ(test, mt6797_md1_remap_attest_plan_snapshot(&plan, &snapshot,
									&attestation), -ENODATA);
	KUNIT_EXPECT_EQ(test, attestation.mode, MT6797_MD1_REMAP_ATTEST_NONE);
}

static struct kunit_case mt6797_md1_remap_attest_test_cases[] = {
	KUNIT_CASE(mt6797_md1_remap_attest_matches_phys32_bank4_and_lock),
	KUNIT_CASE(mt6797_md1_remap_attest_matches_kernel_emi_bank4_and_lock),
	KUNIT_CASE(mt6797_md1_remap_attest_refuses_no_lock_or_mismatched_bank4),
	KUNIT_CASE(mt6797_md1_remap_attest_refuses_unvalidated_lk_plan),
	{}
};

static struct kunit_suite mt6797_md1_remap_attest_test_suite = {
	.name = "mt6797-md1-remap-attest",
	.test_cases = mt6797_md1_remap_attest_test_cases,
};
kunit_test_suite(mt6797_md1_remap_attest_test_suite);

MODULE_LICENSE("GPL");
