// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/soc/mediatek/mt6797-consys-vcn33.h>

static void mt6797_consys_vcn33_table_test(struct kunit *test)
{
	static const int uv[] = { 3300000, 3400000, 3500000, 3600000 };
	int got;
	u8 level;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(uv); i++) {
		KUNIT_ASSERT_EQ(test, mt6797_consys_vcn33_level_to_uv(i, &got), 0);
		KUNIT_EXPECT_EQ(test, got, uv[i]);
		KUNIT_ASSERT_EQ(test, mt6797_consys_vcn33_uv_to_level(uv[i], &level), 0);
		KUNIT_EXPECT_EQ(test, level, (u8)i);
	}
	KUNIT_EXPECT_EQ(test, mt6797_consys_vcn33_level_to_uv(4, &got), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_consys_vcn33_uv_to_level(3350000, &level), -ERANGE);
}

static struct kunit_case mt6797_consys_vcn33_cases[] = {
	KUNIT_CASE(mt6797_consys_vcn33_table_test), {}
};
static struct kunit_suite mt6797_consys_vcn33_suite = {
	.name = "mt6797-consys-vcn33", .test_cases = mt6797_consys_vcn33_cases,
};
kunit_test_suite(mt6797_consys_vcn33_suite);
MODULE_LICENSE("GPL");
