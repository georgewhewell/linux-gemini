// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "ais_resources.h"

static const u8 mt6797_wifi_ais_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static void mt6797_wifi_ais_open_bss(
	struct kunit *test, struct mt6797_wifi_ais_resources *resources,
	struct mt6797_wifi_ais_bss_handle *bss)
{
	mt6797_wifi_ais_resources_init(resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(resources), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_reserve(resources, 3, 1,
			mt6797_wifi_ais_own_addr, bss), 0);
}

static void mt6797_wifi_ais_reserve_roles_test(struct kunit *test)
{
	static const u8 peer_addr[ETH_ALEN] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;

	mt6797_wifi_ais_open_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_reserve(&resources, &bss, peer_addr, &sta), 0);
	KUNIT_EXPECT_EQ(test, bss.bmc.role,
		(u8)MT6797_WIFI_AIS_WTBL_ROLE_BSS_BMC);
	KUNIT_EXPECT_EQ(test, sta.pairwise.role,
		(u8)MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE);
	KUNIT_EXPECT_EQ(test, sta.ap_bmc_group.role,
			(u8)MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP);
	KUNIT_EXPECT_NE(test, bss.bmc.index, sta.pairwise.index);
	KUNIT_EXPECT_NE(test, bss.bmc.index, sta.ap_bmc_group.index);
	KUNIT_EXPECT_NE(test, sta.pairwise.index, sta.ap_bmc_group.index);
	KUNIT_EXPECT_LT(test, sta.sta_index,
		(u8)MT6797_WIFI_AIS_STA_COUNT);
	KUNIT_EXPECT_LT(test, bss.bmc.index,
		(u8)MT6797_WIFI_AIS_WTBL_COUNT);
	KUNIT_EXPECT_LT(test, sta.pairwise.index,
		(u8)MT6797_WIFI_AIS_WTBL_COUNT);
	KUNIT_EXPECT_LT(test, sta.ap_bmc_group.index,
			(u8)MT6797_WIFI_AIS_WTBL_COUNT);
	KUNIT_EXPECT_EQ(test, sta.pairwise.owner_cookie, sta.cookie);
	KUNIT_EXPECT_EQ(test, sta.ap_bmc_group.owner_cookie, sta.cookie);

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &bss),
		-EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &sta), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &bss), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_resources_close(&resources), 0);
}

static void mt6797_wifi_ais_atomic_bss_failure_test(struct kunit *test)
{
	struct mt6797_wifi_ais_bss_handle bss = {
		.cookie = 99,
	};
	struct mt6797_wifi_ais_resources resources;
	u64 next_cookie;

	mt6797_wifi_ais_resources_init(&resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(&resources), 0);
	bitmap_fill(resources.wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT);
	next_cookie = resources.next_cookie;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_bss_reserve(&resources, 3, 1,
			mt6797_wifi_ais_own_addr, &bss), -ENOSPC);
	KUNIT_EXPECT_FALSE(test, resources.bss_used);
	KUNIT_EXPECT_EQ(test, resources.next_cookie, next_cookie);
	KUNIT_EXPECT_EQ(test, bss.cookie, (u64)0);
}

static void mt6797_wifi_ais_atomic_sta_unwind_test(struct kunit *test)
{
	static const u8 peer_addr[ETH_ALEN] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta = {
		.cookie = 99,
	};
	u64 next_cookie;
	unsigned int sole_free = 7;

	mt6797_wifi_ais_open_bss(test, &resources, &bss);
	bitmap_fill(resources.wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT);
	__clear_bit(sole_free, resources.wtbl_used);
	next_cookie = resources.next_cookie;

	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_reserve(&resources, &bss, peer_addr, &sta),
		-ENOSPC);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(resources.sta_used,
		MT6797_WIFI_AIS_STA_COUNT));
	KUNIT_EXPECT_FALSE(test, test_bit(sole_free, resources.wtbl_used));
	KUNIT_EXPECT_EQ(test, bitmap_weight(resources.wtbl_used,
		MT6797_WIFI_AIS_WTBL_COUNT), 30);
	KUNIT_EXPECT_EQ(test, resources.next_cookie, next_cookie);
	KUNIT_EXPECT_EQ(test, sta.cookie, (u64)0);
}

static void mt6797_wifi_ais_single_ap_sta_test(struct kunit *test)
{
	static const u8 peer_addr[ETH_ALEN] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle first;
	struct mt6797_wifi_ais_sta_handle second = {
		.cookie = 99,
	};
	unsigned int sta_weight;
	unsigned int wtbl_weight;

	mt6797_wifi_ais_open_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
			mt6797_wifi_ais_sta_reserve(&resources, &bss,
				peer_addr, &first), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_sta_reserve(&resources, &bss, peer_addr, &second),
		-EBUSY);
	KUNIT_EXPECT_EQ(test, second.cookie, (u64)0);
	sta_weight = bitmap_weight(resources.sta_used,
				   MT6797_WIFI_AIS_STA_COUNT);
	wtbl_weight = bitmap_weight(resources.wtbl_used,
				    MT6797_WIFI_AIS_WTBL_COUNT);
	KUNIT_EXPECT_EQ(test, sta_weight, 1U);
	KUNIT_EXPECT_EQ(test, wtbl_weight, 3U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &first), 0);
}

