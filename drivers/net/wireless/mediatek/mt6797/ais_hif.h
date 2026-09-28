/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_HIF_H
#define __MT6797_WIFI_AIS_HIF_H

#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "ais_auth.h"
#include "ais_assoc.h"
#include "ais_join.h"
#include "ais_mgmt_tx.h"
#include "ais_runtime.h"

struct mt6797_wifi_capability;
struct mt6797_wifi_hif_irq;
struct mt6797_wifi_runtime_config;
struct mt6797_wifi_scan_target;
struct mt6797_wifi_scan_target_owner;

/* Driver-private backend boundary; a NULL backend selects the real AHB HIF. */
struct mt6797_wifi_ais_hif_backend {
	int (*send)(void *context, u32 address, const u8 *packet,
		    size_t length);
	ktime_t (*now)(void *context);
	unsigned long (*wait)(void *context, struct completion *completion,
			      unsigned long timeout);
	void (*terminal)(void *context, int reason);
	void *context;
};

struct mt6797_wifi_ais_hif_request {
	struct mt6797_wifi_scan_target_owner *target_owner;
	const struct mt6797_wifi_scan_target *target;
	u64 now_ms;
	unsigned int response_timeout_ms;
	u8 bss_index;
	u8 own_mac_index;
};

struct mt6797_wifi_ais_hif_auth_request {
	unsigned int response_timeout_ms;
	u16 sequence_number;
	u16 fixed_rate;
};

struct mt6797_wifi_ais_hif_auth_result {
	struct mt6797_wifi_ais_mgmt_tx_status tx;
	struct mt6797_wifi_ais_auth_response response;
	bool tx_done;
	bool response_received;
};

struct mt6797_wifi_ais_hif_auth {
	/* Serializes synchronous IRQ delivery against the sleeping owner. */
	spinlock_t lock;
	struct completion completion;
	struct mt6797_wifi_ais_mgmt_pid_owner pid_owner;
	struct mt6797_wifi_ais_mgmt_pid_lease pid_lease;
	struct mt6797_wifi_ais_auth_pending pending;
	struct mt6797_wifi_ais_mgmt_tx_status tx;
	struct mt6797_wifi_ais_auth_response response;
	int terminal_reason;
	/* Reset is allowed only before dirty; otherwise a new HIF lifetime is required. */
	bool attempted;
	bool armed;
	bool dirty;
	bool sent;
	bool tx_done;
	bool response_received;
};

#define MT6797_WIFI_AIS_EXT_AUTH_RX_SLOTS	4U

struct mt6797_wifi_ais_hif_ext_auth_request {
	const u8 *frame;
	size_t length;
	unsigned int timeout_ms;
	u16 sequence_number;
	u16 fixed_rate;
};

/*
 * SAE is computed by user space (NL80211_CMD_EXTERNAL_AUTH). This bridge only
 * transmits its authentication frames to the AIS peer and queues the peer's
 * SAE frames. The auth lock protects every field against IRQ delivery.
 */
struct mt6797_wifi_ais_hif_ext_auth {
	u8 rx[MT6797_WIFI_AIS_EXT_AUTH_RX_SLOTS][MT6797_WIFI_AIS_SAE_FRAME_MAX];
	u16 rx_length[MT6797_WIFI_AIS_EXT_AUTH_RX_SLOTS];
	u8 packet[MT6797_WIFI_AIS_SAE_FRAME_MAX + MT6797_WIFI_TX_DESCRIPTOR_LENGTH + 4];
	u8 rx_head;
	u8 rx_count;
	u32 rx_frames;
	u32 rx_dropped;
	u32 tx_frames;
	u32 tx_acked;
	bool active;
	bool tx_outstanding;
};

struct mt6797_wifi_ais_hif_join_request {
	struct mt6797_wifi_ais_assoc_params association;
	struct mt6797_wifi_ais_join_params negotiated;
	unsigned int timeout_ms;
	u16 sequence_number;
};

