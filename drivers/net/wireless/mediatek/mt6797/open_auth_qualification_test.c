// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/string.h>

#include "open_auth_qualification.h"

#define MT6797_WIFI_OPEN_AUTH_TEST_SOURCE \
	"1111111111111111111111111111111111111111"
#define MT6797_WIFI_OPEN_AUTH_TEST_DIGEST \
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define MT6797_WIFI_OPEN_AUTH_TEST_COMMAND \
	"schema=mt6797-open-auth-ap-v1\n" \
	"ap_manifest_sha256=" MT6797_WIFI_OPEN_AUTH_TEST_DIGEST "\n" \
	"authentication=open\n" \
	"credential=none\n" \
	"ssid_hex=74657374\n" \
	"bssid=02:11:22:33:44:55\n" \
	"band=2ghz\n" \
	"channel=6\n" \
	"basic_rate=0x4b\n" \
	"scan_timeout_ms=1000\n" \
	"auth_timeout_ms=1000\n"

enum mt6797_wifi_open_auth_test_event {
	MT6797_WIFI_OPEN_AUTH_TEST_SCAN,
	MT6797_WIFI_OPEN_AUTH_TEST_START,
	MT6797_WIFI_OPEN_AUTH_TEST_AUTH,
	MT6797_WIFI_OPEN_AUTH_TEST_STOP,
	MT6797_WIFI_OPEN_AUTH_TEST_CLOSE,
};

struct mt6797_wifi_open_auth_test_context {
	struct kunit *test;
	struct mt6797_wifi_open_auth_qualification *qualification;
	struct mt6797_wifi_scan_hif_request scan_request;
	struct mt6797_wifi_ais_hif_auth_request auth_request;
	enum mt6797_wifi_open_auth_test_event events[5];
	unsigned int event_count;
	int scan_error;
	int start_error;
	int auth_error;
	int stop_error;
	int close_error;
	bool inconsistent_auth;
};

struct mt6797_wifi_open_auth_test_fixture {
	struct mt6797_wifi_open_auth_qualification qualification;
	struct mt6797_wifi_open_auth_test_context context;
	char result[PAGE_SIZE];
};

static void mt6797_wifi_open_auth_test_record(
	struct mt6797_wifi_open_auth_test_context *context,
	enum mt6797_wifi_open_auth_test_event event)
{
	if (context->event_count < ARRAY_SIZE(context->events))
		context->events[context->event_count++] = event;
}

static int mt6797_wifi_open_auth_test_scan(
	void *data, const struct mt6797_wifi_scan_hif_request *request,
	struct mt6797_wifi_scan_hif_result *result)
{
	struct mt6797_wifi_open_auth_test_context *context = data;

	mt6797_wifi_open_auth_test_record(context,
		MT6797_WIFI_OPEN_AUTH_TEST_SCAN);
	context->scan_request = *request;
	if (context->scan_error)
		return context->scan_error;
	memset(result, 0, sizeof(*result));
	result->target.generation = 1;
	result->target.cookie = 1;
	result->target.band = request->band;
	result->target.channel = request->channel;
	memcpy(result->target.bssid, request->key.bssid, ETH_ALEN);
	return 0;
}

static int mt6797_wifi_open_auth_test_close(void *data)
{
	struct mt6797_wifi_open_auth_test_context *context = data;

	mt6797_wifi_open_auth_test_record(context,
		MT6797_WIFI_OPEN_AUTH_TEST_CLOSE);
	return context->close_error;
}

static int mt6797_wifi_open_auth_test_start(
	void *data, const struct mt6797_wifi_ais_hif_request *request)
{
	struct mt6797_wifi_open_auth_test_context *context = data;

	mt6797_wifi_open_auth_test_record(context,
		MT6797_WIFI_OPEN_AUTH_TEST_START);
	KUNIT_EXPECT_PTR_EQ(context->test, request->target_owner,
		&context->qualification->scan.target_owner);
	KUNIT_EXPECT_EQ(context->test, request->target->channel, (u8)6);
	KUNIT_EXPECT_EQ(context->test, request->response_timeout_ms, 1000U);
	return context->start_error;
}

