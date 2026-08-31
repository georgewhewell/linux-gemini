// SPDX-License-Identifier: GPL-2.0-only
/* Injected tests for the hardware-free MT6797 CPU8 transition executor. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>

#include "mt6797-a72-transition-internal.h"

#define MT6797_TEST_EVENT(stage, slot) ((unsigned int)(stage) * 4U + (slot))
#define MT6797_TEST_BEFORE 0U
#define MT6797_TEST_EFFECT 1U
#define MT6797_TEST_AFTER 2U
#define MT6797_TEST_PROVIDER_RELEASE 100U
#define MT6797_TEST_P27_RELEASE 101U
#define MT6797_TEST_TERMINAL 102U

struct mt6797_transition_test_state {
	enum mt6797_a72_transition_stage fail_stage;
	enum mt6797_a72_transition_stage malformed_stage;
	enum mt6797_a72_transition_stage checkpoint_fail_stage;
	enum mt6797_a72_transition_phase checkpoint_fail_phase;
	enum mt6797_a72_transition_terminal terminal_seen;
	bool checkpoint_fails;
	bool terminal_fails;
	bool provider_release_fails;
	bool p27_release_fails;
	unsigned int events[64];
	unsigned int event_count;
	unsigned int watchdog_timeout_ms;
	unsigned int cpu_on_target;
	unsigned int secondary_target;
	unsigned int ipi_target;
	unsigned int membership_target;
	unsigned int terminal_count;
};

static void mt6797_test_record(struct mt6797_transition_test_state *state,
			       unsigned int event)
{
	if (state->event_count < ARRAY_SIZE(state->events))
		state->events[state->event_count++] = event;
}

static int mt6797_test_effect(struct mt6797_transition_test_state *state,
			      enum mt6797_a72_transition_stage stage)
{
	mt6797_test_record(state, MT6797_TEST_EVENT(stage, MT6797_TEST_EFFECT));
	return state->fail_stage == stage ? -EIO : 0;
}

static int
mt6797_test_checkpoint(void *context,
		       enum mt6797_a72_transition_phase phase,
		       enum mt6797_a72_transition_stage stage,
		       const struct mt6797_a72_transition_result *result)
{
	struct mt6797_transition_test_state *state = context;
	unsigned int slot = phase == MT6797_A72_TRANSITION_BEFORE ?
		MT6797_TEST_BEFORE : MT6797_TEST_AFTER;

	(void)result;
	mt6797_test_record(state, MT6797_TEST_EVENT(stage, slot));
	if (state->checkpoint_fails &&
	    state->checkpoint_fail_stage == stage &&
	    state->checkpoint_fail_phase == phase)
		return -EUCLEAN;
	return 0;
}

static int mt6797_test_watchdog(void *context, unsigned int timeout_ms,
				u64 *identity)
{
	struct mt6797_transition_test_state *state = context;
	int ret;

	state->watchdog_timeout_ms = timeout_ms;
	ret = mt6797_test_effect(state, MT6797_A72_TRANSITION_STAGE_WATCHDOG);
	if (ret)
		return ret;
	if (state->malformed_stage != MT6797_A72_TRANSITION_STAGE_WATCHDOG)
		*identity = 0x4757415443483031ULL;
	return 0;
}

static int mt6797_test_p27_acquire(void *context, bool *owned)
{
	struct mt6797_transition_test_state *state = context;

	*owned = state->malformed_stage != MT6797_A72_TRANSITION_STAGE_P27;
	return mt6797_test_effect(state, MT6797_A72_TRANSITION_STAGE_P27);
}

static int mt6797_test_p27_release(void *context)
{
	struct mt6797_transition_test_state *state = context;

	mt6797_test_record(state, MT6797_TEST_P27_RELEASE);
	return state->p27_release_fails ? -EREMOTEIO : 0;
}

static int mt6797_test_provider_acquire(void *context, bool *owned)
{
	struct mt6797_transition_test_state *state = context;

	*owned = state->malformed_stage != MT6797_A72_TRANSITION_STAGE_PROVIDER;
	return mt6797_test_effect(state, MT6797_A72_TRANSITION_STAGE_PROVIDER);
}

static int mt6797_test_provider_release(void *context)
{
	struct mt6797_transition_test_state *state = context;

	mt6797_test_record(state, MT6797_TEST_PROVIDER_RELEASE);
	return state->provider_release_fails ? -EREMOTEIO : 0;
}

static int mt6797_test_isolation(void *context)
{
	return mt6797_test_effect(context,
			MT6797_A72_TRANSITION_STAGE_ISOLATION);
}

static int mt6797_test_sram(void *context)
{
	return mt6797_test_effect(context, MT6797_A72_TRANSITION_STAGE_SRAM);
}

static int mt6797_test_cpu_on(void *context, unsigned int cpu)
{
	struct mt6797_transition_test_state *state = context;

	state->cpu_on_target = cpu;
	return mt6797_test_effect(state, MT6797_A72_TRANSITION_STAGE_CPU_ON);
}

static int mt6797_test_secondary(void *context, unsigned int cpu)
{
	struct mt6797_transition_test_state *state = context;

	state->secondary_target = cpu;
	return mt6797_test_effect(state,
			MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
}

static int mt6797_test_ipi(void *context, unsigned int cpu)
{
	struct mt6797_transition_test_state *state = context;

	state->ipi_target = cpu;
	return mt6797_test_effect(state, MT6797_A72_TRANSITION_STAGE_IPI);
}

static int mt6797_test_dcm(void *context)
{
	return mt6797_test_effect(context, MT6797_A72_TRANSITION_STAGE_DCM);
}

static int mt6797_test_membership(void *context, unsigned int cpu)
{
	struct mt6797_transition_test_state *state = context;

	state->membership_target = cpu;
	return mt6797_test_effect(state,
				  MT6797_A72_TRANSITION_STAGE_MEMBERSHIP);
}

static int
mt6797_test_terminal(void *context,
		     const struct mt6797_a72_transition_result *result)
{
	struct mt6797_transition_test_state *state = context;

	state->terminal_seen = result->terminal;
	state->terminal_count++;
	mt6797_test_record(state, MT6797_TEST_TERMINAL);
	return state->terminal_fails ? -ENOSPC : 0;
}

static const struct mt6797_a72_transition_ops mt6797_test_ops = {
	.checkpoint = mt6797_test_checkpoint,
	.watchdog_arm = mt6797_test_watchdog,
	.p27_acquire = mt6797_test_p27_acquire,
	.p27_release = mt6797_test_p27_release,
	.provider_acquire = mt6797_test_provider_acquire,
	.provider_release = mt6797_test_provider_release,
	.isolation_clear = mt6797_test_isolation,
	.sram_enable = mt6797_test_sram,
	.cpu_on = mt6797_test_cpu_on,
	.secondary_complete = mt6797_test_secondary,
	.ipi_proof = mt6797_test_ipi,
	.dcm_update = mt6797_test_dcm,
	.membership_commit = mt6797_test_membership,
	.terminal = mt6797_test_terminal,
};

static struct mt6797_a72_transition_request mt6797_test_request(void)
{
	return (struct mt6797_a72_transition_request) {
		.cpu = MT6797_A72_TRANSITION_CPU8,
		.token_exact = true,
		.prefix_complete = true,
	};
}

static int mt6797_test_run(struct mt6797_transition_test_state *state,
			   const struct mt6797_a72_transition_request *request,
			   struct mt6797_a72_transition_result *result)
{
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;

	return mt6797_a72_transition_run(&controller, &mt6797_test_ops,
					  state, request, result);
}

static void mt6797_transition_split_success_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result;
	enum mt6797_a72_transition_stage stage;
	unsigned int event = 0;
	int ret;

	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&controller.lifecycle),
			MT6797_A72_TRANSITION_LIFECYCLE_CPU_ON_ACCEPTED);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_TERMINAL_NONE);
	KUNIT_EXPECT_TRUE(test, result.cpu_on_accepted);
	KUNIT_EXPECT_FALSE(test, result.cpu8_online);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 12U);
	KUNIT_EXPECT_EQ(test, state.event_count, 18U);
	KUNIT_EXPECT_EQ(test, state.secondary_target, 0U);
	KUNIT_EXPECT_EQ(test, state.ipi_target, 0U);

	ret = mt6797_a72_transition_secondary_complete(&controller,
						       &mt6797_test_ops, &state,
						       MT6797_A72_TRANSITION_CPU8,
						       true, false, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&controller.lifecycle),
			MT6797_A72_TRANSITION_LIFECYCLE_SECONDARY_COMPLETE);
	KUNIT_EXPECT_TRUE(test, result.cpu8_online);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 14U);
	KUNIT_EXPECT_EQ(test, state.event_count, 21U);
	KUNIT_EXPECT_EQ(test, state.secondary_target,
			MT6797_A72_TRANSITION_CPU8);
	KUNIT_EXPECT_EQ(test, state.ipi_target, 0U);

	ret = mt6797_a72_transition_complete(&controller, &mt6797_test_ops,
					     &state, MT6797_A72_TRANSITION_CPU8,
					     true, false, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&controller.lifecycle),
			MT6797_A72_TRANSITION_LIFECYCLE_TERMINAL);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF);
	KUNIT_EXPECT_EQ(test, result.cpu_requests, 1U);
	KUNIT_EXPECT_EQ(test, result.cpu_off_requests, 0U);
	KUNIT_EXPECT_EQ(test, result.retries, 0U);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 20U);
	KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
	KUNIT_EXPECT_EQ(test, result.checkpoint_errno, 0);
	KUNIT_EXPECT_TRUE(test, result.membership_published);
	KUNIT_EXPECT_EQ(test, result.retained_mask,
			(u32)(MT6797_A72_TRANSITION_OWNED_P27 |
			      MT6797_A72_TRANSITION_OWNED_PROVIDER |
			      MT6797_A72_TRANSITION_OWNED_CPU8));
	KUNIT_EXPECT_EQ(test, state.watchdog_timeout_ms,
			MT6797_A72_TRANSITION_RECOVERY_MS);
	KUNIT_EXPECT_EQ(test, state.cpu_on_target,
			MT6797_A72_TRANSITION_CPU8);
	KUNIT_EXPECT_EQ(test, state.ipi_target,
			MT6797_A72_TRANSITION_CPU8);
	KUNIT_EXPECT_EQ(test, state.membership_target,
			MT6797_A72_TRANSITION_CPU8);
	KUNIT_EXPECT_EQ(test, state.terminal_count, 1U);
	KUNIT_EXPECT_EQ(test, state.terminal_seen,
			MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF);
	KUNIT_ASSERT_EQ(test, state.event_count, 31U);
	for (stage = MT6797_A72_TRANSITION_STAGE_WATCHDOG;
	     stage < MT6797_A72_TRANSITION_STAGE_COUNT; stage++) {
		KUNIT_EXPECT_EQ(test, state.events[event++],
				MT6797_TEST_EVENT(stage, MT6797_TEST_BEFORE));
		KUNIT_EXPECT_EQ(test, state.events[event++],
				MT6797_TEST_EVENT(stage, MT6797_TEST_EFFECT));
		KUNIT_EXPECT_EQ(test, state.events[event++],
				MT6797_TEST_EVENT(stage, MT6797_TEST_AFTER));
	}
	KUNIT_EXPECT_EQ(test, state.events[event++], MT6797_TEST_TERMINAL);
}

static void mt6797_transition_composed_run_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result;
	int ret;

	ret = mt6797_test_run(&state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 20U);
	KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
	KUNIT_EXPECT_TRUE(test, result.membership_published);
	KUNIT_EXPECT_EQ(test, state.event_count, 31U);
}

static void mt6797_transition_entry_rejections_test(struct kunit *test)
{
	struct mt6797_a72_transition_request requests[] = {
		{ .cpu = MT6797_A72_TRANSITION_CPU9,
		  .token_exact = true, .prefix_complete = true },
		{ .cpu = MT6797_A72_TRANSITION_CPU8,
		  .prefix_complete = true },
		{ .cpu = MT6797_A72_TRANSITION_CPU8,
		  .token_exact = true },
		{ .cpu = MT6797_A72_TRANSITION_CPU8,
		  .token_exact = true, .prefix_complete = true,
		  .cpu8_online = true },
		{ .cpu = MT6797_A72_TRANSITION_CPU8,
		  .token_exact = true, .prefix_complete = true,
		  .cpu9_online = true },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(requests); i++) {
		struct mt6797_transition_test_state state = { };
		struct mt6797_a72_transition_result result;
		int ret;

		ret = mt6797_test_run(&state, &requests[i], &result);
		KUNIT_EXPECT_LT(test, ret, 0);
		KUNIT_EXPECT_EQ(test, result.terminal,
				MT6797_A72_TRANSITION_REJECTED_PRESTATE);
		KUNIT_EXPECT_FALSE(test, result.attempted);
		KUNIT_EXPECT_EQ(test, result.checkpoints, 0U);
		KUNIT_EXPECT_EQ(test, state.event_count, 0U);
	}
}

static void mt6797_transition_missing_op_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result;
	struct mt6797_a72_transition_ops ops = mt6797_test_ops;
	int ret;

	ops.terminal = NULL;
	ret = mt6797_a72_transition_begin(&controller, &ops, &state,
					  &request, &result);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, result.attempted);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&controller.consumed), 0);
}

static void mt6797_transition_one_shot_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result;
	unsigned int events;
	int ret;

	ret = mt6797_a72_transition_run(&controller, &mt6797_test_ops, &state,
					&request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	events = state.event_count;
	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_TRUE(test, result.attempted);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 20U);
	KUNIT_EXPECT_EQ(test, result.cpu_requests, 1U);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF);
	KUNIT_EXPECT_EQ(test, state.event_count, events);
}

static void mt6797_transition_stage_failures_test(struct kunit *test)
{
	enum mt6797_a72_transition_stage stage;

	for (stage = MT6797_A72_TRANSITION_STAGE_WATCHDOG;
	     stage < MT6797_A72_TRANSITION_STAGE_COUNT; stage++) {
		struct mt6797_a72_transition_request request =
			mt6797_test_request();
		struct mt6797_transition_test_state state = {
			.fail_stage = stage,
		};
		struct mt6797_a72_transition_result result;
		u32 expected_retained =
			MT6797_A72_TRANSITION_OWNED_P27 |
			MT6797_A72_TRANSITION_OWNED_PROVIDER;
		int ret;

		ret = mt6797_test_run(&state, &request, &result);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EIO, "stage=%u", stage);
		KUNIT_EXPECT_EQ_MSG(test, result.cpu_off_requests, 0U,
				    "stage=%u", stage);
		KUNIT_EXPECT_EQ_MSG(test, result.terminal_commits, 1U,
				    "stage=%u", stage);
		KUNIT_EXPECT_EQ_MSG(test, state.terminal_count, 1U,
				    "stage=%u", stage);
		KUNIT_EXPECT_EQ_MSG(test, result.retries, 0U,
				    "stage=%u", stage);
		KUNIT_EXPECT_FALSE(test, result.cpu9_online);
		KUNIT_EXPECT_EQ_MSG(test, result.checkpoints,
				    (unsigned int)(stage -
				     MT6797_A72_TRANSITION_STAGE_WATCHDOG) *
				    2U + 1U, "stage=%u", stage);
		if (stage == MT6797_A72_TRANSITION_STAGE_WATCHDOG) {
			KUNIT_EXPECT_EQ(test, result.terminal,
					MT6797_A72_TRANSITION_REJECTED_PRESTATE);
			KUNIT_EXPECT_FALSE(test, result.watchdog_armed);
			continue;
		}
		KUNIT_EXPECT_TRUE(test, result.watchdog_armed);
		if (stage == MT6797_A72_TRANSITION_STAGE_P27 ||
		    stage == MT6797_A72_TRANSITION_STAGE_PROVIDER) {
			KUNIT_EXPECT_EQ(test, result.terminal,
					MT6797_A72_TRANSITION_ROLLED_BACK_PREISO);
			KUNIT_EXPECT_FALSE(test, result.p27_owned);
			KUNIT_EXPECT_FALSE(test, result.provider_owned);
			KUNIT_EXPECT_EQ(test, result.retained_mask, 0U);
			continue;
		}
		if (stage >= MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT)
			expected_retained |= MT6797_A72_TRANSITION_OWNED_CPU8;
		KUNIT_EXPECT_EQ(test, result.terminal,
				MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
		KUNIT_EXPECT_TRUE(test, result.isolation_attempted);
		KUNIT_EXPECT_EQ(test, result.isolation_crossed,
				stage != MT6797_A72_TRANSITION_STAGE_ISOLATION);
		KUNIT_EXPECT_EQ(test, result.retained_mask, expected_retained);
		KUNIT_EXPECT_EQ(test, result.cpu_requests,
				stage >= MT6797_A72_TRANSITION_STAGE_CPU_ON ?
				1U : 0U);
		KUNIT_EXPECT_EQ(test, result.cpu8_online,
				stage >= MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
		KUNIT_EXPECT_EQ(test, result.membership_published,
				stage > MT6797_A72_TRANSITION_STAGE_MEMBERSHIP);
	}
}

static void mt6797_transition_lifecycle_failure_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result;
	unsigned int events;
	int ret;

	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_transition_fail(&controller, &mt6797_test_ops,
					 &state, MT6797_A72_TRANSITION_CPU8,
					 false, false, -ETIMEDOUT, &result);
	KUNIT_EXPECT_EQ(test, ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
	KUNIT_EXPECT_FALSE(test, result.cpu8_online);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 13U);
	KUNIT_EXPECT_EQ(test, result.last_stage,
			MT6797_A72_TRANSITION_STAGE_ONLINE_WAIT);
	events = state.event_count;
	ret = mt6797_a72_transition_fail(&controller, &mt6797_test_ops,
					 &state, MT6797_A72_TRANSITION_CPU8,
					 false, false, -EIO, &result);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, state.event_count, events);

	controller = (struct mt6797_a72_transition_controller)
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	state = (struct mt6797_transition_test_state) { };
	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_transition_secondary_complete(&controller,
						       &mt6797_test_ops, &state,
						       MT6797_A72_TRANSITION_CPU8,
						       true, false, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_transition_fail(&controller, &mt6797_test_ops,
					 &state, MT6797_A72_TRANSITION_CPU8,
					 true, false, -ENOMEM, &result);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_TRUE(test, result.cpu8_online);
	KUNIT_EXPECT_EQ(test, result.checkpoints, 14U);
	KUNIT_EXPECT_EQ(test, result.retained_mask,
			(u32)(MT6797_A72_TRANSITION_OWNED_P27 |
			      MT6797_A72_TRANSITION_OWNED_PROVIDER |
			      MT6797_A72_TRANSITION_OWNED_CPU8));
}

static void mt6797_transition_handoff_guards_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_a72_transition_controller controller =
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	struct mt6797_transition_test_state state = { };
	struct mt6797_a72_transition_result result = { };
	unsigned int events;
	int ret;

	ret = mt6797_a72_transition_complete(&controller, &mt6797_test_ops,
					     &state, MT6797_A72_TRANSITION_CPU8,
					     true, false, &result);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, state.event_count, 0U);

	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_transition_secondary_complete(&controller,
						       &mt6797_test_ops, &state,
						       MT6797_A72_TRANSITION_CPU9,
						       true, false, &result);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
	KUNIT_EXPECT_EQ(test, result.cpu_off_requests, 0U);
	KUNIT_EXPECT_EQ(test, result.retries, 0U);

	controller = (struct mt6797_a72_transition_controller)
		MT6797_A72_TRANSITION_CONTROLLER_INIT;
	state = (struct mt6797_transition_test_state) { };
	ret = mt6797_a72_transition_begin(&controller, &mt6797_test_ops,
					  &state, &request, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_a72_transition_secondary_complete(&controller,
						       &mt6797_test_ops, &state,
						       MT6797_A72_TRANSITION_CPU8,
						       true, false, &result);
	KUNIT_ASSERT_EQ(test, ret, 0);
	events = state.event_count;
	ret = mt6797_a72_transition_secondary_complete(&controller,
						       &mt6797_test_ops, &state,
						       MT6797_A72_TRANSITION_CPU8,
						       true, false, &result);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	KUNIT_EXPECT_EQ(test, state.event_count, events);
	ret = mt6797_a72_transition_complete(&controller, &mt6797_test_ops,
					     &state, MT6797_A72_TRANSITION_CPU8,
					     false, false, &result);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_FALSE(test, result.cpu8_online);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
}

static void mt6797_transition_malformed_ownership_test(struct kunit *test)
{
	static const enum mt6797_a72_transition_stage stages[] = {
		MT6797_A72_TRANSITION_STAGE_WATCHDOG,
		MT6797_A72_TRANSITION_STAGE_P27,
		MT6797_A72_TRANSITION_STAGE_PROVIDER,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(stages); i++) {
		struct mt6797_a72_transition_request request =
			mt6797_test_request();
		struct mt6797_transition_test_state state = {
			.malformed_stage = stages[i],
		};
		struct mt6797_a72_transition_result result;
		int ret;

		ret = mt6797_test_run(&state, &request, &result);
		KUNIT_EXPECT_EQ(test, ret, -EPROTO);
		if (stages[i] == MT6797_A72_TRANSITION_STAGE_WATCHDOG) {
			KUNIT_EXPECT_EQ(test, result.terminal,
					MT6797_A72_TRANSITION_REJECTED_PRESTATE);
			KUNIT_EXPECT_FALSE(test, result.watchdog_armed);
		} else {
			KUNIT_EXPECT_EQ(test, result.terminal,
					MT6797_A72_TRANSITION_ROLLBACK_FAULT_PREISO);
			KUNIT_EXPECT_NE(test, result.retained_mask, 0U);
		}
	}
}

static void mt6797_transition_rollback_faults_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	struct mt6797_transition_test_state state = {
		.fail_stage = MT6797_A72_TRANSITION_STAGE_P27,
		.p27_release_fails = true,
	};
	struct mt6797_a72_transition_result result;
	int ret;

	ret = mt6797_test_run(&state, &request, &result);
	KUNIT_EXPECT_EQ(test, ret, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, result.terminal,
			MT6797_A72_TRANSITION_ROLLBACK_FAULT_PREISO);
	KUNIT_EXPECT_EQ(test, result.rollback_errno, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, result.retained_mask,
			(u32)MT6797_A72_TRANSITION_OWNED_P27);

	state = (struct mt6797_transition_test_state) {
		.fail_stage = MT6797_A72_TRANSITION_STAGE_PROVIDER,
		.provider_release_fails = true,
	};
	ret = mt6797_test_run(&state, &request, &result);
	KUNIT_EXPECT_EQ(test, ret, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, result.retained_mask,
			(u32)(MT6797_A72_TRANSITION_OWNED_P27 |
			      MT6797_A72_TRANSITION_OWNED_PROVIDER));

	state = (struct mt6797_transition_test_state) {
		.fail_stage = MT6797_A72_TRANSITION_STAGE_PROVIDER,
		.p27_release_fails = true,
	};
	ret = mt6797_test_run(&state, &request, &result);
	KUNIT_EXPECT_EQ(test, ret, -EREMOTEIO);
	KUNIT_EXPECT_EQ(test, result.rollback_mask,
			(u32)MT6797_A72_TRANSITION_OWNED_PROVIDER);
	KUNIT_EXPECT_EQ(test, result.retained_mask,
			(u32)MT6797_A72_TRANSITION_OWNED_P27);
}

static void mt6797_transition_checkpoint_failures_test(struct kunit *test)
{
	enum mt6797_a72_transition_stage stage;

	for (stage = MT6797_A72_TRANSITION_STAGE_WATCHDOG;
	     stage < MT6797_A72_TRANSITION_STAGE_COUNT; stage++) {
		enum mt6797_a72_transition_phase phase;

		for (phase = MT6797_A72_TRANSITION_BEFORE;
		     phase <= MT6797_A72_TRANSITION_AFTER; phase++) {
			struct mt6797_a72_transition_request request =
				mt6797_test_request();
			struct mt6797_transition_test_state state = {
				.checkpoint_fails = true,
				.checkpoint_fail_stage = stage,
				.checkpoint_fail_phase = phase,
			};
			struct mt6797_a72_transition_result result;
			bool preisolation;
			int ret;

			ret = mt6797_test_run(&state, &request, &result);
			KUNIT_EXPECT_EQ_MSG(test, ret, -EUCLEAN,
					    "stage=%u phase=%u", stage, phase);
			KUNIT_EXPECT_EQ(test, result.checkpoint_errno, -EUCLEAN);
			KUNIT_EXPECT_EQ(test, result.stage_errno, -EUCLEAN);
			KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
			KUNIT_EXPECT_EQ(test, state.terminal_count, 1U);
			KUNIT_EXPECT_EQ(test, result.cpu_off_requests, 0U);
			KUNIT_EXPECT_EQ(test, result.retries, 0U);
			preisolation =
				stage < MT6797_A72_TRANSITION_STAGE_ISOLATION ||
				(stage == MT6797_A72_TRANSITION_STAGE_ISOLATION &&
				 phase == MT6797_A72_TRANSITION_BEFORE);
			if (stage == MT6797_A72_TRANSITION_STAGE_WATCHDOG &&
			    phase == MT6797_A72_TRANSITION_BEFORE)
				KUNIT_EXPECT_EQ(test, result.terminal,
						MT6797_A72_TRANSITION_REJECTED_PRESTATE);
			else if (preisolation)
				KUNIT_EXPECT_EQ(test, result.terminal,
						MT6797_A72_TRANSITION_ROLLED_BACK_PREISO);
			else
				KUNIT_EXPECT_EQ(test, result.terminal,
						MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
			KUNIT_EXPECT_EQ(test, result.membership_published,
					stage == MT6797_A72_TRANSITION_STAGE_MEMBERSHIP &&
					phase == MT6797_A72_TRANSITION_AFTER);
		}
	}
}

static void mt6797_transition_terminal_failures_test(struct kunit *test)
{
	struct mt6797_a72_transition_request request = mt6797_test_request();
	enum mt6797_a72_transition_stage stage;

	for (stage = MT6797_A72_TRANSITION_STAGE_WATCHDOG;
	     stage < MT6797_A72_TRANSITION_STAGE_COUNT; stage++) {
		struct mt6797_transition_test_state state = {
			.fail_stage = stage,
			.terminal_fails = true,
		};
		struct mt6797_a72_transition_result result;
		int ret;

		ret = mt6797_test_run(&state, &request, &result);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EIO, "stage=%u", stage);
		KUNIT_EXPECT_EQ(test, result.stage_errno, -EIO);
		KUNIT_EXPECT_EQ(test, result.checkpoint_errno, -ENOSPC);
		KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
		KUNIT_EXPECT_EQ(test, state.terminal_count, 1U);
	}
	for (stage = MT6797_A72_TRANSITION_STAGE_WATCHDOG;
	     stage < MT6797_A72_TRANSITION_STAGE_COUNT; stage++) {
		enum mt6797_a72_transition_phase phase;

		for (phase = MT6797_A72_TRANSITION_BEFORE;
		     phase <= MT6797_A72_TRANSITION_AFTER; phase++) {
			struct mt6797_transition_test_state state = {
				.checkpoint_fails = true,
				.checkpoint_fail_stage = stage,
				.checkpoint_fail_phase = phase,
				.terminal_fails = true,
			};
			struct mt6797_a72_transition_result result;
			int ret;

			ret = mt6797_test_run(&state, &request, &result);
			KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
			KUNIT_EXPECT_EQ(test, result.stage_errno, -EUCLEAN);
			KUNIT_EXPECT_EQ(test, result.checkpoint_errno, -ENOSPC);
			KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
			KUNIT_EXPECT_EQ(test, state.terminal_count, 1U);
		}
	}
	{
		struct mt6797_transition_test_state state = {
			.terminal_fails = true,
		};
		struct mt6797_a72_transition_result result;
		int ret;

		ret = mt6797_test_run(&state, &request, &result);
		KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
		KUNIT_EXPECT_TRUE(test, result.membership_published);
		KUNIT_EXPECT_EQ(test, result.terminal,
				MT6797_A72_TRANSITION_FAULT_RETAIN_POSTISO);
		KUNIT_EXPECT_EQ(test, state.terminal_seen,
				MT6797_A72_TRANSITION_CPU8_ONLINE_PROOF);
		KUNIT_EXPECT_EQ(test, result.checkpoint_errno, -ENOSPC);
		KUNIT_EXPECT_EQ(test, result.terminal_commits, 1U);
		KUNIT_EXPECT_EQ(test, state.terminal_count, 1U);
	}
}

static struct kunit_case mt6797_transition_cases[] = {
	KUNIT_CASE(mt6797_transition_split_success_test),
	KUNIT_CASE(mt6797_transition_composed_run_test),
	KUNIT_CASE(mt6797_transition_entry_rejections_test),
	KUNIT_CASE(mt6797_transition_missing_op_test),
	KUNIT_CASE(mt6797_transition_one_shot_test),
	KUNIT_CASE(mt6797_transition_stage_failures_test),
	KUNIT_CASE(mt6797_transition_checkpoint_failures_test),
	KUNIT_CASE(mt6797_transition_terminal_failures_test),
	KUNIT_CASE(mt6797_transition_lifecycle_failure_test),
	KUNIT_CASE(mt6797_transition_handoff_guards_test),
	KUNIT_CASE(mt6797_transition_malformed_ownership_test),
	KUNIT_CASE(mt6797_transition_rollback_faults_test),
	{ }
};

static struct kunit_suite mt6797_transition_suite = {
	.name = "mt6797-a72-transition-executor",
	.test_cases = mt6797_transition_cases,
};

kunit_test_suite(mt6797_transition_suite);

MODULE_LICENSE("GPL");
