// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_runtime.h"

#define MT6797_WIFI_AIS_TEST_EVENT_CHANNEL	0x10U
#define MT6797_WIFI_AIS_TEST_GRANT_INTERVAL_MS	1000U
#define MT6797_WIFI_AIS_TEST_TIMEOUT_MS		1234U
#define MT6797_WIFI_AIS_TEST_MAX_CALLS		8U

enum mt6797_wifi_ais_test_wait {
	MT6797_WIFI_AIS_TEST_WAIT_GRANT,
	MT6797_WIFI_AIS_TEST_WAIT_FOREIGN_THEN_GRANT,
	MT6797_WIFI_AIS_TEST_WAIT_TIMEOUT,
	MT6797_WIFI_AIS_TEST_WAIT_GRANT_AT_TIMEOUT,
	MT6797_WIFI_AIS_TEST_WAIT_MALFORMED,
	MT6797_WIFI_AIS_TEST_WAIT_EMPTY,
	MT6797_WIFI_AIS_TEST_WAIT_ERROR,
};

struct mt6797_wifi_ais_test_context {
	struct mt6797_wifi_ais_runtime *runtime;
	struct mt6797_wifi_ais_runtime_ops *ops;
	enum mt6797_wifi_ais_runtime_command call[MT6797_WIFI_AIS_TEST_MAX_CALLS];
	u8 sequence[MT6797_WIFI_AIS_TEST_MAX_CALLS];
	unsigned int calls;
	unsigned int fences;
	unsigned int timeout_ms;
	int event_ret;
	int fence_reason;
	int precheck_ret;
	int send_fail;
	u8 wait_mode;
	u8 event_disposition;
	bool dirty_violation;
	bool sent_violation;
	bool foreign_ignored;
};

struct mt6797_wifi_ais_test_fixture {
	struct mt6797_wifi_ais_runtime runtime;
	struct mt6797_wifi_ais_runtime_params params;
	struct mt6797_wifi_ais_runtime_ops ops;
	struct mt6797_wifi_ais_test_context context;
};

static const u8 mt6797_wifi_ais_runtime_test_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_ais_runtime_test_peer_addr[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static int mt6797_wifi_ais_test_precheck(void *data)
{
	struct mt6797_wifi_ais_test_context *context = data;

	return context->precheck_ret;
}

static int mt6797_wifi_ais_test_send(
	void *data, const struct mt6797_wifi_ais_runtime *runtime,
	enum mt6797_wifi_ais_runtime_command command, const u8 *packet,
	size_t length)
{
	struct mt6797_wifi_ais_test_context *context = data;

	if (command >= MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT || !packet ||
	    !length || context->calls >= MT6797_WIFI_AIS_TEST_MAX_CALLS)
		return -EINVAL;
	if (!runtime->dirty[command])
		context->dirty_violation = true;
	if (runtime->sent[command])
		context->sent_violation = true;
	context->call[context->calls] = command;
	context->sequence[context->calls] = packet[7];
	context->calls++;
	return context->send_fail == command ? -EIO : 0;
}

static void mt6797_wifi_ais_test_event(
	struct mt6797_wifi_ais_test_context *context, bool foreign,
	bool malformed)
{
	u8 payload[MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH -
		   MT6797_WIFI_EVENT_HEADER_LENGTH] = {};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_AIS_CHANNEL_EVENT_LENGTH,
		.id = MT6797_WIFI_AIS_TEST_EVENT_CHANNEL,
		.sequence = 0xee,
	};
	enum mt6797_wifi_ais_event_disposition disposition;

	payload[0] = context->runtime->channel_pending.bss_index;
	payload[1] = context->runtime->channel_pending.token + foreign;
	payload[3] = context->runtime->channel_pending.primary;
	payload[5] = context->runtime->channel_pending.band;
	put_unaligned_le32(MT6797_WIFI_AIS_TEST_GRANT_INTERVAL_MS,
			   payload + 12);
	if (malformed) {
		event.payload_length = 2;
		event.packet_length = MT6797_WIFI_EVENT_HEADER_LENGTH + 2;
	}
	context->event_ret = mt6797_wifi_ais_runtime_event(context->runtime,
		context->ops, &event, &disposition);
	context->event_disposition = disposition;
	if (foreign)
		context->foreign_ignored =
			!context->event_ret &&
			disposition == MT6797_WIFI_AIS_EVENT_IGNORED;
}