static int mt6797_wifi_open_auth_test_authenticate(
	void *data, const struct mt6797_wifi_ais_hif_auth_request *request,
	struct mt6797_wifi_ais_hif_auth_result *result)
{
	struct mt6797_wifi_open_auth_test_context *context = data;

	mt6797_wifi_open_auth_test_record(context,
		MT6797_WIFI_OPEN_AUTH_TEST_AUTH);
	context->auth_request = *request;
	memset(result, 0, sizeof(*result));
	if (!context->inconsistent_auth) {
		result->tx_done = true;
		result->response_received = true;
	}
	if (context->auth_error == -EACCES)
		result->response.status_code = 13;
	return context->auth_error;
}

static int mt6797_wifi_open_auth_test_stop(void *data)
{
	struct mt6797_wifi_open_auth_test_context *context = data;

	mt6797_wifi_open_auth_test_record(context,
		MT6797_WIFI_OPEN_AUTH_TEST_STOP);
	return context->stop_error;
}

static ktime_t mt6797_wifi_open_auth_test_now(void *data)
{
	(void)data;
	return ms_to_ktime(100);
}

static void mt6797_wifi_open_auth_test_init(
	struct kunit *test,
	struct mt6797_wifi_open_auth_qualification *qualification,
	struct mt6797_wifi_open_auth_test_context *context)
{
	struct mt6797_wifi_open_auth_qualification_ops ops;

	memset(qualification, 0, sizeof(*qualification));
	memset(context, 0, sizeof(*context));
	context->test = test;
	context->qualification = qualification;
	ops = (struct mt6797_wifi_open_auth_qualification_ops) {
		.scan_run = mt6797_wifi_open_auth_test_scan,
		.scan_close = mt6797_wifi_open_auth_test_close,
		.ais_start = mt6797_wifi_open_auth_test_start,
		.authenticate = mt6797_wifi_open_auth_test_authenticate,
		.ais_stop = mt6797_wifi_open_auth_test_stop,
		.now = mt6797_wifi_open_auth_test_now,
		.context = context,
	};
	KUNIT_ASSERT_EQ(test, mt6797_wifi_open_auth_qualification_init(
		qualification, NULL, NULL, NULL,
		MT6797_WIFI_OPEN_AUTH_TEST_SOURCE, &ops), 0);
}

static struct mt6797_wifi_open_auth_test_fixture *
mt6797_wifi_open_auth_test_fixture(struct kunit *test)
{
	struct mt6797_wifi_open_auth_test_fixture *fixture;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return NULL;
	mt6797_wifi_open_auth_test_init(test, &fixture->qualification,
		&fixture->context);
	return fixture;
}

static void mt6797_wifi_open_auth_parse_test(struct kunit *test)
{
	struct mt6797_wifi_open_auth_qualification_params params;

	KUNIT_ASSERT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND), &params), 0);
	KUNIT_EXPECT_STREQ(test, params.manifest_sha256,
		MT6797_WIFI_OPEN_AUTH_TEST_DIGEST);
	KUNIT_EXPECT_MEMEQ(test, params.key.ssid, "test", 4);
	KUNIT_EXPECT_EQ(test, params.key.ssid_length, (size_t)4);
	KUNIT_EXPECT_EQ(test, params.key.bssid[0], (u8)0x02);
	KUNIT_EXPECT_EQ(test, params.key.bssid[5], (u8)0x55);
	KUNIT_EXPECT_EQ(test, params.band, (u8)MT6797_WIFI_SCAN_BAND_2GHZ);
	KUNIT_EXPECT_EQ(test, params.channel, (u8)6);
	KUNIT_EXPECT_EQ(test, params.basic_rate, (u16)0x4b);
	KUNIT_EXPECT_EQ(test, params.scan_timeout_ms, 1000U);
	KUNIT_EXPECT_EQ(test, params.auth_timeout_ms, 1000U);
}

