// SPDX-License-Identifier: GPL-2.0-only
/* Dormant one-shot open-auth qualification caller for recovery candidates. */

#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "ais_protocol.h"
#include "open_auth_qualification.h"

#define MT6797_WIFI_OPEN_AUTH_LIBRARY_REVISION \
	"2597a1100741e9f87debd4e80038371c7c7ecd17"
#define MT6797_WIFI_OPEN_AUTH_SCHEMA		"mt6797-open-auth-ap-v1"
#define MT6797_WIFI_OPEN_AUTH_DIGEST_INVALID	"invalid"
#define MT6797_WIFI_OPEN_AUTH_SCAN_COMMAND_SEQUENCE	3U
#define MT6797_WIFI_OPEN_AUTH_SCAN_CANCEL_SEQUENCE	4U
#define MT6797_WIFI_OPEN_AUTH_SCAN_SEQUENCE		1U
#define MT6797_WIFI_OPEN_AUTH_AIS_COMMAND_SEQUENCE	5U
#define MT6797_WIFI_OPEN_AUTH_AIS_CHANNEL_TOKEN		1U
#define MT6797_WIFI_OPEN_AUTH_FRAME_SEQUENCE		1U
#define MT6797_WIFI_OPEN_AUTH_BSS_INDEX			0U
#define MT6797_WIFI_OPEN_AUTH_OWN_MAC_INDEX		0U

struct mt6797_wifi_open_auth_field {
	const char *value;
	size_t length;
};

static bool mt6797_wifi_open_auth_lower_hex(const char *value, size_t length)
{
	size_t index;

	if (!value || !length)
		return false;
	for (index = 0; index < length; index++)
		if (!isdigit(value[index]) &&
		    (value[index] < 'a' || value[index] > 'f'))
			return false;
	return true;
}

static int mt6797_wifi_open_auth_field(
	const char *buffer, size_t count, size_t *offset, const char *name,
	struct mt6797_wifi_open_auth_field *field)
{
	const char *position;
	size_t name_length;
	size_t remaining;

	if (!buffer || !offset || !name || !field || *offset >= count)
		return -EINVAL;
	name_length = strlen(name);
	remaining = count - *offset;
	if (remaining <= name_length ||
	    memcmp(buffer + *offset, name, name_length))
		return -EINVAL;
	position = memchr(buffer + *offset + name_length, '\n',
		remaining - name_length);
	if (!position)
		return -EINVAL;
	field->value = buffer + *offset + name_length;
	field->length = position - field->value;
	if (!field->length)
		return -EINVAL;
	*offset = position - buffer + 1;
	return 0;
}

static bool mt6797_wifi_open_auth_field_equal(
	const struct mt6797_wifi_open_auth_field *field, const char *expected)
{
	size_t length = strlen(expected);

	return field->length == length && !memcmp(field->value, expected, length);
}

static int mt6797_wifi_open_auth_decimal(
	const struct mt6797_wifi_open_auth_field *field, unsigned int maximum,
	unsigned int *result)
{
	unsigned int value = 0;
	size_t index;

	if (!field || !result || !field->length ||
	    (field->length > 1 && field->value[0] == '0'))
		return -EINVAL;
	for (index = 0; index < field->length; index++) {
		if (!isdigit(field->value[index]) ||
		    value > (maximum - (field->value[index] - '0')) / 10)
			return -EINVAL;
		value = value * 10 + field->value[index] - '0';
	}
	if (!value || value > maximum)
		return -EINVAL;
	*result = value;
	return 0;
}

static int mt6797_wifi_open_auth_basic_rate(
	const struct mt6797_wifi_open_auth_field *field, u16 *rate)
{
	unsigned int value = 0;
	size_t index;

	if (!field || !rate || field->length < 3 || field->length > 5 ||
	    field->value[0] != '0' || field->value[1] != 'x' ||
	    (field->length > 3 && field->value[2] == '0') ||
	    !mt6797_wifi_open_auth_lower_hex(field->value + 2,
		field->length - 2))
		return -EINVAL;
	for (index = 2; index < field->length; index++) {
		value <<= 4;
		value |= hex_to_bin(field->value[index]);
	}
	if (!value || value > MT6797_WIFI_AIS_MGMT_RATE_MAX)
		return -EINVAL;
	*rate = value;
	return 0;
}

static int mt6797_wifi_open_auth_bssid(
	const struct mt6797_wifi_open_auth_field *field, u8 bssid[ETH_ALEN])
{
	size_t index;

	if (!field || field->length != 17)
		return -EINVAL;
	for (index = 0; index < field->length; index++) {
		if (index % 3 == 2) {
			if (field->value[index] != ':')
				return -EINVAL;
		} else if (!isdigit(field->value[index]) &&
			   (field->value[index] < 'a' ||
			    field->value[index] > 'f')) {
			return -EINVAL;
		}
	}
	if (!mac_pton(field->value, bssid) || !is_valid_ether_addr(bssid))
		return -EINVAL;
	return 0;
}

