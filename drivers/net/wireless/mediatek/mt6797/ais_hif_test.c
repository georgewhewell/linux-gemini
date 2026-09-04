// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_hif.h"
#include "boot.h"
#include "capability.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "runtime_config.h"
#include "scan_target.h"

#define MT6797_WIFI_AIS_HIF_TEST_NOW_MS		100U
#define MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS	1000U
#define MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS	8U

enum mt6797_wifi_ais_hif_test_wait_mode {
	MT6797_WIFI_AIS_HIF_TEST_GRANT,
	MT6797_WIFI_AIS_HIF_TEST_FOREIGN_THEN_GRANT,
	MT6797_WIFI_AIS_HIF_TEST_TIMEOUT,
	MT6797_WIFI_AIS_HIF_TEST_GRANT_AT_TIMEOUT,
	MT6797_WIFI_AIS_HIF_TEST_MALFORMED,
	MT6797_WIFI_AIS_HIF_TEST_OVERSIZED,
	MT6797_WIFI_AIS_HIF_TEST_HIF_FAULT,
	MT6797_WIFI_AIS_HIF_TEST_DUPLICATE,
};

struct mt6797_wifi_ais_hif_test_context {
	struct kunit *test;
	struct mt6797_wifi_ais_hif *ais;
	ktime_t now;
	u32 address[MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS];
	size_t length[MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS];
	unsigned long wait_timeout[2];
	unsigned int sends;
	unsigned int waits;
	unsigned int terminals;
	unsigned int fail_send;
	int terminal_reason;
	int offer_error;
	u8 wait_mode;
	bool foreign_signalled;
};

struct mt6797_wifi_ais_hif_test_fixture {
	struct mt6797_wifi_ais_hif ais;
	struct mt6797_wifi_hif_irq irq;
	struct mt6797_wifi_ahb_hif hif;
	struct mt6797_wifi_boot boot;
	struct mt6797_wifi_runtime_config config;
	struct mt6797_wifi_capability capability;
	struct mt6797_wifi_scan_target_owner target_owner;
	struct mt6797_wifi_scan_target target;
	struct mt6797_wifi_ais_hif_backend backend;
	struct mt6797_wifi_ais_hif_request request;
	struct mt6797_wifi_ais_hif_test_context context;
};

static const u8 mt6797_wifi_ais_hif_test_own_addr[ETH_ALEN] = {
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
};

static const u8 mt6797_wifi_ais_hif_test_bssid[ETH_ALEN] = {
	0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
};

static int mt6797_wifi_ais_hif_test_send(void *data, u32 address,
					 const u8 *packet, size_t length)
{
	struct mt6797_wifi_ais_hif_test_context *context = data;
	unsigned int send = context->sends;

	if (!packet || !length || send >= MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS)
		return -EINVAL;
	context->address[send] = address;
	context->length[send] = length;
	context->sends++;
	return send == context->fail_send ? -EIO : 0;
}

static ktime_t mt6797_wifi_ais_hif_test_now(void *data)
{
	struct mt6797_wifi_ais_hif_test_context *context = data;

	return context->now;
}

static int mt6797_wifi_ais_hif_test_offer_length(
	struct mt6797_wifi_ais_hif_test_context *context, bool foreign,
	size_t payload_length)
{
	struct mt6797_wifi_ais_channel_pending *pending =
		&context->ais->runtime.channel_pending;
	u8 payload[MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX + 1] = {};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = payload_length,
		.packet_length = MT6797_WIFI_EVENT_HEADER_LENGTH + payload_length,
		.id = 0x10,
		.sequence = 0xee,
	};

	payload[0] = pending->bss_index;
	payload[1] = pending->token + foreign;
	payload[3] = pending->primary;
	payload[5] = pending->band;
	payload[9] = 0;
	put_unaligned_le32(500, payload + 12);
	return mt6797_wifi_hif_wait_event_offer(
		&context->ais->irq->wait_event, &event);
}

static int mt6797_wifi_ais_hif_test_offer(
	struct mt6797_wifi_ais_hif_test_context *context, bool foreign)
{
	return mt6797_wifi_ais_hif_test_offer_length(context, foreign,
		MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX);
}