static void mt6797_wifi_open_auth_parse_reject_test(struct kunit *test)
{
	struct mt6797_wifi_open_auth_qualification_params params;
	char command[sizeof(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND) + 32];
	char *position;
	size_t length = strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND);

	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, length - 1, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, MT6797_WIFI_OPEN_AUTH_TEST_DIGEST);
	KUNIT_ASSERT_NOT_NULL(test, position);
	position[0] = 'A';
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, MT6797_WIFI_OPEN_AUTH_TEST_DIGEST);
	KUNIT_ASSERT_NOT_NULL(test, position);
	memset(position, '0', MT6797_WIFI_OPEN_AUTH_MANIFEST_DIGEST_LENGTH);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, "02:11:22:33:44:55");
	KUNIT_ASSERT_NOT_NULL(test, position);
	position[3] = 'A';
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, "02:11:22:33:44:55");
	KUNIT_ASSERT_NOT_NULL(test, position);
	position[1] = '3';
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, "credential=none");
	KUNIT_ASSERT_NOT_NULL(test, position);
	position[sizeof("credential=") - 1] = 'p';
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EOPNOTSUPP);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, "ssid_hex=74657374");
	KUNIT_ASSERT_NOT_NULL(test, position);
	memset(position + sizeof("ssid_hex=") - 1, '0', 8);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	position = strstr(command, "channel=6");
	KUNIT_ASSERT_NOT_NULL(test, position);
	position[sizeof("channel=") - 1] = '0';
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, length, &params), -EINVAL);

	strscpy(command, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND, sizeof(command));
	strscpy(command + length, "psk=forbidden\n", sizeof(command) - length);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_parse(
		command, strlen(command), &params), -EINVAL);
}

static void mt6797_wifi_open_auth_success_result_test(struct kunit *test)
{
	static const enum mt6797_wifi_open_auth_test_event expected[] = {
		MT6797_WIFI_OPEN_AUTH_TEST_SCAN,
		MT6797_WIFI_OPEN_AUTH_TEST_START,
		MT6797_WIFI_OPEN_AUTH_TEST_AUTH,
		MT6797_WIFI_OPEN_AUTH_TEST_STOP,
		MT6797_WIFI_OPEN_AUTH_TEST_CLOSE,
	};
	struct mt6797_wifi_open_auth_test_fixture *fixture;
	ssize_t length;

	fixture = mt6797_wifi_open_auth_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_result_show(
		&fixture->qualification, fixture->result), (ssize_t)-ENODATA);
	length = strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND);
	KUNIT_ASSERT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length), length);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count,
		ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, fixture->context.events, expected,
		sizeof(expected));
	KUNIT_EXPECT_EQ(test, fixture->context.scan_request.command_sequence,
		(u8)3);
	KUNIT_EXPECT_EQ(test, fixture->context.scan_request.cancel_sequence,
		(u8)4);
	KUNIT_EXPECT_EQ(test, fixture->context.scan_request.scan_sequence,
		(u8)1);
	KUNIT_EXPECT_EQ(test, fixture->context.scan_request.max_age_ms, 2000ULL);
	KUNIT_EXPECT_EQ(test, fixture->context.auth_request.sequence_number,
		(u16)1);
	KUNIT_EXPECT_EQ(test, fixture->context.auth_request.fixed_rate,
		(u16)0x4b);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length),
		(ssize_t)-EALREADY);
	length = mt6797_wifi_open_auth_qualification_result_show(
		&fixture->qualification, fixture->result);
	KUNIT_ASSERT_GT(test, length, (ssize_t)0);
	KUNIT_EXPECT_STREQ(test, fixture->result,
		"schema=mt6797-open-auth-transcript-v1\n"
		"ap_manifest_sha256=" MT6797_WIFI_OPEN_AUTH_TEST_DIGEST "\n"
		"source_revision=" MT6797_WIFI_OPEN_AUTH_TEST_SOURCE "\n"
		"library_revision=2597a1100741e9f87debd4e80038371c7c7ecd17\n"
		"attempts=1\n"
		"operation=open-auth\n"
		"retry=none\n"
		"association=not-attempted\n"
		"key_install=not-attempted\n"
		"data_path=not-attempted\n"
		"result=success errno=0 tx_done=yes response_received=yes\n");
}

static void mt6797_wifi_open_auth_parse_consumes_test(struct kunit *test)
{
	struct mt6797_wifi_open_auth_test_fixture *fixture;
	ssize_t length;

	fixture = mt6797_wifi_open_auth_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, "bad\n", 4), (ssize_t)-EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND)), (ssize_t)-EALREADY);
	length = mt6797_wifi_open_auth_qualification_result_show(
		&fixture->qualification, fixture->result);
	KUNIT_ASSERT_GT(test, length, (ssize_t)0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result,
		"ap_manifest_sha256=invalid\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result, "attempts=0\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result,
		"result=failure errno=-22 tx_done=no response_received=no\n"));
}