static int mt6797_wifi_ais_test_wait(void *data, unsigned int timeout_ms)
{
	struct mt6797_wifi_ais_test_context *context = data;

	context->timeout_ms = timeout_ms;
	switch (context->wait_mode) {
	case MT6797_WIFI_AIS_TEST_WAIT_GRANT:
		mt6797_wifi_ais_test_event(context, false, false);
		return context->event_ret;
	case MT6797_WIFI_AIS_TEST_WAIT_FOREIGN_THEN_GRANT:
		mt6797_wifi_ais_test_event(context, true, false);
		if (context->event_ret)
			return context->event_ret;
		mt6797_wifi_ais_test_event(context, false, false);
		return context->event_ret;
	case MT6797_WIFI_AIS_TEST_WAIT_TIMEOUT:
		return -ETIMEDOUT;
	case MT6797_WIFI_AIS_TEST_WAIT_GRANT_AT_TIMEOUT:
		mt6797_wifi_ais_test_event(context, false, false);
		return context->event_ret ?: -ETIMEDOUT;
	case MT6797_WIFI_AIS_TEST_WAIT_MALFORMED:
		mt6797_wifi_ais_test_event(context, false, true);
		return context->event_ret;
	case MT6797_WIFI_AIS_TEST_WAIT_EMPTY:
		return 0;
	case MT6797_WIFI_AIS_TEST_WAIT_ERROR:
		return -EINTR;
	default:
		return -EINVAL;
	}
}

static void mt6797_wifi_ais_test_fence(void *data, int reason)
{
	struct mt6797_wifi_ais_test_context *context = data;

	context->fences++;
	context->fence_reason = reason;
}

static void mt6797_wifi_ais_test_init(
	struct mt6797_wifi_ais_test_fixture *fixture)
{
	struct mt6797_wifi_ais_runtime *runtime = &fixture->runtime;
	struct mt6797_wifi_ais_runtime_ops *ops = &fixture->ops;
	struct mt6797_wifi_ais_runtime_params *params = &fixture->params;
	struct mt6797_wifi_ais_test_context *context = &fixture->context;

	memset(context, 0, sizeof(*context));
	context->runtime = runtime;
	context->ops = ops;
	context->send_fail = MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT;
	context->wait_mode = MT6797_WIFI_AIS_TEST_WAIT_GRANT;
	*ops = (struct mt6797_wifi_ais_runtime_ops) {
		.precheck = mt6797_wifi_ais_test_precheck,
		.send = mt6797_wifi_ais_test_send,
		.wait_channel = mt6797_wifi_ais_test_wait,
		.fence = mt6797_wifi_ais_test_fence,
		.context = context,
	};
	*params = (struct mt6797_wifi_ais_runtime_params) {
		.channel = {
			.band = MT6797_WIFI_AIS_BAND_2GHZ,
			.primary = 6,
		},
		.bss_index = 3,
		.own_mac_index = 1,
		.response_timeout_ms = MT6797_WIFI_AIS_TEST_TIMEOUT_MS,
	};
	memcpy(params->own_addr, mt6797_wifi_ais_runtime_test_own_addr,
	       ETH_ALEN);
	memcpy(params->peer_addr, mt6797_wifi_ais_runtime_test_peer_addr,
	       ETH_ALEN);
	mt6797_wifi_ais_runtime_init(runtime, 3, 1);
}

static struct mt6797_wifi_ais_test_fixture *
mt6797_wifi_ais_test_fixture(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (fixture)
		mt6797_wifi_ais_test_init(fixture);
	return fixture;
}

