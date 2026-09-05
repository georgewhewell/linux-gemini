// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "clk-mt6797-reset.h"

static int mt6797_resolve(unsigned int index, bool deassert,
			  unsigned int *reg, unsigned int *mask)
{
	int id;

	id = mtk_reset_xlate_index(&infra_rst_desc, index);
	if (id < 0)
		return id;

	return mtk_reset_set_clr_reg(&infra_rst_desc, id, deassert,
				     reg, mask);
}

static void mt6797_reset_descriptor_contract(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, infra_rst_desc.version, MTK_RST_SET_CLR);
	KUNIT_EXPECT_EQ(test, infra_rst_desc.rst_bank_nr, 2U);
	KUNIT_EXPECT_EQ(test, infra_rst_desc.rst_idx_map_nr, 4U);
	KUNIT_EXPECT_EQ(test, infra_rst_ofs[0], (u16)0x120);
	KUNIT_EXPECT_EQ(test, infra_rst_ofs[1], (u16)0x140);
}

static void mt6797_reset_thermal_transaction(struct kunit *test)
{
	unsigned int index = MT6797_INFRA_THERM_CTRL_RST;
	unsigned int mask = 0, reg = 0;
	int ret;

	ret = mt6797_resolve(index, false, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x120U);
	KUNIT_EXPECT_EQ(test, mask, BIT(0));
	ret = mt6797_resolve(index, true, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x124U);
	KUNIT_EXPECT_EQ(test, mask, BIT(0));
}

static void mt6797_reset_pwrap_transaction(struct kunit *test)
{
	unsigned int index = MT6797_INFRA_PMIC_WRAP_RST;
	unsigned int mask = 0, reg = 0;
	int ret;

	ret = mt6797_resolve(index, false, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x140U);
	KUNIT_EXPECT_EQ(test, mask, BIT(0));
	ret = mt6797_resolve(index, true, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x144U);
	KUNIT_EXPECT_EQ(test, mask, BIT(0));
}

static void mt6797_reset_unknown_public_id_rejected(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mtk_reset_xlate_index(&infra_rst_desc, 4),
			-EINVAL);
}

static void mt6797_reset_old_linear_id_rejected(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mtk_reset_xlate_index(&infra_rst_desc, 13),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mtk_reset_xlate_index(&infra_rst_desc, 64),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mtk_reset_xlate_index(&infra_rst_desc, 68),
			-EINVAL);
}

static void mt6797_reset_cldma_pd_transaction(struct kunit *test)
{
	unsigned int mask = 0, reg = 0;
	int ret;

	ret = mt6797_resolve(MT6797_INFRA_CLDMA_RST, false, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x120U);
	KUNIT_EXPECT_EQ(test, mask, BIT(13));
	ret = mt6797_resolve(MT6797_INFRA_CLDMA_RST, true, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x124U);
	KUNIT_EXPECT_EQ(test, mask, BIT(13));
}

static void mt6797_reset_cldma_ao_transaction(struct kunit *test)
{
	unsigned int mask = 0, reg = 0;
	int ret;

	ret = mt6797_resolve(MT6797_INFRA_CLDMA_AO_RST, false, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x140U);
	KUNIT_EXPECT_EQ(test, mask, BIT(4));
	ret = mt6797_resolve(MT6797_INFRA_CLDMA_AO_RST, true, &reg, &mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, reg, 0x144U);
	KUNIT_EXPECT_EQ(test, mask, BIT(4));
}

static void mt6797_reset_internal_bank_overflow_rejected(struct kunit *test)
{
	unsigned long id = 2 * RST_NR_PER_BANK;
	unsigned int mask = 0, reg = 0;
	int ret;

	ret = mtk_reset_set_clr_reg(&infra_rst_desc, id, false, &reg, &mask);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_infra_reset_cases[] = {
	KUNIT_CASE(mt6797_reset_descriptor_contract),
	KUNIT_CASE(mt6797_reset_thermal_transaction),
	KUNIT_CASE(mt6797_reset_pwrap_transaction),
	KUNIT_CASE(mt6797_reset_cldma_pd_transaction),
	KUNIT_CASE(mt6797_reset_cldma_ao_transaction),
	KUNIT_CASE(mt6797_reset_unknown_public_id_rejected),
	KUNIT_CASE(mt6797_reset_old_linear_id_rejected),
	KUNIT_CASE(mt6797_reset_internal_bank_overflow_rejected),
	{}
};

static struct kunit_suite mt6797_infra_reset_suite = {
	.name = "mt6797-infracfg-reset-translation",
	.test_cases = mt6797_infra_reset_cases,
};

kunit_test_suite(mt6797_infra_reset_suite);

MODULE_LICENSE("GPL");
