// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
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
#define MT6797_WIFI_AIS_HIF_TEST_PACKET_MAX	160U

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

enum mt6797_wifi_ais_hif_test_auth_mode {
	MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_THEN_RX,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_RX_THEN_TX,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_FOREIGN_THEN_OWNED,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_FAILED,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_REJECTED,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_TIMEOUT,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_EVENT,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_FRAME,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_DUPLICATE,
	MT6797_WIFI_AIS_HIF_TEST_AUTH_HIF_ABORT,
};

struct mt6797_wifi_ais_hif_test_context {
	struct kunit *test;
	struct mt6797_wifi_ais_hif *ais;
	ktime_t now;
	u32 address[MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS];
	size_t length[MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS];
	u8 packet[MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS]
		 [MT6797_WIFI_AIS_HIF_TEST_PACKET_MAX];
	unsigned long wait_timeout[2];
	unsigned int sends;
	unsigned int waits;
	unsigned int terminals;
	unsigned int fail_send;
	int terminal_reason;
	int offer_error;
	u8 wait_mode;
	u8 auth_mode;
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

	if (!packet || !length ||
	    length > MT6797_WIFI_AIS_HIF_TEST_PACKET_MAX ||
	    send >= MT6797_WIFI_AIS_HIF_TEST_MAX_SENDS)
		return -EINVAL;
	context->address[send] = address;
	context->length[send] = length;
	memcpy(context->packet[send], packet, length);
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

static int mt6797_wifi_ais_hif_test_auth_event(
	struct mt6797_wifi_ais_hif_test_context *context, bool foreign,
	bool malformed, u8 status)
{
	struct mt6797_wifi_ais_hif_auth *auth = &context->ais->auth;
	u8 payload[16] = {};
	struct mt6797_wifi_event_view event = {
		.payload = payload,
		.payload_length = malformed ? sizeof(payload) - 1 : sizeof(payload),
		.packet_length = malformed ? 23 :
			MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH,
		.id = 0x0f,
	};

	payload[0] = auth->pid_lease.pid + foreign;
	payload[1] = status;
	put_unaligned_le16(7, payload + 2);
	payload[4] = auth->pid_lease.wlan_index;
	payload[5] = 1;
	payload[8] = BIT(0);
	return mt6797_wifi_hif_runtime_sink_event(context->ais->irq, &event);
}

static int mt6797_wifi_ais_hif_test_auth_frame(
	struct mt6797_wifi_ais_hif_test_context *context, bool foreign,
	bool malformed, u16 status)
{
	struct mt6797_wifi_ais_hif_auth *auth = &context->ais->auth;
	u8 frame_data[MT6797_WIFI_AIS_AUTH_FRAME_LENGTH] = {};
	struct mt6797_wifi_rx_view frame = {
		.payload = frame_data,
		.payload_length = sizeof(frame_data),
		.packet_length = MT6797_WIFI_RX_DESCRIPTOR_LENGTH +
			sizeof(frame_data),
		.kind = MT6797_WIFI_RX_IEEE80211,
		.header_length = 24,
		.wlan_index = auth->pid_lease.wlan_index,
	};

	put_unaligned_le16(IEEE80211_STYPE_AUTH, frame_data);
	memcpy(frame_data + 4, auth->pending.own_addr, ETH_ALEN);
	memcpy(frame_data + 10, auth->pending.peer_addr, ETH_ALEN);
	memcpy(frame_data + 16, auth->pending.peer_addr, ETH_ALEN);
	if (foreign)
		frame_data[10] ^= 1;
	put_unaligned_le16(malformed ? 3 : 2, frame_data + 26);
	put_unaligned_le16(status, frame_data + 28);
	return mt6797_wifi_hif_runtime_sink_frame(context->ais->irq, &frame);
}

static unsigned long mt6797_wifi_ais_hif_test_auth_wait(
	struct mt6797_wifi_ais_hif_test_context *context,
	struct completion *completion)
{
	switch (context->auth_mode) {
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_THEN_RX:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
			context, false, false, 0);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
				context, false, false, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_RX_THEN_TX:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
			context, false, false, 0);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
				context, false, false, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_FOREIGN_THEN_OWNED:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
			context, true, false, 0);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
				context, true, false, 0);
		context->foreign_signalled = completion_done(completion);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
				context, false, false, 0);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
				context, false, false, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_FAILED:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
			context, false, false, 7);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_REJECTED:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
			context, false, false, 13);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
				context, false, false, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_TIMEOUT:
		context->now = ktime_add_ms(context->now,
			MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS);
		return 0;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_EVENT:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
			context, false, true, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_FRAME:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_frame(
			context, false, true, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_DUPLICATE:
		context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
			context, false, false, 0);
		if (!context->offer_error)
			context->offer_error = mt6797_wifi_ais_hif_test_auth_event(
				context, false, false, 0);
		break;
	case MT6797_WIFI_AIS_HIF_TEST_AUTH_HIF_ABORT:
		mt6797_wifi_hif_runtime_sink_abort(context->ais->irq, -EIO);
		break;
	default:
		return 0;
	}
	return try_wait_for_completion(completion) ? 1 : 0;
}

