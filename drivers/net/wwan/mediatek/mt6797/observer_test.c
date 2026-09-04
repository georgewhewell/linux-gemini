// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/string.h>

#include "observer.h"

static void mt6797_md1_inherited_quiescent_test(struct kunit *test)
{
	struct mt6797_md1_snapshot snapshot = {};

	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_QUIESCENT);
}

static void mt6797_md1_inherited_ccif_pending_test(struct kunit *test)
{
	struct mt6797_md1_snapshot snapshot = {
		.ccif_pending = BIT(0),
	};

	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_CCIF_PENDING);

	snapshot.ccif_pending = 0;
	snapshot.ccif_busy = BIT(3);
	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_CCIF_PENDING);
}

static void mt6797_md1_inherited_foreign_ring_test(struct kunit *test)
{
	struct mt6797_md1_snapshot snapshot = {};

	snapshot.tx_start[0] = 0x88001000;
	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_FOREIGN_RINGS);

	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.rx_start[6] = 0x88002000;
	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_FOREIGN_RINGS);

	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.tx_active = BIT(2);
	KUNIT_EXPECT_EQ(test, mt6797_md1_snapshot_classify(&snapshot),
			MT6797_MD1_INHERITED_FOREIGN_RINGS);
}

static void mt6797_md1_inherited_stability_test(struct kunit *test)
{
	struct mt6797_md1_snapshot first = {
		.tx_start = { 0x88001000 },
	};
	struct mt6797_md1_snapshot second = first;

	KUNIT_EXPECT_TRUE(test,
			  mt6797_md1_snapshot_equal(&first, &second));
	second.rx_active = BIT(0);
	KUNIT_EXPECT_FALSE(test,
			   mt6797_md1_snapshot_equal(&first, &second));
}

static struct kunit_case mt6797_md1_observer_test_cases[] = {
	KUNIT_CASE(mt6797_md1_inherited_quiescent_test),
	KUNIT_CASE(mt6797_md1_inherited_ccif_pending_test),
	KUNIT_CASE(mt6797_md1_inherited_foreign_ring_test),
	KUNIT_CASE(mt6797_md1_inherited_stability_test),
	{}
};

static struct kunit_suite mt6797_md1_observer_test_suite = {
	.name = "mt6797-md1-observer",
	.test_cases = mt6797_md1_observer_test_cases,
};

kunit_test_suite(mt6797_md1_observer_test_suite);