static void mt6797_wifi_ais_physical_bounds_test(struct kunit *test)
{
	struct mt6797_wifi_ais_resources resources = {};
	unsigned int sta_weight;
	unsigned int wtbl_weight;

	KUNIT_EXPECT_EQ(test, ARRAY_SIZE(resources.sta),
			(size_t)MT6797_WIFI_AIS_STA_COUNT);
	KUNIT_EXPECT_EQ(test, ARRAY_SIZE(resources.wtbl),
			(size_t)MT6797_WIFI_AIS_WTBL_COUNT);
	bitmap_fill(resources.sta_used, MT6797_WIFI_AIS_STA_COUNT);
	bitmap_fill(resources.wtbl_used, MT6797_WIFI_AIS_WTBL_COUNT);
	sta_weight = bitmap_weight(resources.sta_used,
				   MT6797_WIFI_AIS_STA_COUNT);
	wtbl_weight = bitmap_weight(resources.wtbl_used,
				    MT6797_WIFI_AIS_WTBL_COUNT);
	KUNIT_EXPECT_EQ(test, sta_weight, 27U);
	KUNIT_EXPECT_EQ(test, wtbl_weight, 31U);
	KUNIT_EXPECT_FALSE(test, test_bit(MT6797_WIFI_AIS_STA_COUNT,
					  resources.sta_used));
	KUNIT_EXPECT_FALSE(test, test_bit(MT6797_WIFI_AIS_WTBL_COUNT,
					  resources.wtbl_used));
}

static void mt6797_wifi_ais_stale_wrong_double_test(struct kunit *test)
{
	static const u8 peer_addr[ETH_ALEN] = {
		0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
	};
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle wrong;
	struct mt6797_wifi_ais_sta_handle sta;

	mt6797_wifi_ais_open_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_sta_reserve(&resources, &bss, peer_addr, &sta), 0);
	wrong = sta;
	wrong.ap_bmc_group.role = MT6797_WIFI_AIS_WTBL_ROLE_STA_PAIRWISE;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &wrong),
		-ESTALE);
	KUNIT_EXPECT_TRUE(test, test_bit(sta.sta_index, resources.sta_used));
	wrong = sta;
	wrong.generation++;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &wrong),
		-ESTALE);
	wrong = sta;
	wrong.sta_index = MT6797_WIFI_AIS_STA_COUNT;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &wrong),
		-ESTALE);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &sta), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_sta_release(&resources, &sta),
		-ESTALE);

	bss.bmc.role = MT6797_WIFI_AIS_WTBL_ROLE_AP_STA_BMC_GROUP;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &bss),
		-ESTALE);
}

static void mt6797_wifi_ais_generation_test(struct kunit *test)
{
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle first;
	struct mt6797_wifi_ais_bss_handle second;

	mt6797_wifi_ais_open_bss(test, &resources, &first);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &first), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_close(&resources), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(&resources), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_bss_reserve(&resources, 3, 1,
			mt6797_wifi_ais_own_addr, &second), 0);
	KUNIT_EXPECT_NE(test, first.generation, second.generation);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &first),
		-ESTALE);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &second), 0);
}

static void mt6797_wifi_ais_overflow_terminal_test(struct kunit *test)
{
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;

	mt6797_wifi_ais_resources_init(&resources);
	resources.generation = U64_MAX;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_resources_open(&resources),
		-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, resources.state,
		(u8)MT6797_WIFI_AIS_RESOURCES_TERMINAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_resources_open(&resources),
		-ESHUTDOWN);

	mt6797_wifi_ais_resources_init(&resources);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_resources_open(&resources), 0);
	resources.next_cookie = U64_MAX;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_bss_reserve(&resources, 3, 1,
			mt6797_wifi_ais_own_addr, &bss), -EOVERFLOW);
	KUNIT_EXPECT_FALSE(test, resources.bss_used);
	KUNIT_EXPECT_EQ(test, resources.state,
		(u8)MT6797_WIFI_AIS_RESOURCES_TERMINAL);
}

static void mt6797_wifi_ais_explicit_terminal_test(struct kunit *test)
{
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;

	mt6797_wifi_ais_open_bss(test, &resources, &bss);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_resources_mark_terminal(&resources), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_resources_mark_terminal(&resources), 0);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_bss_release(&resources, &bss),
		-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_resources_close(&resources),
		-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_resources_open(&resources),
		-ESHUTDOWN);
	KUNIT_EXPECT_TRUE(test, resources.bss_used);
}

static struct kunit_case mt6797_wifi_ais_resources_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_reserve_roles_test),
	KUNIT_CASE(mt6797_wifi_ais_atomic_bss_failure_test),
	KUNIT_CASE(mt6797_wifi_ais_atomic_sta_unwind_test),
	KUNIT_CASE(mt6797_wifi_ais_single_ap_sta_test),
	KUNIT_CASE(mt6797_wifi_ais_physical_bounds_test),
	KUNIT_CASE(mt6797_wifi_ais_stale_wrong_double_test),
	KUNIT_CASE(mt6797_wifi_ais_generation_test),
	KUNIT_CASE(mt6797_wifi_ais_overflow_terminal_test),
	KUNIT_CASE(mt6797_wifi_ais_explicit_terminal_test),
	{}
};

static struct kunit_suite mt6797_wifi_ais_resources_suite = {
	.name = "mt6797-wifi-ais-resources",
	.test_cases = mt6797_wifi_ais_resources_cases,
};

kunit_test_suite(mt6797_wifi_ais_resources_suite);

MODULE_LICENSE("GPL");