static unsigned long mt6797_wifi_ais_hif_test_wait(
	void *data, struct completion *completion, unsigned long timeout)
{
	struct mt6797_wifi_ais_hif_test_context *context = data;
	unsigned int wait = context->waits++;
	struct mt6797_wifi_hif_irq *irq = context->ais->irq;

	if (wait < ARRAY_SIZE(context->wait_timeout))
		context->wait_timeout[wait] = timeout;
	switch (context->wait_mode) {
	case MT6797_WIFI_AIS_HIF_TEST_GRANT:
		context->offer_error = mt6797_wifi_ais_hif_test_offer(
			context, false);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_FOREIGN_THEN_GRANT:
		if (!wait) {
			context->offer_error = mt6797_wifi_ais_hif_test_offer(
				context, true);
			context->foreign_signalled = completion_done(completion);
			context->now = ktime_add_ms(context->now, 400);
			return 0;
		}
		context->offer_error = mt6797_wifi_ais_hif_test_offer(
			context, false);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_TIMEOUT:
		context->now = ktime_add_ms(context->now,
			MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS);
		return 0;
	case MT6797_WIFI_AIS_HIF_TEST_GRANT_AT_TIMEOUT:
		context->offer_error = mt6797_wifi_ais_hif_test_offer(
			context, false);
		context->now = ktime_add_ms(context->now,
			MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS);
		return 0;
	case MT6797_WIFI_AIS_HIF_TEST_MALFORMED:
		/* Matching-ID but too short to classify foreign is ambiguous. */
		context->offer_error = mt6797_wifi_ais_hif_test_offer_length(
			context, false, 1);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_OVERSIZED:
		context->offer_error = mt6797_wifi_ais_hif_test_offer_length(
			context, false,
			MT6797_WIFI_HIF_WAIT_EVENT_PAYLOAD_MAX + 1);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_HIF_FAULT:
		WRITE_ONCE(irq->terminal_reason, -EIO);
		WRITE_ONCE(irq->mode, MT6797_WIFI_HIF_IRQ_TERMINAL);
		mt6797_wifi_hif_wait_event_abort(&irq->wait_event, -EIO);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_DUPLICATE:
		context->offer_error = mt6797_wifi_ais_hif_test_offer(
			context, false);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_offer(
				context, false);
		WRITE_ONCE(irq->terminal_reason, context->offer_error);
		WRITE_ONCE(irq->mode, MT6797_WIFI_HIF_IRQ_TERMINAL);
		break;
	default:
		return 0;
	}
	return try_wait_for_completion(completion) ? 1 : 0;
}

static void mt6797_wifi_ais_hif_test_terminal(void *data, int reason)
{
	struct mt6797_wifi_ais_hif_test_context *context = data;
	struct mt6797_wifi_hif_irq *irq = context->ais->irq;

	context->terminals++;
	context->terminal_reason = reason;
	WRITE_ONCE(irq->terminal_reason, reason);
	WRITE_ONCE(irq->mode, MT6797_WIFI_HIF_IRQ_TERMINAL);
	mt6797_wifi_hif_wait_event_abort(&irq->wait_event, reason);
}

static void mt6797_wifi_ais_hif_test_init(
	struct kunit *test, struct mt6797_wifi_ais_hif_test_fixture *fixture)
{
	struct mt6797_wifi_scan_target_key key = {
		.ssid = "test",
		.ssid_length = 4,
	};
	struct mt6797_wifi_scan_target_owner *owner = &fixture->target_owner;
	struct mt6797_wifi_ais_hif_test_context *context = &fixture->context;
	int ret;

