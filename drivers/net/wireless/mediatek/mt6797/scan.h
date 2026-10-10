/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_SCAN_H
#define __MT6797_WIFI_SCAN_H

#include <linux/types.h>

#include "protocol.h"

#define MT6797_WIFI_SCAN_MAX_CHANNELS		32U
#define MT6797_WIFI_SCAN_COMMAND_LENGTH		236U
#define MT6797_WIFI_SCAN_MAX_SSIDS		4U
#define MT6797_WIFI_SCAN_MAX_SSID_LENGTH		32U
#define MT6797_WIFI_SCAN_MAX_IE_LENGTH		600U
#define MT6797_WIFI_SCAN_MAX_COMMAND_LENGTH	836U
#define MT6797_WIFI_SCAN_CANCEL_LENGTH		12U
#define MT6797_WIFI_SCAN_TIMEOUT_MS		30000U
#define MT6797_WIFI_SCAN_DONE_EVENT_ID		0x0dU

enum mt6797_wifi_scan_band {
	MT6797_WIFI_SCAN_BAND_2GHZ = 1,
	MT6797_WIFI_SCAN_BAND_5GHZ = 2,
};

struct mt6797_wifi_scan_channel {
	u8 band;
	u8 number;
};

struct mt6797_wifi_scan_ssid {
	u8 length;
	u8 bytes[MT6797_WIFI_SCAN_MAX_SSID_LENGTH];
};

struct mt6797_wifi_scan_probe {
	bool active;
	const struct mt6797_wifi_scan_ssid *ssids;
	size_t ssid_count;
	const u8 *ies;
	size_t ie_length;
};

enum mt6797_wifi_scan_state {
	MT6797_WIFI_SCAN_IDLE,
	MT6797_WIFI_SCAN_WAIT_DONE,
	MT6797_WIFI_SCAN_DONE,
	MT6797_WIFI_SCAN_TERMINAL,
};

enum mt6797_wifi_scan_event_result {
	MT6797_WIFI_SCAN_EVENT_IGNORED,
	MT6797_WIFI_SCAN_EVENT_COMPLETE,
};

struct mt6797_wifi_scan_transaction {
	enum mt6797_wifi_scan_state state;
	u8 scan_sequence;
};

int mt6797_wifi_scan_start(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_scan_channel *channels,
			   size_t channel_count, u8 command_sequence,
			   u8 scan_sequence, u8 *command, size_t capacity);
int mt6797_wifi_scan_probe_validate(const struct mt6797_wifi_scan_probe *probe);
int mt6797_wifi_scan_start_probe(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_scan_channel *channels,
			   size_t channel_count,
			   const struct mt6797_wifi_scan_probe *probe,
			   u8 command_sequence, u8 scan_sequence,
			   u8 *command, size_t capacity, size_t *command_length);
int mt6797_wifi_scan_event(struct mt6797_wifi_scan_transaction *transaction,
			   const struct mt6797_wifi_event_view *event,
			   enum mt6797_wifi_scan_event_result *result);
/* Invoke from the future owner's timer after MT6797_WIFI_SCAN_TIMEOUT_MS. */
int mt6797_wifi_scan_timeout(struct mt6797_wifi_scan_transaction *transaction,
			     u8 command_sequence, u8 *command, size_t capacity);
int mt6797_wifi_scan_reset(struct mt6797_wifi_scan_transaction *transaction);

#endif /* __MT6797_WIFI_SCAN_H */
