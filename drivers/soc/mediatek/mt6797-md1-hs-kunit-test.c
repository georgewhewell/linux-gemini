// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free MT6797 MD1 CCCI HS1/HS2 wire-contract tests. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-hs.h>

static struct mt6797_md1_hs_frame mt6797_md1_hs_legacy_hs1 = {
	.data1 = 0,
	.channel = MT6797_MD1_CCCI_CONTROL_RX,
	.reserved = MT6797_MD1_MD_INIT_CHK_ID,
	.wire_size = MT6797_MD1_CCCI_HEADER_SIZE,
};

static struct mt6797_md1_hs_frame mt6797_md1_hs_v2_hs1 = {
	.data1 = 0,
	.channel = MT6797_MD1_CCCI_CONTROL_RX,
	.reserved = MT6797_MD1_MD_INIT_CHK_ID,
	.wire_size = MT6797_MD1_HS1_V2_FRAME_SIZE,
};

static struct mt6797_md1_hs_frame mt6797_md1_hs_hs2 = {
	.data1 = 0,
	.channel = MT6797_MD1_CCCI_CONTROL_RX,
	.wire_size = MT6797_MD1_CCCI_HEADER_SIZE,
};

static void mt6797_md1_hs_init_and_arm(struct kunit *test,
				       struct mt6797_md1_hs *hs)
{
	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_init(hs), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_arm(hs, true), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_state(hs), MT6797_MD1_HS_WAIT_HS1);
}

static void mt6797_md1_hs_requires_post_vector_activity(struct kunit *test)
{
	struct mt6797_md1_hs hs;

	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_init(&hs), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_arm(&hs, false), -EPERM);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_OFF);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_arm(&hs, true), 0);
}

static void
mt6797_md1_hs_refuses_unframed_or_wrong_channel_input(struct kunit *test)
{
	struct mt6797_md1_hs_frame frame = mt6797_md1_hs_legacy_hs1;
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	frame.wire_size--;
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_rx(&hs, &frame, &action),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, action, MT6797_MD1_HS_ACTION_NONE);
	frame = mt6797_md1_hs_legacy_hs1;
	frame.channel = MT6797_MD1_CCCI_CONTROL_TX;
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_rx(&hs, &frame, &action), -ENOMSG);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS1);
}

static void mt6797_md1_hs_accepts_exact_legacy_hs1(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_legacy_hs1, &action),
		0);
	KUNIT_EXPECT_EQ(test, action,
			MT6797_MD1_HS_ACTION_HS1_LEGACY_NEEDS_RUNTIME);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS2);
}

static void mt6797_md1_hs_accepts_exact_v2_hs1(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_v2_hs1, &action), 0);
	KUNIT_EXPECT_EQ(test, action,
			MT6797_MD1_HS_ACTION_HS1_V2_NEEDS_RUNTIME);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS2);
}

static void
mt6797_md1_hs_refuses_malformed_hs1_without_runtime_action(struct kunit *test)
{
	struct mt6797_md1_hs_frame frame = mt6797_md1_hs_legacy_hs1;
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	frame.wire_size++;
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_rx(&hs, &frame, &action),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, action, MT6797_MD1_HS_ACTION_NONE);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS1);
}

static void
mt6797_md1_hs_hs2_requires_a_valid_hs1_transition(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_hs2, &action),
			-ENOMSG);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS1);
}

static void mt6797_md1_hs_accepts_hs2_after_hs1(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_v2_hs1, &action), 0);
	KUNIT_ASSERT_EQ(test,
			mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_hs2, &action), 0);
	KUNIT_EXPECT_EQ(test, action, MT6797_MD1_HS_ACTION_HS2_READY);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_READY);
}

static void
mt6797_md1_hs_keeps_hs1_priority_while_waiting_for_hs2(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_legacy_hs1, &action),
		0);
	KUNIT_EXPECT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_legacy_hs1, &action),
		-ENOMSG);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_WAIT_HS2);
}

static void mt6797_md1_hs_rejects_replayed_hs2(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(
		test, mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_legacy_hs1, &action),
		0);
	KUNIT_ASSERT_EQ(test,
			mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_hs2, &action), 0);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_hs2, &action),
			-EALREADY);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_READY);
}

static void mt6797_md1_hs_latches_crash_until_reset(struct kunit *test)
{
	struct mt6797_md1_hs hs;
	enum mt6797_md1_hs_action action;

	mt6797_md1_hs_init_and_arm(test, &hs);
	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_crash(&hs, -EIO), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_CRASHED);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_hs_rx(&hs, &mt6797_md1_hs_v2_hs1, &action),
			-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_arm(&hs, true), -EALREADY);
	KUNIT_ASSERT_EQ(test, mt6797_md1_hs_reset(&hs), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_state(&hs), MT6797_MD1_HS_OFF);
	KUNIT_EXPECT_EQ(test, mt6797_md1_hs_arm(&hs, true), 0);
}

static struct kunit_case mt6797_md1_hs_test_cases[] = {
	KUNIT_CASE(mt6797_md1_hs_requires_post_vector_activity),
	KUNIT_CASE(mt6797_md1_hs_refuses_unframed_or_wrong_channel_input),
	KUNIT_CASE(mt6797_md1_hs_accepts_exact_legacy_hs1),
	KUNIT_CASE(mt6797_md1_hs_accepts_exact_v2_hs1),
	KUNIT_CASE(mt6797_md1_hs_refuses_malformed_hs1_without_runtime_action),
	KUNIT_CASE(mt6797_md1_hs_hs2_requires_a_valid_hs1_transition),
	KUNIT_CASE(mt6797_md1_hs_accepts_hs2_after_hs1),
	KUNIT_CASE(mt6797_md1_hs_keeps_hs1_priority_while_waiting_for_hs2),
	KUNIT_CASE(mt6797_md1_hs_rejects_replayed_hs2),
	KUNIT_CASE(mt6797_md1_hs_latches_crash_until_reset),
	{}
};

static struct kunit_suite mt6797_md1_hs_test_suite = {
	.name = "mt6797-md1-hs",
	.test_cases = mt6797_md1_hs_test_cases,
};
kunit_test_suite(mt6797_md1_hs_test_suite);

MODULE_LICENSE("GPL");