static unsigned long mt6797_wifi_ais_hif_test_wait(
	void *data, struct completion *completion, unsigned long timeout)
{
	struct mt6797_wifi_ais_hif_test_context *context = data;
	unsigned int wait = context->waits++;
	struct mt6797_wifi_hif_irq *irq = context->ais->irq;

	if (context->ais->auth.armed)
		return mt6797_wifi_ais_hif_test_auth_wait(context, completion);
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
	spin_lock_init(&fixture->irq.sink_lock);
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

static void mt6797_wifi_ais_hif_auth_success_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_auth_request request = {
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.sequence_number = 0x123,
		.fixed_rate = 0,
	};
	struct mt6797_wifi_ais_hif_auth_result result;
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);
	const u8 *packet;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.auth_mode =
		MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_THEN_RX;
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), 0);
	KUNIT_EXPECT_TRUE(test, result.tx_done);
	KUNIT_EXPECT_TRUE(test, result.response_received);
	KUNIT_EXPECT_EQ(test, result.tx.status, (u8)0);
	KUNIT_EXPECT_EQ(test, result.response.status_code, (u16)0);
	KUNIT_EXPECT_EQ(test, fixture->context.sends, 4U);
	KUNIT_EXPECT_EQ(test, fixture->context.address[3],
		MT6797_WIFI_MCR_WTDR1);
	KUNIT_EXPECT_EQ(test, fixture->context.length[3], (size_t)60);
	packet = fixture->context.packet[3];
	KUNIT_EXPECT_EQ(test, packet[3], (u8)0x88);
	KUNIT_EXPECT_EQ(test, packet[20], (u8)1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 14), (u16)0x8123);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 26), (u16)0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 28),
		(u16)IEEE80211_STYPE_AUTH);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), -EALREADY);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_stop(&fixture->ais), 0);
}

static void mt6797_wifi_ais_hif_auth_reverse_race_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_auth_request request = {
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.sequence_number = 1,
		.fixed_rate = 0x4b,
	};
	struct mt6797_wifi_ais_hif_auth_result result;
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.auth_mode =
		MT6797_WIFI_AIS_HIF_TEST_AUTH_RX_THEN_TX;
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), 0);
	KUNIT_EXPECT_TRUE(test, result.tx_done);
	KUNIT_EXPECT_TRUE(test, result.response_received);
	KUNIT_EXPECT_EQ(test, fixture->context.offer_error, 0);
}

static void mt6797_wifi_ais_hif_auth_foreign_reject_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_auth_request request = {
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.sequence_number = 1,
		.fixed_rate = 0x4b,
	};
	struct mt6797_wifi_ais_hif_auth_result result;
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.auth_mode =
		MT6797_WIFI_AIS_HIF_TEST_AUTH_FOREIGN_THEN_OWNED;
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), 0);
	KUNIT_EXPECT_FALSE(test, fixture->context.foreign_signalled);

	fixture = mt6797_wifi_ais_hif_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.auth_mode = MT6797_WIFI_AIS_HIF_TEST_AUTH_REJECTED;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), -EACCES);
	KUNIT_EXPECT_TRUE(test, result.tx_done);
	KUNIT_EXPECT_TRUE(test, result.response_received);
	KUNIT_EXPECT_EQ(test, result.response.status_code, (u16)13);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);
}