int mt6797_wifi_open_auth_qualification_parse(
	const char *buffer, size_t count,
	struct mt6797_wifi_open_auth_qualification_params *params)
{
	struct mt6797_wifi_open_auth_qualification_params parsed = {};
	struct mt6797_wifi_open_auth_field field;
	unsigned int value;
	size_t offset = 0;
	int ret;

	if (!buffer || !params || !count ||
	    count > MT6797_WIFI_OPEN_AUTH_COMMAND_MAX || memchr(buffer, '\0', count))
		return -EINVAL;

	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "schema=",
		&field);
	if (ret)
		return ret;
	if (!mt6797_wifi_open_auth_field_equal(&field,
		MT6797_WIFI_OPEN_AUTH_SCHEMA))
		return -EINVAL;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset,
		"ap_manifest_sha256=", &field);
	if (ret)
		return ret;
	if (field.length != MT6797_WIFI_OPEN_AUTH_MANIFEST_DIGEST_LENGTH ||
	    !mt6797_wifi_open_auth_lower_hex(field.value, field.length) ||
	    !memchr_inv(field.value, '0', field.length))
		return -EINVAL;
	memcpy(parsed.manifest_sha256, field.value, field.length);
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset,
		"authentication=", &field);
	if (ret)
		return ret;
	if (!mt6797_wifi_open_auth_field_equal(&field, "open"))
		return -EOPNOTSUPP;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset,
		"credential=", &field);
	if (ret)
		return ret;
	if (!mt6797_wifi_open_auth_field_equal(&field, "none"))
		return -EOPNOTSUPP;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "ssid_hex=",
		&field);
	if (ret)
		return ret;
	if (field.length < 2 || field.length > IEEE80211_MAX_SSID_LEN * 2 ||
	    field.length % 2 ||
	    !mt6797_wifi_open_auth_lower_hex(field.value, field.length) ||
	    hex2bin(parsed.key.ssid, field.value, field.length / 2))
		return -EINVAL;
	parsed.key.ssid_length = field.length / 2;
	if (!memchr_inv(parsed.key.ssid, 0, parsed.key.ssid_length))
		return -EINVAL;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "bssid=",
		&field);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_bssid(&field, parsed.key.bssid);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "band=",
		&field);
	if (ret)
		return ret;
	if (mt6797_wifi_open_auth_field_equal(&field, "2ghz"))
		parsed.band = MT6797_WIFI_SCAN_BAND_2GHZ;
	else if (mt6797_wifi_open_auth_field_equal(&field, "5ghz"))
		parsed.band = MT6797_WIFI_SCAN_BAND_5GHZ;
	else
		return -EINVAL;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "channel=",
		&field);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_decimal(&field, U8_MAX, &value);
	if (ret || (parsed.band == MT6797_WIFI_SCAN_BAND_2GHZ && value > 14) ||
	    (parsed.band == MT6797_WIFI_SCAN_BAND_5GHZ &&
	     (value <= 14 || value > 199)))
		return -EINVAL;
	parsed.channel = value;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset, "basic_rate=",
		&field);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_basic_rate(&field, &parsed.basic_rate);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset,
		"scan_timeout_ms=", &field);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_decimal(&field,
		MT6797_WIFI_SCAN_TIMEOUT_MS, &parsed.scan_timeout_ms);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_field(buffer, count, &offset,
		"auth_timeout_ms=", &field);
	if (ret)
		return ret;
	ret = mt6797_wifi_open_auth_decimal(&field,
		MT6797_WIFI_AIS_JOIN_MAX_INTERVAL_MS, &parsed.auth_timeout_ms);
	if (ret || offset != count)
		return -EINVAL;
	*params = parsed;
	return 0;
}

static int mt6797_wifi_open_auth_real_scan_run(
	void *context, const struct mt6797_wifi_scan_hif_request *request,
	struct mt6797_wifi_scan_hif_result *result)
{
	struct mt6797_wifi_open_auth_qualification *qualification = context;

	return mt6797_wifi_scan_hif_run(&qualification->scan, request, result);
}

static int mt6797_wifi_open_auth_real_scan_close(void *context)
{
	struct mt6797_wifi_open_auth_qualification *qualification = context;

	return mt6797_wifi_scan_hif_close(&qualification->scan);
}

static int mt6797_wifi_open_auth_real_ais_start(
	void *context, const struct mt6797_wifi_ais_hif_request *request)
{
	struct mt6797_wifi_open_auth_qualification *qualification = context;

	return mt6797_wifi_ais_hif_start(&qualification->ais, request);
}