static void mt6797_wifi_ais_runtime_order_test(struct kunit *test)
{
	static const enum mt6797_wifi_ais_runtime_command expected[] = {
		MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE,
		MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST,
		MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE,
		MT6797_WIFI_AIS_RUNTIME_STA_REMOVE,
		MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT,
		MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE,
	};
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), 0);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_STA_STATE1);
	KUNIT_EXPECT_EQ(test, fixture->context.timeout_ms,
			MT6797_WIFI_AIS_TEST_TIMEOUT_MS);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_runtime_stop(&fixture->runtime,
							  &fixture->ops), 0);
	KUNIT_ASSERT_EQ(test, fixture->context.calls, ARRAY_SIZE(expected));
	for (i = 0; i < ARRAY_SIZE(expected); i++) {
		KUNIT_EXPECT_EQ(test, fixture->context.call[i], expected[i]);
		KUNIT_EXPECT_EQ(test, fixture->context.sequence[i], (u8)(i + 3));
		KUNIT_EXPECT_TRUE(test, fixture->runtime.dirty[i]);
		KUNIT_EXPECT_TRUE(test, fixture->runtime.sent[i]);
	}
	KUNIT_EXPECT_FALSE(test, fixture->context.dirty_violation);
	KUNIT_EXPECT_FALSE(test, fixture->context.sent_violation);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 0U);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_STOPPED);
	KUNIT_EXPECT_EQ(test, fixture->runtime.resources.state,
			MT6797_WIFI_AIS_RESOURCES_CLOSED);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), -EALREADY);
}

static void mt6797_wifi_ais_runtime_foreign_event_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode =
		MT6797_WIFI_AIS_TEST_WAIT_FOREIGN_THEN_GRANT;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), 0);
	KUNIT_EXPECT_TRUE(test, fixture->context.foreign_ignored);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 0U);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_STA_STATE1);
	mt6797_wifi_ais_test_event(&fixture->context, false, false);
	KUNIT_EXPECT_EQ(test, fixture->context.event_ret, 0);
	KUNIT_EXPECT_EQ(test, fixture->context.event_disposition,
			MT6797_WIFI_AIS_EVENT_IGNORED);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 0U);
}

static void mt6797_wifi_ais_runtime_timeout_race_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_GRANT_AT_TIMEOUT;
	KUNIT_ASSERT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), 0);
	KUNIT_EXPECT_EQ(test, fixture->context.calls, 3U);
	KUNIT_EXPECT_EQ(test, fixture->context.call[2],
			MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 0U);
}

static void mt6797_wifi_ais_runtime_timeout_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_TIMEOUT;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params),
		-ETIMEDOUT);
	KUNIT_ASSERT_EQ(test, fixture->context.calls, 3U);
	KUNIT_EXPECT_EQ(test, fixture->context.call[2],
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT);
	KUNIT_EXPECT_TRUE(test,
		fixture->runtime.dirty[MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT]);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
	KUNIT_EXPECT_EQ(test, fixture->context.fence_reason, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_TERMINAL);
	KUNIT_EXPECT_EQ(test, fixture->runtime.resources.state,
			MT6797_WIFI_AIS_RESOURCES_TERMINAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params),
		-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
}

static void mt6797_wifi_ais_runtime_malformed_event_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_MALFORMED;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture->context.calls, 2U);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_TERMINAL);
}

static void mt6797_wifi_ais_runtime_wait_failures_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_EMPTY;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);

	fixture = mt6797_wifi_ais_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_ERROR;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), -EINTR);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
}

