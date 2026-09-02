// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free MT6797 CCCI boot-handshake tests. */

#include <kunit/test.h>
#include <linux/errno.h>

#include "mtk_mt6797_hs.h"

static const struct mtk_ccci_frame mtk_mt6797_hs1 = {
	.channel = MTK_CCCI_CONTROL_RX,
	.reserved = MTK_CCCI_MD_INIT_CHK_ID,
};

static const struct mtk_ccci_frame mtk_mt6797_hs2 = {
	.channel = MTK_CCCI_CONTROL_RX,
};

static void mtk_mt6797_hs_init_and_start(struct kunit *test,
					 struct mtk_mt6797_hs *hs)
{
	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_init(hs), 0);
	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_start(hs), 0);
	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_state(hs),
			MTK_MT6797_HS_WAIT_HS1);
}

static void mtk_mt6797_hs_requires_start(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_init(&hs), 0);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE, &action),
			-EPERM);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs), MTK_MT6797_HS_OFF);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_start(&hs), 0);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_start(&hs), -EALREADY);
}

static void mtk_mt6797_hs_rejects_bad_frames(struct kunit *test)
{
	struct mtk_ccci_frame frame = mtk_mt6797_hs1;
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &frame,
					 MTK_CCCI_HEADER_SIZE - 1, &action),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, action, MTK_MT6797_HS_ACTION_NONE);
	frame.channel = MTK_CCCI_CONTROL_TX;
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &frame,
					 MTK_CCCI_HEADER_SIZE, &action),
			-ENOMSG);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs),
			MTK_MT6797_HS_WAIT_HS1);
}

static void mtk_mt6797_hs_accepts_legacy_hs1(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE, &action),
			0);
	KUNIT_EXPECT_EQ(test, action, MTK_MT6797_HS_ACTION_HS1_LEGACY);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs),
			MTK_MT6797_HS_WAIT_HS2);
}

static void mtk_mt6797_hs_accepts_v2_hs1(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE +
					 MTK_CCCI_FEATURE_QUERY_SIZE,
					 &action),
			0);
	KUNIT_EXPECT_EQ(test, action, MTK_MT6797_HS_ACTION_HS1_V2);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs),
			MTK_MT6797_HS_WAIT_HS2);
}

static void mtk_mt6797_hs_rejects_malformed_hs1(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE + 1, &action),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, action, MTK_MT6797_HS_ACTION_NONE);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs),
			MTK_MT6797_HS_WAIT_HS1);
}

static void mtk_mt6797_hs_accepts_hs2_after_hs1(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE, &action),
			0);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs2,
					 MTK_CCCI_HEADER_SIZE, &action),
			0);
	KUNIT_EXPECT_EQ(test, action, MTK_MT6797_HS_ACTION_HS2_READY);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs), MTK_MT6797_HS_READY);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs2,
					 MTK_CCCI_HEADER_SIZE, &action),
			-EALREADY);
}

static void mtk_mt6797_hs_latches_crash_until_reset(struct kunit *test)
{
	struct mtk_mt6797_hs hs;
	enum mtk_mt6797_hs_action action;

	mtk_mt6797_hs_init_and_start(test, &hs);
	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_crash(&hs), 0);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs),
			MTK_MT6797_HS_CRASHED);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_hs_rx(&hs, &mtk_mt6797_hs1,
					 MTK_CCCI_HEADER_SIZE, &action),
			-ESHUTDOWN);
	KUNIT_ASSERT_EQ(test, mtk_mt6797_hs_reset(&hs), 0);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_state(&hs), MTK_MT6797_HS_OFF);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_hs_start(&hs), 0);
}

static struct kunit_case mtk_mt6797_hs_test_cases[] = {
	KUNIT_CASE(mtk_mt6797_hs_requires_start),
	KUNIT_CASE(mtk_mt6797_hs_rejects_bad_frames),
	KUNIT_CASE(mtk_mt6797_hs_accepts_legacy_hs1),
	KUNIT_CASE(mtk_mt6797_hs_accepts_v2_hs1),
	KUNIT_CASE(mtk_mt6797_hs_rejects_malformed_hs1),
	KUNIT_CASE(mtk_mt6797_hs_accepts_hs2_after_hs1),
	KUNIT_CASE(mtk_mt6797_hs_latches_crash_until_reset),
	{}
};

static struct kunit_suite mtk_mt6797_hs_test_suite = {
	.name = "mtk-mt6797-hs",
	.test_cases = mtk_mt6797_hs_test_cases,
};

kunit_test_suite(mtk_mt6797_hs_test_suite);

MODULE_LICENSE("GPL");