struct mt6797_wifi_ais_hif_join {
	spinlock_t lock;
	struct completion completion;
	struct mt6797_wifi_ais_assoc_pending association;
	struct mt6797_wifi_ais_sta_pending station;
	struct mt6797_wifi_ais_mgmt_pid_lease lease;
	u8 response[MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH];
	size_t response_length;
	u16 status_code;
	u8 tx_status;
	int error;
	bool attempted;
	bool armed;
	bool station_phase;
	bool tx_done;
	bool activated;
};

/*
 * This object is explicitly invoked and never started from probe. The caller
 * owns its lifetime and the scan-target owner for the entire transaction.
 */
struct mt6797_wifi_ais_hif {
	struct mt6797_wifi_hif_irq *irq;
	struct mt6797_wifi_runtime_config *config;
	const struct mt6797_wifi_capability *capability;
	struct mt6797_wifi_ais_hif_backend backend;
	struct mt6797_wifi_ais_runtime runtime;
	struct mt6797_wifi_ais_hif_auth auth;
	struct mt6797_wifi_ais_hif_ext_auth ext_auth;
	struct mt6797_wifi_ais_hif_join join;
	ktime_t channel_requested_at;
	ktime_t channel_deadline;
	u64 target_generation;
	u64 target_cookie;
	bool initialized;
};

int mt6797_wifi_ais_hif_init(
	struct mt6797_wifi_ais_hif *ais, struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability,
	unsigned int first_command_sequence,
	unsigned int first_channel_token,
	const struct mt6797_wifi_ais_hif_backend *backend);
int mt6797_wifi_ais_hif_start(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_request *request);
int mt6797_wifi_ais_hif_stop(struct mt6797_wifi_ais_hif *ais);
int mt6797_wifi_ais_hif_release_channel(struct mt6797_wifi_ais_hif *ais);
/*
 * Explicit, blocking, one-shot operation after start() reached STA_STATE1.
 * It is never invoked by probe; the caller supplies both timeout policy and
 * an evidenced basic rate after recovery/rollback supervision is armed.
 */
int mt6797_wifi_ais_hif_open_auth(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_auth_request *request,
	struct mt6797_wifi_ais_hif_auth_result *result);
/*
 * External (SAE) authentication after start() reached STA_STATE1, instead of
 * open auth. begin() attaches the AIS sink; send() blocks for one TX-done;
 * take() pops a queued peer SAE frame; wait() sleeps until RX, TX or kick().
 * end() detaches, closes management TX ownership and, only for a successful
 * user-space status after completed TX/RX, arms association.
 */
int mt6797_wifi_ais_hif_ext_auth_begin(struct mt6797_wifi_ais_hif *ais);
int mt6797_wifi_ais_hif_ext_auth_send(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_ext_auth_request *request,
	struct mt6797_wifi_ais_mgmt_tx_status *status);
int mt6797_wifi_ais_hif_ext_auth_take(struct mt6797_wifi_ais_hif *ais,
	u8 *frame, size_t capacity, size_t *length);
void mt6797_wifi_ais_hif_ext_auth_wait(struct mt6797_wifi_ais_hif *ais,
	ktime_t deadline);
void mt6797_wifi_ais_hif_ext_auth_kick(struct mt6797_wifi_ais_hif *ais);
int mt6797_wifi_ais_hif_ext_auth_end(struct mt6797_wifi_ais_hif *ais,
	u16 status_code);
/* Blocking association+firmware activation after successful authentication.
 * The owned response remains in ais->join until the AIS lifetime ends.
 * A successful result still needs key installation and the data path.
 */
int mt6797_wifi_ais_hif_associate(
	struct mt6797_wifi_ais_hif *ais,
	const struct mt6797_wifi_ais_hif_join_request *request,
	struct mt6797_wifi_ais_join_params *negotiated);

#endif /* __MT6797_WIFI_AIS_HIF_H */