static void mt6797_wifi_ais_hif_auth_tx_failure_test(struct kunit *test)
{
	struct mt6797_wifi_ais_hif_auth_request request = {
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.sequence_number = 1,
		.fixed_rate = 0x4b,
	};
	struct mt6797_wifi_ais_hif_auth_result result;
	struct mt6797_wifi_ais_hif_test_fixture *fixture =
		mt6797_wifi_ais_hif_test_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.auth_mode = MT6797_WIFI_AIS_HIF_TEST_AUTH_TX_FAILED;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), -EREMOTEIO);
	KUNIT_EXPECT_TRUE(test, result.tx_done);
	KUNIT_EXPECT_FALSE(test, result.response_received);
	KUNIT_EXPECT_EQ(test, result.tx.status, (u8)7);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 0U);

	fixture = mt6797_wifi_ais_hif_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
		&fixture->request), 0);
	fixture->context.fail_send = 3;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ais_hif_open_auth(&fixture->ais,
		&request, &result), -EIO);
	KUNIT_EXPECT_TRUE(test, fixture->ais.auth.dirty);
	KUNIT_EXPECT_FALSE(test, fixture->ais.auth.sent);
	KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
}

static void mt6797_wifi_ais_hif_auth_terminal_paths_test(struct kunit *test)
{
	static const struct {
		u8 mode;
		int error;
	} cases[] = {
		{ MT6797_WIFI_AIS_HIF_TEST_AUTH_TIMEOUT, -ETIMEDOUT },
		{ MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_EVENT, -EPROTO },
		{ MT6797_WIFI_AIS_HIF_TEST_AUTH_MALFORMED_FRAME, -EPROTO },
		{ MT6797_WIFI_AIS_HIF_TEST_AUTH_DUPLICATE, -EOVERFLOW },
		{ MT6797_WIFI_AIS_HIF_TEST_AUTH_HIF_ABORT, -EIO },
	};
	struct mt6797_wifi_ais_hif_auth_request request = {
		.response_timeout_ms = MT6797_WIFI_AIS_HIF_TEST_TIMEOUT_MS,
		.sequence_number = 1,
		.fixed_rate = 0x4b,
	};
	struct mt6797_wifi_ais_hif_auth_result result;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct mt6797_wifi_ais_hif_test_fixture *fixture =
			mt6797_wifi_ais_hif_test_fixture(test);

		KUNIT_ASSERT_NOT_NULL(test, fixture);
		KUNIT_ASSERT_EQ(test, mt6797_wifi_ais_hif_start(&fixture->ais,
			&fixture->request), 0);
		fixture->context.auth_mode = cases[i].mode;
		KUNIT_EXPECT_EQ(test,
			mt6797_wifi_ais_hif_open_auth(&fixture->ais, &request,
				&result), cases[i].error);
		KUNIT_EXPECT_EQ(test, fixture->ais.auth.terminal_reason,
			cases[i].error);
		KUNIT_EXPECT_EQ(test, fixture->ais.auth.pid_owner.state,
			(u8)MT6797_WIFI_AIS_MGMT_PID_TERMINAL);
		KUNIT_EXPECT_EQ(test, fixture->context.terminals, 1U);
		KUNIT_EXPECT_PTR_EQ(test, fixture->irq.sink.context, NULL);
	}
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
	KUNIT_CASE(mt6797_wifi_ais_hif_auth_success_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_auth_reverse_race_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_auth_foreign_reject_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_auth_tx_failure_test),
	KUNIT_CASE(mt6797_wifi_ais_hif_auth_terminal_paths_test),
	{}
};

static struct kunit_suite mt6797_wifi_ais_hif_test_suite = {
	.name = "mt6797-wifi-ais-hif",
	.test_cases = mt6797_wifi_ais_hif_test_cases,
};

kunit_test_suite(mt6797_wifi_ais_hif_test_suite);

MODULE_LICENSE("GPL");
