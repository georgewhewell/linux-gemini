// SPDX-License-Identifier: GPL-2.0-only
/* Allocated-memory tests for the unarmed MT6797 MD1 backend helpers. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-backend.h>

struct mt6797_md1_backend_test_memory {
	void __iomem *infracfg;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	void __iomem *md_clksw;
	struct mt6797_md1_bringup_backend backend;
};

static struct mt6797_md1_backend_test_memory *
mt6797_md1_backend_test_new(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory;

	memory = kunit_kzalloc(test, sizeof(*memory), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, memory);
	memory->infracfg = kunit_kzalloc(test, MT6797_MD1_INFRACFG_SIZE, GFP_KERNEL);
	memory->ap_ccif = kunit_kzalloc(test, MT6797_MD1_CCIF_SIZE, GFP_KERNEL);
	memory->md_ccif = kunit_kzalloc(test, MT6797_MD1_CCIF_SIZE, GFP_KERNEL);
	memory->md_clksw = kunit_kzalloc(test, MT6797_MD1_CLKSW_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, memory->infracfg);
	KUNIT_ASSERT_NOT_NULL(test, memory->ap_ccif);
	KUNIT_ASSERT_NOT_NULL(test, memory->md_ccif);
	KUNIT_ASSERT_NOT_NULL(test, memory->md_clksw);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_backend_init(&memory->backend, NULL,
		memory->infracfg, memory->ap_ccif, memory->md_ccif,
		memory->md_clksw), 0);
	return memory;
}

static void mt6797_md1_backend_remap_has_exact_four_readbacks_and_restore(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	const struct mt6797_md1_remap_values values = {
		.bank0_map0 = 0x12345678,
		.bank0_map1 = 0x87654321,
		.bank4_map0 = 0xabcdefff,
		.bank4_map1 = 0x10203040,
	};
	const struct mt6797_md1_remap_values original = {
		.bank0_map0 = 0x13579bdf,
		.bank0_map1 = 0x2468ace0,
		.bank4_map0 = 0xdeadbeef,
		.bank4_map1 = 0x01234567,
	};

	KUNIT_ASSERT_NOT_NULL(test, memory);
	writel(original.bank0_map0, memory->infracfg + MT6797_MD1_BANK0_MAP0_OFFSET);
	writel(original.bank0_map1, memory->infracfg + MT6797_MD1_BANK0_MAP1_OFFSET);
	writel(original.bank4_map0, memory->infracfg + MT6797_MD1_BANK4_MAP0_OFFSET);
	writel(original.bank4_map1, memory->infracfg + MT6797_MD1_BANK4_MAP1_OFFSET);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_apply_remap(&memory->backend,
									 &values), 0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK0_MAP0_OFFSET),
			values.bank0_map0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK0_MAP1_OFFSET),
			values.bank0_map1);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK4_MAP0_OFFSET),
			values.bank4_map0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK4_MAP1_OFFSET),
		values.bank4_map1);
	KUNIT_ASSERT_EQ(test,
		mt6797_md1_bringup_backend_restore_remap(&memory->backend), 0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK0_MAP0_OFFSET),
		original.bank0_map0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK0_MAP1_OFFSET),
		original.bank0_map1);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK4_MAP0_OFFSET),
		original.bank4_map0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK4_MAP1_OFFSET),
		original.bank4_map1);
}

static void mt6797_md1_backend_refuses_lk_locked_remap(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	const struct mt6797_md1_remap_values values = {
		.bank0_map0 = 0x12345678,
	};

	KUNIT_ASSERT_NOT_NULL(test, memory);
	writel(0x1234abcd, memory->infracfg + MT6797_MD1_BANK0_MAP0_OFFSET);
	writel(MT6797_MD1_INFRA_REMAP_LOCK_MD1,
		memory->infracfg + MT6797_MD1_INFRA_REMAP_LOCK);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_bringup_backend_apply_remap(&memory->backend, &values),
		-EUCLEAN);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_BANK0_MAP0_OFFSET),
		0x1234abcdU);
}

static void mt6797_md1_backend_ccif_clear_is_observable(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	struct mt6797_md1_ccif_observation before, after;
	u32 word;

	KUNIT_ASSERT_NOT_NULL(test, memory);
	writel(BIT(0), memory->ap_ccif + MT6797_MD1_CCIF_BUSY);
	writel(BIT(1), memory->md_ccif + MT6797_MD1_CCIF_BUSY);
	writel(0xa5a5a5a5, memory->ap_ccif + MT6797_MD1_CCIF_CHDATA);
	writel(0x5a5a5a5a, memory->md_ccif + MT6797_MD1_CCIF_CHDATA + sizeof(u32));
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_backend_ccif_observe(&memory->backend,
									   &before), 0);
	KUNIT_EXPECT_EQ(test, before.ap_busy, BIT(0));
	KUNIT_EXPECT_EQ(test, before.md_busy, BIT(1));
	KUNIT_EXPECT_EQ(test, before.ap_sram_nonzero_words, 1U);
	KUNIT_EXPECT_EQ(test, before.md_sram_nonzero_words, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_backend_ccif_clear(&memory->backend),
				0);
	KUNIT_EXPECT_EQ(test, readl(memory->ap_ccif + MT6797_MD1_CCIF_TCHNUM), 0U);
	KUNIT_EXPECT_EQ(test, readl(memory->md_ccif + MT6797_MD1_CCIF_TCHNUM), 1U);
	KUNIT_EXPECT_EQ(test, readl(memory->ap_ccif + MT6797_MD1_CCIF_ACK), BIT(1));
	KUNIT_EXPECT_EQ(test, readl(memory->md_ccif + MT6797_MD1_CCIF_ACK), BIT(0));
	for (word = 0; word < MT6797_MD1_CCIF_SRAM_BYTES / sizeof(u32); word++) {
		KUNIT_EXPECT_EQ(test, readl(memory->ap_ccif + MT6797_MD1_CCIF_CHDATA +
			word * sizeof(u32)), 0U);
		KUNIT_EXPECT_EQ(test, readl(memory->md_ccif + MT6797_MD1_CCIF_CHDATA +
			word * sizeof(u32)), 0U);
	}
	/* Plain allocated memory has no CCIF TCHNUM side effect; model its clear. */
	writel(0, memory->ap_ccif + MT6797_MD1_CCIF_BUSY);
	writel(0, memory->md_ccif + MT6797_MD1_CCIF_BUSY);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_backend_ccif_observe(&memory->backend,
									   &after), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_ccif_verify_cleared(
		&memory->backend, &before, &after), 0);
}

