// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/soc/mediatek/mt6797-md1-boot-status-observe.h>

#define MD1_REQUESTS	(BIT(2) | BIT(3))
#define MD1_COLD_BITS	(BIT(1) | BIT(4) | BIT(8))
#define MD1_PROTECT	(BIT(3) | BIT(4) | BIT(5) | BIT(7) | BIT(10) | BIT(16))

static void mt6797_md1_power_cold_consistent_test(struct kunit *test)
{
	struct mt6797_md1_power_snapshot snapshot = {
		.poweron_config = BIT(0),
		.md1_pwr_con = MD1_COLD_BITS,
		.md_ext_buck_iso = BIT(0),
		.infra_protect_en = MD1_PROTECT,
		.infra_protect_sta = MD1_PROTECT,
	};
	bool cold_consistent;

	KUNIT_EXPECT_EQ(test,
			mt6797_md1_power_state_classify(&snapshot, &cold_consistent),
			MT6797_MD1_POWER_COLD_OFF);
	KUNIT_EXPECT_TRUE(test, cold_consistent);
}

static void mt6797_md1_power_cold_without_pg_signature_test(struct kunit *test)
{
	struct mt6797_md1_power_snapshot snapshot = {
		.poweron_config = BIT(0),
		.md1_pwr_con = MD1_COLD_BITS,
		.md_ext_buck_iso = BIT(0),
	};
	bool cold_consistent = true;

	KUNIT_EXPECT_EQ(test,
			mt6797_md1_power_state_classify(&snapshot, &cold_consistent),
			MT6797_MD1_POWER_COLD_OFF);
	KUNIT_EXPECT_FALSE(test, cold_consistent);
}

static void mt6797_md1_power_reset_held_test(struct kunit *test)
{
	struct mt6797_md1_power_snapshot snapshot = {
		.pwr_status = BIT(0),
		.pwr_status_2nd = BIT(0),
		.md1_pwr_con = MD1_REQUESTS,
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_classify(&snapshot, NULL),
			MT6797_MD1_POWERED_DOMAIN_RESET_HELD);
}

static void mt6797_md1_power_domain_reset_deasserted_test(struct kunit *test)
{
	struct mt6797_md1_power_snapshot snapshot = {
		.pwr_status = BIT(0),
		.pwr_status_2nd = BIT(0),
		.md1_pwr_con = MD1_REQUESTS | BIT(0),
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_classify(&snapshot, NULL),
			MT6797_MD1_POWERED_DOMAIN_RESET_DEASSERTED);
}

static void mt6797_md1_power_inconsistent_test(struct kunit *test)
{
	struct mt6797_md1_power_snapshot snapshot = {
		.pwr_status = BIT(0),
		.md1_pwr_con = MD1_REQUESTS,
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_power_state_classify(&snapshot, NULL),
			MT6797_MD1_POWER_INCONSISTENT);
}

static struct kunit_case mt6797_md1_power_cases[] = {
	KUNIT_CASE(mt6797_md1_power_cold_consistent_test),
	KUNIT_CASE(mt6797_md1_power_cold_without_pg_signature_test),
	KUNIT_CASE(mt6797_md1_power_reset_held_test),
	KUNIT_CASE(mt6797_md1_power_domain_reset_deasserted_test),
	KUNIT_CASE(mt6797_md1_power_inconsistent_test),
	{}
};

static struct kunit_suite mt6797_md1_power_suite = {
	.name = "mt6797-md1-power-observe",
	.test_cases = mt6797_md1_power_cases,
};

kunit_test_suite(mt6797_md1_power_suite);

MODULE_LICENSE("GPL");