static void mt6797_wifi_ais_runtime_forward_failures_test(struct kunit *test)
{
	unsigned int command;

	for (command = MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE;
	     command <= MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE; command++) {
		struct mt6797_wifi_ais_test_fixture *fixture =
			mt6797_wifi_ais_test_fixture(test);

		KUNIT_ASSERT_NOT_NULL(test, fixture);
		fixture->context.send_fail = command;
		KUNIT_EXPECT_EQ(test,
			mt6797_wifi_ais_runtime_start(&fixture->runtime,
				&fixture->ops, &fixture->params),
			-EIO);
		KUNIT_EXPECT_EQ(test, fixture->context.calls, command + 1);
		KUNIT_EXPECT_EQ(test, fixture->context.call[command], command);
		KUNIT_EXPECT_TRUE(test, fixture->runtime.dirty[command]);
		KUNIT_EXPECT_FALSE(test, fixture->runtime.sent[command]);
		KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
		KUNIT_EXPECT_EQ(test, fixture->runtime.state,
				MT6797_WIFI_AIS_RUNTIME_TERMINAL);
	}
}

static void mt6797_wifi_ais_runtime_reverse_failures_test(struct kunit *test)
{
	unsigned int command;

	for (command = MT6797_WIFI_AIS_RUNTIME_STA_REMOVE;
	     command <= MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE; command++) {
		struct mt6797_wifi_ais_test_fixture *fixture =
			mt6797_wifi_ais_test_fixture(test);

		KUNIT_ASSERT_NOT_NULL(test, fixture);
		KUNIT_ASSERT_EQ(test,
			mt6797_wifi_ais_runtime_start(&fixture->runtime,
				&fixture->ops, &fixture->params), 0);
		fixture->context.send_fail = command;
		KUNIT_EXPECT_EQ(test,
			mt6797_wifi_ais_runtime_stop(&fixture->runtime,
						     &fixture->ops), -EIO);
		KUNIT_EXPECT_EQ(test, fixture->context.calls, command + 1);
		KUNIT_EXPECT_EQ(test, fixture->context.call[command], command);
		KUNIT_EXPECT_TRUE(test, fixture->runtime.dirty[command]);
		KUNIT_EXPECT_FALSE(test, fixture->runtime.sent[command]);
		KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
		KUNIT_EXPECT_EQ(test, fixture->runtime.state,
				MT6797_WIFI_AIS_RUNTIME_TERMINAL);
	}
}

static void mt6797_wifi_ais_runtime_overflow_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	mt6797_wifi_ais_runtime_init(&fixture->runtime, 251, 1);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params),
		-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, fixture->context.calls, 0U);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_TERMINAL);

	fixture = mt6797_wifi_ais_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	mt6797_wifi_ais_runtime_init(&fixture->runtime, 3, 256);
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params),
		-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
}

static void mt6797_wifi_ais_runtime_abort_failure_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_TEST_WAIT_TIMEOUT;
	fixture->context.send_fail = MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params),
		-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->context.calls, 3U);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 1U);
	KUNIT_EXPECT_EQ(test, fixture->context.fence_reason, -ETIMEDOUT);
}

static void mt6797_wifi_ais_runtime_precheck_test(struct kunit *test)
{
	struct mt6797_wifi_ais_test_fixture *fixture =
		mt6797_wifi_ais_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.precheck_ret = -EAGAIN;
	KUNIT_EXPECT_EQ(test,
		mt6797_wifi_ais_runtime_start(&fixture->runtime, &fixture->ops,
					      &fixture->params), -EAGAIN);
	KUNIT_EXPECT_EQ(test, fixture->runtime.state,
			MT6797_WIFI_AIS_RUNTIME_IDLE);
	KUNIT_EXPECT_EQ(test, fixture->context.calls, 0U);
	KUNIT_EXPECT_EQ(test, fixture->context.fences, 0U);
}

static struct kunit_case mt6797_wifi_ais_runtime_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_runtime_order_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_foreign_event_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_timeout_race_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_timeout_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_malformed_event_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_wait_failures_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_forward_failures_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_reverse_failures_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_overflow_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_abort_failure_test),
	KUNIT_CASE(mt6797_wifi_ais_runtime_precheck_test),
	{}
};

static struct kunit_suite mt6797_wifi_ais_runtime_suite = {
	.name = "mt6797-wifi-ais-runtime",
	.test_cases = mt6797_wifi_ais_runtime_cases,
};

kunit_test_suite(mt6797_wifi_ais_runtime_suite);

MODULE_LICENSE("GPL");