	context->test = test;
	context->ais = &fixture->ais;
	context->now = ms_to_ktime(MT6797_WIFI_AIS_HIF_TEST_NOW_MS);
	context->fail_send = UINT_MAX;
	context->wait_mode = MT6797_WIFI_AIS_HIF_TEST_GRANT;
	mutex_init(&fixture->irq.lifecycle_lock);
	spin_lock_init(&fixture->irq.terminal_lock);
	mt6797_wifi_hif_wait_event_init(&fixture->irq.wait_event);
	fixture->irq.hif = &fixture->hif;
	fixture->irq.boot = &fixture->boot;
	fixture->irq.mode = MT6797_WIFI_HIF_IRQ_RUNTIME;
	fixture->irq.transaction.active = true;
	fixture->boot.hif = &fixture->hif;
	fixture->boot.lease = (void *)fixture;
	fixture->boot.online = true;
	fixture->boot.transaction.ready = true;
	fixture->config.boot = &fixture->boot;
	fixture->config.hif = &fixture->hif;
	fixture->config.transaction.dirty = true;
	fixture->config.transaction.configured = true;
	memcpy(fixture->capability.mac_address,
	       mt6797_wifi_ais_hif_test_own_addr, ETH_ALEN);
	memcpy(key.bssid, mt6797_wifi_ais_hif_test_bssid, ETH_ALEN);
	mt6797_wifi_scan_target_init(owner);
	ret = mt6797_wifi_scan_target_open(owner, &key, 1000);
	KUNIT_ASSERT_EQ(test, ret, 0);
	owner->selected = (struct mt6797_wifi_scan_target) {
		.generation = owner->generation,
		.cookie = 1,
		.observed_at_ms = MT6797_WIFI_AIS_HIF_TEST_NOW_MS,
		.capability = WLAN_CAPABILITY_ESS,
		.ssid_length = key.ssid_length,
		.band = MT6797_WIFI_RX_BAND_2GHZ,
		.channel = 6,
		.rcpi = 100,
	};
	memcpy(owner->selected.bssid, key.bssid, ETH_ALEN);
	memcpy(owner->selected.ssid, key.ssid, key.ssid_length);
	owner->have_selected = true;
	owner->next_cookie = 2;
	fixture->target = owner->selected;
	fixture->backend = (struct mt6797_wifi_ais_hif_backend) {
		.send = mt6797_wifi_ais_hif_test_send,
		.now = mt6797_wifi_ais_hif_test_now,
		.wait = mt6797_wifi_ais_hif_test_wait,
		.terminal = mt6797_wifi_ais_hif_test_terminal,
		.context = context,
	};
	ret = mt6797_wifi_ais_hif_init(&fixture->ais, &fixture->irq,
		&fixture->config, &fixture->capability, 3, 1,
		&fixture->backend);
	KUNIT_ASSERT_EQ(test, ret, 0);
	fixture->request = (struct mt6797_wifi_ais_hif_request) {
		.target_owner = owner,
		.target = &fixture->target,
		.now_ms = MT6797_WIFI_AIS_HIF_TEST_NOW_MS + 1,
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.bss_index = 0,
		.own_mac_index = 0,
	};
}

static struct mt6797_wifi_ais_hif_test_fixture *
mt6797_wifi_ais_hif_test_fixture(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (fixture)
		mt6797_wifi_ais_hif_test_init(test, fixture);
	return fixture;
}

static void mt6797_wifi_ais_hif_start_stop_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), 0);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 3U);
	KUNIT_EXPECT_EQ(test, fixture->ais.runtime.state,
			(enum mt6797_wifi_ais_runtime_state)
			MT6797_WIFI_AIS_RUNTIME_STA_STATE1);
	KUNIT_EXPECT_EQ(test, fixture->ais.target_generation,
			fixture->target.generation);
	KUNIT_EXPECT_EQ(test, fixture->ais.target_cookie,
			fixture->target.cookie);
	KUNIT_EXPECT_FALSE(test, fixture->irq.wait_event.armed);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_stop(&fixture->ais), 0);
	KUNIT_ASSERT_EQ(test, fixture->context.sends, 6U);
	for (i = 0; i < fixture->context.sends; i++)
		KUNIT_EXPECT_EQ(test, fixture->context.address[i],
				MT6797_WIFI_MCR_WTDR1);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);
}

static void mt6797_wifi_ais_hif_ready_target_gate_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->irq.mode = MT6797_WIFI_HIF_IRQ_QUIESCENT;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EPIPE);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 0U);
	fixture->irq.mode = MT6797_WIFI_HIF_IRQ_RUNTIME;
	fixture->target.cookie++;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -ESTALE);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 0U);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);
}

static void mt6797_wifi_ais_hif_foreign_deadline_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode =
		MT6797_WIFI_AIS_HIF_TEST_FOREIGN_THEN_GRANT;
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), 0);
	KUNIT_EXPECT_EQ(test, fixture->context.waits, 2U);
	KUNIT_EXPECT_FALSE(test, fixture->context.foreign_signalled);
	KUNIT_EXPECT_LT(test, fixture->context.wait_timeout[1],
			fixture->context.wait_timeout[0]);
	KUNIT_EXPECT_EQ(test, fixture->context.offer_error, 0);
}

