// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "mtk-scpsys-internal.h"

static void mtk_scpsys_bus_prot_group_test(struct kunit *test)
{
	const u32 mask = GENMASK(11, 8);

	KUNIT_EXPECT_TRUE(test, mtk_scpsys_bus_prot_group_is_on(mask, mask, mask));
	KUNIT_EXPECT_TRUE(test, mtk_scpsys_bus_prot_group_is_on(~0U, ~0U, mask));
	KUNIT_EXPECT_FALSE(test,
			   mtk_scpsys_bus_prot_group_is_on(mask & ~BIT(8), mask, mask));
	KUNIT_EXPECT_FALSE(test,
			   mtk_scpsys_bus_prot_group_is_on(mask, mask & ~BIT(11), mask));
}

static struct kunit_case mtk_scpsys_test_cases[] = {
	KUNIT_CASE(mtk_scpsys_bus_prot_group_test),
	{}
};

static struct kunit_suite mtk_scpsys_test_suite = {
	.name = "mtk-scpsys",
	.test_cases = mtk_scpsys_test_cases,
};

kunit_test_suite(mtk_scpsys_test_suite);

MODULE_LICENSE("GPL");
