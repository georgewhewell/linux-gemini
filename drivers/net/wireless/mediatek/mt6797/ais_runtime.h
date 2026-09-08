/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_RUNTIME_H
#define __MT6797_WIFI_AIS_RUNTIME_H

#include <linux/types.h>

#include "ais_protocol.h"
#include "ais_resources.h"

enum mt6797_wifi_ais_runtime_state {
	MT6797_WIFI_AIS_RUNTIME_IDLE,
	MT6797_WIFI_AIS_RUNTIME_WAIT_CHANNEL,
	MT6797_WIFI_AIS_RUNTIME_CHANNEL_GRANTED,
	MT6797_WIFI_AIS_RUNTIME_STA_STATE1,
	MT6797_WIFI_AIS_RUNTIME_STA_STATE3,
	MT6797_WIFI_AIS_RUNTIME_STOPPED,
	MT6797_WIFI_AIS_RUNTIME_TERMINAL,
};

enum mt6797_wifi_ais_runtime_command {
	MT6797_WIFI_AIS_RUNTIME_BSS_ACTIVATE,
	MT6797_WIFI_AIS_RUNTIME_CHANNEL_REQUEST,
	MT6797_WIFI_AIS_RUNTIME_STA_STATE1_UPDATE,
	MT6797_WIFI_AIS_RUNTIME_STA_REMOVE,
	MT6797_WIFI_AIS_RUNTIME_CHANNEL_ABORT,
	MT6797_WIFI_AIS_RUNTIME_BSS_DEACTIVATE,
	MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT,
};

struct mt6797_wifi_ais_runtime;

struct mt6797_wifi_ais_runtime_ops {
	int (*precheck)(void *context);
	int (*send)(void *context, const struct mt6797_wifi_ais_runtime *runtime,
		    enum mt6797_wifi_ais_runtime_command command,
		    const u8 *packet, size_t length);
	/* The timeout is owner policy, not part of the firmware wire format. */
	int (*wait_channel)(void *context, unsigned int timeout_ms);
	void (*fence)(void *context, int reason);
	void *context;
};

struct mt6797_wifi_ais_runtime_params {
	struct mt6797_wifi_ais_channel channel;
	u8 own_addr[ETH_ALEN];
	u8 peer_addr[ETH_ALEN];
	u8 bss_index;
	u8 own_mac_index;
	unsigned int response_timeout_ms;
};

struct mt6797_wifi_ais_runtime_packet {
	u8 data[MT6797_WIFI_AIS_STA_UPDATE_LENGTH];
	size_t length;
};

/*
 * This object is caller-serialized. Event delivery may occur synchronously
 * from wait_channel(); a future IRQ adapter must provide its own short lock
 * around event/timeout arbitration and must never retain an event payload.
 */
struct mt6797_wifi_ais_runtime {
	struct mt6797_wifi_ais_resources resources;
	struct mt6797_wifi_ais_bss_handle bss;
	struct mt6797_wifi_ais_sta_handle sta;
	struct mt6797_wifi_ais_channel_pending channel_pending;
	struct mt6797_wifi_ais_runtime_packet
		packet[MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT];
	void *owner_context;
	unsigned int next_command_sequence;
	unsigned int next_channel_token;
	int terminal_reason;
	u8 state;
	bool dirty[MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT];
	bool sent[MT6797_WIFI_AIS_RUNTIME_COMMAND_COUNT];
	bool fenced;
};

void mt6797_wifi_ais_runtime_init(struct mt6797_wifi_ais_runtime *runtime,
				  unsigned int first_command_sequence,
				  unsigned int first_channel_token);
int mt6797_wifi_ais_runtime_start(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops,
	const struct mt6797_wifi_ais_runtime_params *params);
int mt6797_wifi_ais_runtime_event(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition);
int mt6797_wifi_ais_runtime_stop(
	struct mt6797_wifi_ais_runtime *runtime,
	const struct mt6797_wifi_ais_runtime_ops *ops);

#endif /* __MT6797_WIFI_AIS_RUNTIME_H */
