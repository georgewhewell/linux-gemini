// SPDX-License-Identifier: GPL-2.0-only
/* Fake-read tests for the MT6797 MD1 remap observer core. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>
#include <linux/string.h>

struct mt6797_md1_remap_observe_fake {
	u32 values[5];
	u32 offsets[5];
	unsigned int reads;
};

static struct mt6797_md1_mpu_remap_plan mt6797_md1_remap_observe_test_plan(void)
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

static u32 mt6797_md1_remap_observe_fake_read(void *context, u32 offset)
{
	struct mt6797_md1_remap_observe_fake *fake = context;

	if (WARN_ON_ONCE(fake->reads >= ARRAY_SIZE(fake->values)))
		return 0;
	fake->offsets[fake->reads] = offset;
	return fake->values[fake->reads++];
}

static void mt6797_md1_remap_observe_expect_offsets(struct kunit *test,
	const struct mt6797_md1_remap_observe_fake *fake)
{
	static const u32 expected[] = {
		MT6797_MD1_REMAP_BANK0_MAP0_OFFSET,
		MT6797_MD1_REMAP_BANK0_MAP1_OFFSET,
		MT6797_MD1_REMAP_BANK4_MAP0_OFFSET,
		MT6797_MD1_REMAP_BANK4_MAP1_OFFSET,
		MT6797_MD1_REMAP_LOCK_OFFSET,
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, fake->reads, ARRAY_SIZE(expected));
	for (i = 0; i < ARRAY_SIZE(expected); i++)
		KUNIT_EXPECT_EQ(test, fake->offsets[i], expected[i]);
}

static void mt6797_md1_remap_observe_phys32_is_exactly_five_reads(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_observe_test_plan();
	struct mt6797_md1_remap_observe_fake fake = {
		.values = { 0xaaaabbbb, 0xccccdddd, 0x13579bdf, 0x2468ace1,
				    MT6797_MD1_REMAP_LOCK_MD1 },
	};
	struct mt6797_md1_remap_observation observation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_observe(&plan,
		mt6797_md1_remap_observe_fake_read, &fake, &observation), 0);
	mt6797_md1_remap_observe_expect_offsets(test, &fake);
	KUNIT_EXPECT_TRUE(test, observation.observed);
	KUNIT_EXPECT_EQ(test, observation.classification, 0);
	KUNIT_EXPECT_EQ(test, observation.attestation.mode,
		MT6797_MD1_REMAP_ATTEST_PHYS32);
	KUNIT_EXPECT_EQ(test, observation.snapshot.bank0_map0, 0xaaaabbbbU);
}

static void mt6797_md1_remap_observe_kernel_emi_is_classified(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_observe_test_plan();
	struct mt6797_md1_remap_observe_fake fake = {
		.values = { 0, 0, 0xdeadbeef, 0x01234567,
				    MT6797_MD1_REMAP_LOCK_MD1 },
	};
	struct mt6797_md1_remap_observation observation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_observe(&plan,
		mt6797_md1_remap_observe_fake_read, &fake, &observation), 0);
	mt6797_md1_remap_observe_expect_offsets(test, &fake);
	KUNIT_EXPECT_EQ(test, observation.classification, 0);
	KUNIT_EXPECT_EQ(test, observation.attestation.mode,
		MT6797_MD1_REMAP_ATTEST_KERNEL_EMI_40000000);
}

static void mt6797_md1_remap_observe_keeps_raw_rejected_lock_evidence(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_observe_test_plan();
	struct mt6797_md1_remap_observe_fake fake = {
		.values = { 1, 2, 0x13579bdf, 0x2468ace1, 0 },
	};
	struct mt6797_md1_remap_observation observation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_observe(&plan,
		mt6797_md1_remap_observe_fake_read, &fake, &observation), 0);
	mt6797_md1_remap_observe_expect_offsets(test, &fake);
	KUNIT_EXPECT_TRUE(test, observation.observed);
	KUNIT_EXPECT_EQ(test, observation.classification, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, observation.snapshot.bank4_map0, 0x13579bdfU);
	KUNIT_EXPECT_EQ(test, observation.attestation.mode,
		MT6797_MD1_REMAP_ATTEST_NONE);
}

static void mt6797_md1_remap_observe_keeps_raw_mismatch_evidence(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_observe_test_plan();
	struct mt6797_md1_remap_observe_fake fake = {
		.values = { 1, 2, 3, 4, MT6797_MD1_REMAP_LOCK_MD1 },
	};
	struct mt6797_md1_remap_observation observation;

	KUNIT_ASSERT_EQ(test, mt6797_md1_remap_observe(&plan,
		mt6797_md1_remap_observe_fake_read, &fake, &observation), 0);
	mt6797_md1_remap_observe_expect_offsets(test, &fake);
	KUNIT_EXPECT_EQ(test, observation.classification, -EUCLEAN);
	KUNIT_EXPECT_EQ(test, observation.snapshot.bank4_map1, 4U);
	KUNIT_EXPECT_EQ(test, observation.attestation.mode,
		MT6797_MD1_REMAP_ATTEST_NONE);
}

static void mt6797_md1_remap_observe_rejects_plan_before_any_read(
	struct kunit *test)
{
	struct mt6797_md1_mpu_remap_plan plan = mt6797_md1_remap_observe_test_plan();
	struct mt6797_md1_remap_observe_fake fake;
	struct mt6797_md1_remap_observation observation = {
		.observed = true,
	};

	plan.lk_reported_load_complete = false;
	memset(&fake, 0, sizeof(fake));
	KUNIT_EXPECT_EQ(test, mt6797_md1_remap_observe(&plan,
		mt6797_md1_remap_observe_fake_read, &fake, &observation), -ENODATA);
	KUNIT_EXPECT_EQ(test, fake.reads, 0U);
	KUNIT_EXPECT_FALSE(test, observation.observed);
}

static struct kunit_case mt6797_md1_remap_observe_test_cases[] = {
	KUNIT_CASE(mt6797_md1_remap_observe_phys32_is_exactly_five_reads),
	KUNIT_CASE(mt6797_md1_remap_observe_kernel_emi_is_classified),
	KUNIT_CASE(mt6797_md1_remap_observe_keeps_raw_rejected_lock_evidence),
	KUNIT_CASE(mt6797_md1_remap_observe_keeps_raw_mismatch_evidence),
	KUNIT_CASE(mt6797_md1_remap_observe_rejects_plan_before_any_read),
	{}
};

static struct kunit_suite mt6797_md1_remap_observe_test_suite = {
	.name = "mt6797-md1-remap-observe",
	.test_cases = mt6797_md1_remap_observe_test_cases,
};
kunit_test_suite(mt6797_md1_remap_observe_test_suite);

MODULE_LICENSE("GPL");