static int mt6797_wifi_open_auth_real_authenticate(
	void *context, const struct mt6797_wifi_ais_hif_auth_request *request,
	struct mt6797_wifi_ais_hif_auth_result *result)
{
	struct mt6797_wifi_open_auth_qualification *qualification = context;

	return mt6797_wifi_ais_hif_open_auth(&qualification->ais, request, result);
}

static int mt6797_wifi_open_auth_real_ais_stop(void *context)
{
	struct mt6797_wifi_open_auth_qualification *qualification = context;

	return mt6797_wifi_ais_hif_stop(&qualification->ais);
}

static ktime_t mt6797_wifi_open_auth_real_now(void *context)
{
	(void)context;
	return ktime_get();
}

static bool mt6797_wifi_open_auth_ops_valid(
	const struct mt6797_wifi_open_auth_qualification_ops *ops)
{
	return ops && ops->scan_run && ops->scan_close && ops->ais_start &&
		ops->authenticate && ops->ais_stop && ops->now && ops->context;
}

bool mt6797_wifi_open_auth_source_revision_valid(const char *source_revision)
{
	return source_revision &&
		strlen(source_revision) ==
			MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH &&
		mt6797_wifi_open_auth_lower_hex(source_revision,
			MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH) &&
		memchr_inv(source_revision, '0',
			MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH);
}

int mt6797_wifi_open_auth_qualification_init(
	struct mt6797_wifi_open_auth_qualification *qualification,
	struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability,
	const char *source_revision,
	const struct mt6797_wifi_open_auth_qualification_ops *ops)
{
	int ret;

	if (!qualification ||
	    !mt6797_wifi_open_auth_source_revision_valid(source_revision) ||
	    (ops && !mt6797_wifi_open_auth_ops_valid(ops)) ||
	    (!ops && (!irq || !config || !capability)))
		return -EINVAL;
	memset(qualification, 0, sizeof(*qualification));
	memcpy(qualification->source_revision, source_revision,
	       MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH);
	if (ops) {
		qualification->ops = *ops;
	} else {
		ret = mt6797_wifi_scan_hif_init(&qualification->scan, irq, NULL);
		if (ret)
			return ret;
		ret = mt6797_wifi_ais_hif_init(&qualification->ais, irq, config,
			capability, MT6797_WIFI_OPEN_AUTH_AIS_COMMAND_SEQUENCE,
			MT6797_WIFI_OPEN_AUTH_AIS_CHANNEL_TOKEN, NULL);
		if (ret)
			return ret;
		qualification->ops =
			(struct mt6797_wifi_open_auth_qualification_ops) {
				.scan_run = mt6797_wifi_open_auth_real_scan_run,
				.scan_close = mt6797_wifi_open_auth_real_scan_close,
				.ais_start = mt6797_wifi_open_auth_real_ais_start,
				.authenticate = mt6797_wifi_open_auth_real_authenticate,
				.ais_stop = mt6797_wifi_open_auth_real_ais_stop,
				.now = mt6797_wifi_open_auth_real_now,
				.context = qualification,
			};
	}
	atomic_set(&qualification->state,
		MT6797_WIFI_OPEN_AUTH_QUALIFICATION_IDLE);
	qualification->initialized = true;
	return 0;
}

