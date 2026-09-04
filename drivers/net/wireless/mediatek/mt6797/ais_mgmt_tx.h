/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_MGMT_TX_H
#define __MT6797_WIFI_AIS_MGMT_TX_H

#include <linux/bits.h>
#include <linux/types.h>

#include "ais_protocol.h"
#include "ais_resources.h"

#define MT6797_WIFI_AIS_MGMT_PID_MIN		1U
#define MT6797_WIFI_AIS_MGMT_PID_MAX		127U
#define MT6797_WIFI_AIS_MGMT_RATE_MAX		GENMASK(11, 0)
#define MT6797_WIFI_AIS_MGMT_TX_EVENT_LENGTH	24U

enum mt6797_wifi_ais_mgmt_pid_state {
	MT6797_WIFI_AIS_MGMT_PID_CLOSED,
	MT6797_WIFI_AIS_MGMT_PID_OPEN,
	MT6797_WIFI_AIS_MGMT_PID_TERMINAL,
};

struct mt6797_wifi_ais_mgmt_pid_lease {
	u64 owner_generation;
	u64 cookie;
	u64 sta_generation;
	u64 bss_cookie;
	u64 sta_cookie;
	u8 wlan_index;
	u8 pid;
};

/*
 * Externally serialized; exactly one management TX may be outstanding.
 * Release is safe only after an exact TX-done match or a proved-not-sent
 * failure. Ambiguous submission, timeout, or malformed owned completion must
 * terminally fence the owner before the lease can be released or reused.
 */
struct mt6797_wifi_ais_mgmt_pid_owner {
	struct mt6797_wifi_ais_mgmt_pid_lease active;
	u64 generation;
	u64 next_cookie;
	u64 sta_generation;
	u64 bss_cookie;
	u64 sta_cookie;
	u8 wlan_index;
	u8 next_pid;
	u8 state;
	bool held;
};

struct mt6797_wifi_ais_mgmt_tx_params {
	u16 sequence_number;
	u16 fixed_rate;
};

struct mt6797_wifi_ais_mgmt_tx_status {
	u16 sequence_number;
	u16 rate;
	u8 status;
	u8 tx_count;
	u8 flags;
	bool advanced;
};

void mt6797_wifi_ais_mgmt_pid_init(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner);
int mt6797_wifi_ais_mgmt_pid_open(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta);
int mt6797_wifi_ais_mgmt_pid_acquire(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	struct mt6797_wifi_ais_mgmt_pid_lease *lease);
int mt6797_wifi_ais_mgmt_pid_release(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease);
int mt6797_wifi_ais_mgmt_pid_close(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner);
int mt6797_wifi_ais_mgmt_pid_mark_terminal(
	struct mt6797_wifi_ais_mgmt_pid_owner *owner);

int mt6797_wifi_ais_mgmt_tx_build(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease,
	const struct mt6797_wifi_ais_bss_handle *bss,
	const struct mt6797_wifi_ais_sta_handle *sta,
	const struct mt6797_wifi_ais_mgmt_tx_params *params,
	const u8 *frame, size_t frame_length, u8 *packet, size_t capacity,
	size_t *transfer_length);
int mt6797_wifi_ais_mgmt_tx_event_match(
	const struct mt6797_wifi_ais_mgmt_pid_owner *owner,
	const struct mt6797_wifi_ais_mgmt_pid_lease *lease,
	const struct mt6797_wifi_event_view *event,
	enum mt6797_wifi_ais_event_disposition *disposition,
	struct mt6797_wifi_ais_mgmt_tx_status *status);

#endif /* __MT6797_WIFI_AIS_MGMT_TX_H */