static void mt6797_wifi_open_auth_cleanup_test(struct kunit *test)
{
	struct mt6797_wifi_open_auth_test_fixture *fixture;
	ssize_t length = strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND);

	fixture = mt6797_wifi_open_auth_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.auth_error = -EACCES;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length),
		(ssize_t)-EACCES);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count, 5U);
	KUNIT_EXPECT_EQ(test, fixture->context.events[3],
		(enum mt6797_wifi_open_auth_test_event)
		MT6797_WIFI_OPEN_AUTH_TEST_STOP);
	KUNIT_EXPECT_EQ(test, fixture->context.events[4],
		(enum mt6797_wifi_open_auth_test_event)
		MT6797_WIFI_OPEN_AUTH_TEST_CLOSE);
	KUNIT_ASSERT_GT(test,
		mt6797_wifi_open_auth_qualification_result_show(
			&fixture->qualification, fixture->result), (ssize_t)0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result,
		"result=failure errno=-13 tx_done=yes response_received=yes\n"));

	mt6797_wifi_open_auth_test_init(test, &fixture->qualification,
		&fixture->context);
	fixture->context.start_error = -EIO;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length),
		(ssize_t)-EIO);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count, 3U);
	KUNIT_EXPECT_EQ(test, fixture->context.events[2],
		(enum mt6797_wifi_open_auth_test_event)
		MT6797_WIFI_OPEN_AUTH_TEST_CLOSE);
	KUNIT_ASSERT_GT(test,
		mt6797_wifi_open_auth_qualification_result_show(
			&fixture->qualification, fixture->result), (ssize_t)0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result, "attempts=0\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(fixture->result,
		"result=failure errno=-5 tx_done=no response_received=no\n"));
}

static void mt6797_wifi_open_auth_fail_closed_test(struct kunit *test)
{
	struct mt6797_wifi_open_auth_test_fixture *fixture;
	ssize_t length = strlen(MT6797_WIFI_OPEN_AUTH_TEST_COMMAND);

	fixture = mt6797_wifi_open_auth_test_fixture(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	fixture->context.inconsistent_auth = true;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length),
		(ssize_t)-EPROTO);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count, 5U);

	mt6797_wifi_open_auth_test_init(test, &fixture->qualification,
		&fixture->context);
	fixture->context.stop_error = -EUCLEAN;
	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_trigger(
		&fixture->qualification, MT6797_WIFI_OPEN_AUTH_TEST_COMMAND,
		length),
		(ssize_t)-EUCLEAN);
	KUNIT_EXPECT_EQ(test, fixture->context.event_count, 5U);

	KUNIT_EXPECT_EQ(test, mt6797_wifi_open_auth_qualification_init(
		&fixture->qualification, NULL, NULL, NULL,
		"not-a-source-revision", NULL),
		-EINVAL);
	KUNIT_EXPECT_FALSE(test, mt6797_wifi_open_auth_source_revision_valid(
		"0000000000000000000000000000000000000000"));
}

static struct kunit_case mt6797_wifi_open_auth_qualification_cases[] = {
	KUNIT_CASE(mt6797_wifi_open_auth_parse_test),
	KUNIT_CASE(mt6797_wifi_open_auth_parse_reject_test),
	KUNIT_CASE(mt6797_wifi_open_auth_success_result_test),
	KUNIT_CASE(mt6797_wifi_open_auth_parse_consumes_test),
	KUNIT_CASE(mt6797_wifi_open_auth_cleanup_test),
	KUNIT_CASE(mt6797_wifi_open_auth_fail_closed_test),
	{}
};

static struct kunit_suite mt6797_wifi_open_auth_qualification_suite = {
	.name = "mt6797-wifi-open-auth-qualification",
	.test_cases = mt6797_wifi_open_auth_qualification_cases,
};

kunit_test_suite(mt6797_wifi_open_auth_qualification_suite);

MODULE_LICENSE("GPL");
