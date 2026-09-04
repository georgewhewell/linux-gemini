/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_OPEN_AUTH_QUALIFICATION_H
#define __MT6797_WIFI_OPEN_AUTH_QUALIFICATION_H

#include <linux/atomic.h>
#include <linux/types.h>

#include "ais_hif.h"
#include "scan_hif.h"

#define MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH	40U
#define MT6797_WIFI_OPEN_AUTH_MANIFEST_DIGEST_LENGTH	64U
#define MT6797_WIFI_OPEN_AUTH_COMMAND_MAX		512U

struct mt6797_wifi_capability;
struct mt6797_wifi_hif_irq;
struct mt6797_wifi_runtime_config;

enum mt6797_wifi_open_auth_qualification_state {
	MT6797_WIFI_OPEN_AUTH_QUALIFICATION_IDLE,
	MT6797_WIFI_OPEN_AUTH_QUALIFICATION_CONSUMED,
	MT6797_WIFI_OPEN_AUTH_QUALIFICATION_COMPLETE,
};

struct mt6797_wifi_open_auth_qualification_params {
	struct mt6797_wifi_scan_target_key key;
	char manifest_sha256[MT6797_WIFI_OPEN_AUTH_MANIFEST_DIGEST_LENGTH + 1];
	unsigned int scan_timeout_ms;
	unsigned int auth_timeout_ms;
	u16 basic_rate;
	u8 band;
	u8 channel;
};

struct mt6797_wifi_open_auth_qualification_result {
	char manifest_sha256[MT6797_WIFI_OPEN_AUTH_MANIFEST_DIGEST_LENGTH + 1];
	int error;
	unsigned int attempts;
	bool tx_done;
	bool response_received;
};

struct mt6797_wifi_open_auth_qualification_ops {
	int (*scan_run)(void *context,
		const struct mt6797_wifi_scan_hif_request *request,
		struct mt6797_wifi_scan_hif_result *result);
	int (*scan_close)(void *context);
	int (*ais_start)(void *context,
		const struct mt6797_wifi_ais_hif_request *request);
	int (*authenticate)(void *context,
		const struct mt6797_wifi_ais_hif_auth_request *request,
		struct mt6797_wifi_ais_hif_auth_result *result);
	int (*ais_stop)(void *context);
	ktime_t (*now)(void *context);
	void *context;
};

struct mt6797_wifi_open_auth_qualification {
	struct mt6797_wifi_scan_hif scan;
	struct mt6797_wifi_ais_hif ais;
	struct mt6797_wifi_open_auth_qualification_ops ops;
	struct mt6797_wifi_open_auth_qualification_result result;
	atomic_t state;
	char source_revision[MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION_LENGTH + 1];
	bool initialized;
};

int mt6797_wifi_open_auth_qualification_parse(
	const char *buffer, size_t count,
	struct mt6797_wifi_open_auth_qualification_params *params);
bool mt6797_wifi_open_auth_source_revision_valid(const char *source_revision);
int mt6797_wifi_open_auth_qualification_init(
	struct mt6797_wifi_open_auth_qualification *qualification,
	struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability,
	const char *source_revision,
	const struct mt6797_wifi_open_auth_qualification_ops *ops);
ssize_t mt6797_wifi_open_auth_qualification_trigger(
	struct mt6797_wifi_open_auth_qualification *qualification,
	const char *buffer, size_t count);
ssize_t mt6797_wifi_open_auth_qualification_result_show(
	struct mt6797_wifi_open_auth_qualification *qualification, char *buffer);

#endif /* __MT6797_WIFI_OPEN_AUTH_QUALIFICATION_H */