static void mt6797_md1_backend_srcclk_restores_snapshot(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	const u32 original = 0x55aa00ff;

	KUNIT_ASSERT_NOT_NULL(test, memory);
	writel(original, memory->infracfg + MT6797_MD1_INFRA_SRC_CLKENA);
	KUNIT_ASSERT_EQ(test, mt6797_md1_bringup_backend_enable_srcclk(&memory->backend),
				0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_INFRA_SRC_CLKENA),
		(original & ~0xff) | 0x29);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_bringup_backend_power_off_reset_held(&memory->backend), 0);
	KUNIT_EXPECT_EQ(test, readl(memory->infracfg + MT6797_MD1_INFRA_SRC_CLKENA),
		original);
}

static void mt6797_md1_backend_pll_poll_is_bounded_state_machine(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, memory);
	writel(0x00000093, memory->md_clksw + 0x040);
	writel(0x80800080, memory->md_clksw + 0x030);
	writel(0x80808080, memory->md_clksw + 0x034);
	writel(0x00008080, memory->md_clksw + 0x048);
	for (i = 0; i < MT6797_MD1_PLL_DCM; i++)
		KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_pll_poll(&memory->backend),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_pll_poll(&memory->backend), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_pll_poll(&memory->backend),
		-EINVAL);
}

static void mt6797_md1_backend_mpu_phases_are_explicitly_refused(struct kunit *test)
{
	struct mt6797_md1_backend_test_memory *memory = mt6797_md1_backend_test_new(test);
	struct mt6797_md1_mpu_remap_plan plan = { };
	struct mt6797_md1_mpu_plan_entry entries[MT6797_MD1_MPU_PLAN_ENTRIES] = { };

	KUNIT_ASSERT_NOT_NULL(test, memory);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_apply_initial_mpu(
		&memory->backend, &plan, entries,
		MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mt6797_md1_bringup_backend_apply_stage1_mpu(
		&memory->backend, &plan, entries,
		MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES), -EOPNOTSUPP);
}

static struct kunit_case mt6797_md1_bringup_backend_test_cases[] = {
	KUNIT_CASE(mt6797_md1_backend_remap_has_exact_four_readbacks_and_restore),
	KUNIT_CASE(mt6797_md1_backend_refuses_lk_locked_remap),
	KUNIT_CASE(mt6797_md1_backend_ccif_clear_is_observable),
	KUNIT_CASE(mt6797_md1_backend_srcclk_restores_snapshot),
	KUNIT_CASE(mt6797_md1_backend_pll_poll_is_bounded_state_machine),
	KUNIT_CASE(mt6797_md1_backend_mpu_phases_are_explicitly_refused),
	{}
};

static struct kunit_suite mt6797_md1_bringup_backend_test_suite = {
	.name = "mt6797-md1-bringup-backend",
	.test_cases = mt6797_md1_bringup_backend_test_cases,
};

kunit_test_suite(mt6797_md1_bringup_backend_test_suite);

MODULE_LICENSE("GPL");