static void mt6797_wifi_ais_hif_timeout_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 3U);
	KUNIT_EXPECT_TRUE(test,
		fixture->ais.runtime.dirty[
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT]);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, fixture->context.terminal_reason, -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, fixture->irq.wait_event.armed);
}

static void mt6797_wifi_ais_hif_completion_at_timeout_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode =
		MT6797_WIFI_AIS_HIF_TEST_GRANT_AT_TIMEOUT;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), 0);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 3U);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);
}

static void mt6797_wifi_ais_hif_malformed_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_HIF_TEST_MALFORMED;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EPROTO);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 2U);
}

static void mt6797_wifi_ais_hif_send_failure_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.fail_send = 1;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EIO);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 2U);
	KUNIT_EXPECT_TRUE(test,
		fixture->ais.runtime.dirty[
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST]);
	KUNIT_EXPECT_FALSE(test,
		fixture->ais.runtime.sent[
			MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST]);
	KUNIT_EXPECT_FALSE(test, fixture->irq.wait_event.armed);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
}

static void mt6797_wifi_ais_hif_oversized_event_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_HIF_TEST_OVERSIZED;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 2U);
	KUNIT_EXPECT_FALSE(test, fixture->irq.wait_event.armed);
}

static void mt6797_wifi_ais_hif_fault_race_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_HIF_TEST_HIF_FAULT;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EIO);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 2U);
	KUNIT_EXPECT_FALSE(test, fixture->irq.wait_event.armed);
	KUNIT_EXPECT_EQ(test, fixture->ais.runtime.state,
			(enum mt6797_wifi_ais_runtime_state)
			MT6797_WIFI_AIS_RUNTIME_TERMINAL);
}

static void mt6797_wifi_ais_hif_duplicate_event_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.wait_mode = MT6797_WIFI_AIS_HIF_TEST_DUPLICATE;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, fixture->context.offer_error, -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 2U);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
}

static void mt6797_wifi_ais_hif_stop_fault_race_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
						       &fixture->request), 0);
	WRITE_ONCE(fixture->irq.terminal_reason, -EIO);
	WRITE_ONCE(fixture->irq.mode, MT6797_WIFI_HIF_IRQ_TERMINAL);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_stop(&fixture->ais),
			-EIO);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 3U);
	KUNIT_EXPECT_EQ(test, fixture->ais.runtime.state,
			(enum mt6797_wifi_ais_runtime_state)
			MT6797_WIFI_AIS_RUNTIME_TERMINAL);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
}

static void mt6797_wifi_hif_event_copy_lifetime_test(struct kunit *test)
{
	struct mt6797_wifi_hif_wait_event wait_event;
	struct mt6797_wifi_hif_event_copy copy;
	u8 match[] = { 1, 2 };
	u8 payload[] = { 1, 2, 3, 4 };
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = sizeof(payload),
		.packet_length = MT6797_WIFI_EVENT_HEADER_LENGTH + sizeof(payload),
		.id = 0x10,
		.sequence = 9,
	};

	mt6797_wifi_hif_wait_event_init(&wait_event);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_hif_wait_event_arm(&wait_event,
						0x10, match, sizeof(match)), 0);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_hif_wait_event_offer(&wait_event,
						  &event), 0);
	memset(payload, 0xff, sizeof(payload));
	KUNIT_ASSERT_EQ(test, mt6797_wifi_hif_wait_event_take(&wait_event,
						 &copy), 0);
	KUNIT_EXPECT_MEMEQ(test, copy.payload, match, sizeof(match));
	KUNIT_EXPECT_EQ(test, copy.payload[2], (u8)3);
	mt6797_wifi_hif_wait_event_disarm(&wait_event);
}

static struct kunit_case mt6797_wifi_ais_hif_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_ais_hif_start_stop_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_ready_target_gate_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_foreign_deadline_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_timeout_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_completion_at_timeout_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_malformed_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_send_failure_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_oversized_event_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_fault_race_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_duplicate_event_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_stop_fault_race_test),
	KUNIT_CASE(mt6797_wifi_hif_event_copy_lifetime_test),
	{}
};

static struct kunit_suite mt6797_wifi_ais_hif_test_suite = {
	.name = "mt6797-wifi-ais-hif",
	.test_cases = mt6797_wifi_ais_hif_test_cases,
};

kunit_test_suite(mt6797_wifi_ais_hif_test_suite);

MODULE_LICENSE("GPL");