static int mt6797_wifi_open_auth_qualification_execute(
	struct mt6797_wifi_open_auth_qualification *qualification,
	const struct mt6797_wifi_open_auth_qualification_params *params,
	struct mt6797_wifi_ais_hif_auth_result *auth_result,
	bool *auth_attempted)
{
	struct mt6797_wifi_scan_hif_request scan_request = {
		.key = params->key,
		.max_age_ms = (u64)params->scan_timeout_ms +
			params->auth_timeout_ms,
		.timeout_ms = params->scan_timeout_ms,
		.command_sequence = MT6797_WIFI_OPEN_AUTH_SCAN_COMMAND_SEQUENCE,
		.cancel_sequence = MT6797_WIFI_OPEN_AUTH_SCAN_CANCEL_SEQUENCE,
		.scan_sequence = MT6797_WIFI_OPEN_AUTH_SCAN_SEQUENCE,
		.band = params->band,
		.channel = params->channel,
	};
	struct mt6797_wifi_ais_hif_auth_request auth_request = {
		.response_timeout_ms = params->auth_timeout_ms,
		.sequence_number = MT6797_WIFI_OPEN_AUTH_FRAME_SEQUENCE,
		.fixed_rate = params->basic_rate,
	};
	struct mt6797_wifi_scan_hif_result scan_result;
	struct mt6797_wifi_ais_hif_request ais_request;
	s64 now_ms;
	int cleanup;
	int ret;

	ret = qualification->ops.scan_run(qualification->ops.context,
		&scan_request, &scan_result);
	if (ret)
		return ret;
	now_ms = ktime_to_ms(qualification->ops.now(qualification->ops.context));
	if (now_ms < 0) {
		ret = -ERANGE;
		goto close_scan;
	}
	ais_request = (struct mt6797_wifi_ais_hif_request) {
		.target_owner = &qualification->scan.target_owner,
		.target = &scan_result.target,
		.now_ms = now_ms,
		.response_timeout_ms = params->auth_timeout_ms,
		.bss_index = MT6797_WIFI_OPEN_AUTH_BSS_INDEX,
		.own_mac_index = MT6797_WIFI_OPEN_AUTH_OWN_MAC_INDEX,
	};
	ret = qualification->ops.ais_start(qualification->ops.context,
		&ais_request);
	if (ret)
		goto close_scan;

	*auth_attempted = true;
	ret = qualification->ops.authenticate(qualification->ops.context,
		&auth_request, auth_result);
	if (!ret && (!auth_result->tx_done || !auth_result->response_received ||
		     auth_result->tx.status || auth_result->response.status_code))
		ret = -EPROTO;
	cleanup = qualification->ops.ais_stop(qualification->ops.context);
	if (cleanup && (!ret || ret == -EACCES || ret == -EREMOTEIO))
		ret = cleanup;
close_scan:
	cleanup = qualification->ops.scan_close(qualification->ops.context);
	if (cleanup && (!ret || ret == -EACCES || ret == -EREMOTEIO))
		ret = cleanup;
	return ret;
}

ssize_t mt6797_wifi_open_auth_qualification_trigger(
	struct mt6797_wifi_open_auth_qualification *qualification,
	const char *buffer, size_t count)
{
	struct mt6797_wifi_open_auth_qualification_params params;
	struct mt6797_wifi_ais_hif_auth_result auth_result = {};
	bool auth_attempted = false;
	int ret;

	if (!qualification || !qualification->initialized || !buffer)
		return -EINVAL;
	if (atomic_cmpxchg(&qualification->state,
		MT6797_WIFI_OPEN_AUTH_QUALIFICATION_IDLE,
		MT6797_WIFI_OPEN_AUTH_QUALIFICATION_CONSUMED) !=
	    MT6797_WIFI_OPEN_AUTH_QUALIFICATION_IDLE)
		return -EALREADY;

	ret = mt6797_wifi_open_auth_qualification_parse(buffer, count, &params);
	memset(&qualification->result, 0, sizeof(qualification->result));
	if (!ret) {
		memcpy(qualification->result.manifest_sha256,
		       params.manifest_sha256,
		       sizeof(qualification->result.manifest_sha256));
		ret = mt6797_wifi_open_auth_qualification_execute(qualification,
			&params, &auth_result, &auth_attempted);
	} else {
		strscpy(qualification->result.manifest_sha256,
			MT6797_WIFI_OPEN_AUTH_DIGEST_INVALID,
			sizeof(qualification->result.manifest_sha256));
	}
	qualification->result.error = ret;
	qualification->result.attempts = auth_attempted ? 1 : 0;
	qualification->result.tx_done = auth_result.tx_done;
	qualification->result.response_received = auth_result.response_received;
	atomic_set_release(&qualification->state,
		MT6797_WIFI_OPEN_AUTH_QUALIFICATION_COMPLETE);
	return ret ? ret : count;
}

ssize_t mt6797_wifi_open_auth_qualification_result_show(
	struct mt6797_wifi_open_auth_qualification *qualification, char *buffer)
{
	const struct mt6797_wifi_open_auth_qualification_result *result;

	if (!qualification || !qualification->initialized || !buffer)
		return -EINVAL;
	if (atomic_read_acquire(&qualification->state) !=
	    MT6797_WIFI_OPEN_AUTH_QUALIFICATION_COMPLETE)
		return -ENODATA;
	result = &qualification->result;
	return scnprintf(buffer, PAGE_SIZE,
		"schema=mt6797-open-auth-transcript-v1\n"
		"ap_manifest_sha256=%s\n"
		"source_revision=%s\n"
		"library_revision=%s\n"
		"attempts=%u\n"
		"operation=open-auth\n"
		"retry=none\n"
		"association=not-attempted\n"
		"key_install=not-attempted\n"
		"data_path=not-attempted\n"
		"result=%s errno=%d tx_done=%s response_received=%s\n",
		result->manifest_sha256, qualification->source_revision,
		MT6797_WIFI_OPEN_AUTH_LIBRARY_REVISION,
		result->attempts,
		result->error ? "failure" : "success", result->error,
		result->tx_done ? "yes" : "no",
		result->response_received ? "yes" : "no");
}
