// SPDX-License-Identifier: GPL-2.0-only
/* cfg80211 station interface and asynchronous, firmware-backed passive scan. */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/if_vlan.h>
#include <linux/random.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>
#include "ais_hif.h"
#include "ais_key.h"
#include "ais_rsn.h"
#include "capability.h"
#include "data_rx.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "radio.h"
#include "mgmt_rx.h"
#include "rx_metadata.h"
#include "scan.h"
#include "scan_hif.h"
#include "tx_diag.h"

struct mt6797_radio_key {
	u8 material[MT6797_WIFI_AIS_CCMP_KEY_LENGTH];
	u8 receive_sequence[6];
	u64 rx_pn[16];
	u64 mgmt_rx_pn; /* Robust management has a distinct CCMP replay space. */
	u8 index;
	bool installed;
	bool updating;
};

struct mt6797_radio_connection {
	struct cfg80211_bss *bss;
	struct mt6797_wifi_scan_hif scan;
	struct mt6797_wifi_ais_hif ais;
	struct mt6797_wifi_ais_hif_join_request join;
	struct mt6797_radio_key pairwise_key;
	struct mt6797_radio_key group_key;
	/* Host overlap for MMIE key IDs 4/5. Firmware slot coexistence/delete
	 * isolation is still unverified: these fields do not enable PMF.
	 */
	struct mt6797_radio_key igtk[2];
	/* Exact association history: never evict a retired key and allow its IPN
	 * to reset. Exhaustion requires a new association, not a cache flush.
	 */
	u8 igtk_history[32][MT6797_WIFI_AIS_CCMP_KEY_LENGTH];
	u8 igtk_history_count;
	bool igtk_updating; /* Both IDs share firmware WTBL ownership. */
	struct mt6797_wifi_ais_sta_pending igtk_owner;
	struct mt6797_wifi_ais_wtbl_handle igtk_wtbl;
	u8 igtk_default; /* 0 means none, otherwise on-wire ID 4 or 5. */
	/* Both RSN elements set MFPC (ais_rsn.c); fixed for the association. */
	bool pmf_negotiated;
	/* SAE bridge mailbox for NL80211_CMD_EXTERNAL_AUTH; radio->lock guards it. */
	u8 sae_frame[MT6797_WIFI_AIS_SAE_FRAME_MAX];
	u8 sae_scratch[MT6797_WIFI_AIS_SAE_FRAME_MAX]; /* Worker-owned copy. */
	size_t sae_frame_length;
	u64 sae_cookie;
	u8 sae_pmkid[MT6797_WIFI_AIS_RSN_PMKID_LENGTH];
	u16 sae_status;
	u16 auth_sequence; /* Last 802.11 sequence number used before association. */
	bool sae;
	bool sae_active;
	bool sae_tx_pending;
	bool sae_tx_ack;
	bool sae_done;
	bool sae_pmkid_set;
	struct mt6797_wifi_ais_sta_pending key_pending;
	struct mt6797_wifi_ais_wtbl_handle pairwise_wtbl;
	struct completion key_completion;
	struct completion pm_completion;
	struct completion management_completion;
	struct mt6797_wifi_ais_mgmt_pid_lease management_lease;
	u16 management_sequence;
	u8 sa_query_id[2];
	bool sa_query_response_pending;
	bool management_waiting;
	bool management_done;
	int management_error;
	/* 1000 TU maximum / 201 TU retry permits at most five attempts. */
	u8 sa_query_ids[5][2];
	u8 sa_query_count;
	u8 sa_query_attempts;
	bool sa_query_active;
	u64 sa_query_generation;
	ktime_t sa_query_deadline;
	ktime_t sa_query_next;
	ktime_t sa_query_next_start;
	struct mt6797_wifi_ais_sta_pending sa_query_owner;
	struct mt6797_wifi_ais_wtbl_handle sa_query_wtbl;
	struct mt6797_wifi_ais_pm_params pm;
	bool beacon_seen;
	bool pm_started;
	int key_error;
	bool key_waiting;
	bool pairwise_ready;
	bool pairwise_attempted;
	bool authorized;
	/* Authorization requested before the group keys; see change_station. */
	bool authorize_pending;
	bool control_port_no_encrypt;
	bool pairwise_reply_plaintext;
	u8 ies[MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH];
	bool cancel;
	bool connected;
	bool link_attached;
	bool peer_disconnect;
	u16 reason;
};

struct mt6797_wifi_radio {
	struct wireless_dev wdev;
	struct net_device *netdev;
	struct mt6797_wifi_hif_irq *irq;
	struct mt6797_wifi_runtime_config *config;
	struct mt6797_wifi_capability capability;
	struct mt6797_radio_connection *connection;
	struct mutex connection_lock; /* Worker and key callbacks own connection lifetime. */
	struct work_struct connection_work;
	struct work_struct tx_work;
	struct work_struct management_work;
	struct delayed_work sa_query_work;
	struct sk_buff_head tx_queue;
	struct mt6797_radio_tx_diag tx_diag;
	struct work_struct scan_work;
	spinlock_t lock;
	struct completion scan_done;
	struct cfg80211_scan_request *request;
	struct mt6797_wifi_scan_transaction scan;
	struct ieee80211_supported_band bands[2];
	struct ieee80211_channel channels[2][32];
	struct ieee80211_rate rates[12];
	unsigned int command_sequence;
	u8 scan_sequence;
	u8 channel_token;
	u8 management_pid;
	u64 mgmt_cookie;
	int scan_error;
	bool armed;
	bool abort_scan;
	/* The scan runs while associated; its events arrive via the link sink. */
	bool scan_on_link;
	bool stopping;
};

#define MT6797_RADIO_TX_QUEUE_LIMIT 16U
#define MT6797_RADIO_TX_WAKE_THRESHOLD 8U

struct mt6797_radio_tx_cb {
	u64 enqueued_ns;
};

static void mt6797_radio_tx_diag_add(struct mt6797_radio_tx_diag_span *span, u64 ns)
{
	static const u64 bounds_ns[] = {
		100000, 500000, 1000000, 2000000, 5000000,
		10000000, 20000000, 50000000, 100000000,
	};
	unsigned int bucket = 0;

	while (bucket < ARRAY_SIZE(bounds_ns) && ns > bounds_ns[bucket])
		bucket++;
	span->count++;
	span->total_ns += ns;
	if (ns > span->max_ns)
		span->max_ns = ns;
	span->buckets[bucket]++;
}

static int mt6797_radio_tx_diag_span_emit(char *buf, int offset, const char *name,
					const struct mt6797_radio_tx_diag_span *span)
{
	unsigned int bucket;

	offset += sysfs_emit_at(buf, offset, "%s_count=%llu\n%s_total_ns=%llu\n%s_max_ns=%llu\n%s_hist=",
		name, (unsigned long long)span->count,
		name, (unsigned long long)span->total_ns,
		name, (unsigned long long)span->max_ns, name);
	for (bucket = 0; bucket < MT6797_RADIO_TX_DIAG_BUCKETS; bucket++)
		offset += sysfs_emit_at(buf, offset, "%s%llu", bucket ? "," : "",
			(unsigned long long)span->buckets[bucket]);
	return offset + sysfs_emit_at(buf, offset, "\n");
}

static ssize_t tx_diagnostics_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct net_device *netdev = to_net_dev(dev);
	struct mt6797_wifi_radio *radio = wiphy_priv(netdev->ieee80211_ptr->wiphy);
	struct mt6797_radio_tx_diag snapshot;
	unsigned long flags;
	bool connected, stopping;
	u64 now;
	int offset = 0;

	spin_lock_irqsave(&radio->lock, flags);
	snapshot = radio->tx_diag;
	now = ktime_get_ns();
	connected = radio->connection && radio->connection->connected && !radio->connection->cancel;
	stopping = radio->stopping;
	spin_unlock_irqrestore(&radio->lock, flags);

#define EMIT_VALUE(name, value) \
	(offset += sysfs_emit_at(buf, offset, name "=%llu\n", (unsigned long long)(value)))
#define EMIT_FIELD(field) EMIT_VALUE(#field, snapshot.field)
	EMIT_VALUE("version", 1);
	EMIT_FIELD(lifetime_start_ns);
	EMIT_VALUE("snapshot_ns", now);
	EMIT_FIELD(connection_generation);
	EMIT_VALUE("connected", connected);
	EMIT_VALUE("stopping", stopping);
	EMIT_VALUE("queue_limit", MT6797_RADIO_TX_QUEUE_LIMIT);
	EMIT_VALUE("queue_wake_below", MT6797_RADIO_TX_WAKE_THRESHOLD);
	EMIT_FIELD(queue_depth);
	EMIT_FIELD(queue_high_water);
	EMIT_FIELD(enqueued);
	EMIT_FIELD(dequeued);
	EMIT_FIELD(purged);
	EMIT_FIELD(rejected);
	EMIT_FIELD(busy);
	EMIT_FIELD(hif_submitted);
	EMIT_FIELD(send_failed);
	EMIT_FIELD(send_cancelled);
	EMIT_FIELD(send_disconnected);
	EMIT_FIELD(send_timed_out);
	EMIT_FIELD(credit_wait_attempts);
	EMIT_FIELD(credit_wait_packets);
	EMIT_FIELD(credit_wait_wakeups);
	EMIT_FIELD(credit_wait_timeouts);
	EMIT_FIELD(worker_active);
	EMIT_FIELD(send_active);
	EMIT_FIELD(credit_wait_active);
	EMIT_VALUE("worker_active_ns", snapshot.worker_active ? now - snapshot.worker_started_ns : 0);
	EMIT_VALUE("send_active_ns", snapshot.send_active ? now - snapshot.send_started_ns : 0);
	EMIT_VALUE("credit_wait_active_ns", snapshot.credit_wait_active ? now - snapshot.credit_wait_started_ns : 0);
#undef EMIT_FIELD
#undef EMIT_VALUE
	offset += sysfs_emit_at(buf, offset,
		"hist_upper_ns=100000,500000,1000000,2000000,5000000,10000000,20000000,50000000,100000000,inf\n");
	offset = mt6797_radio_tx_diag_span_emit(buf, offset, "queue_wait", &snapshot.queue_wait);
	offset = mt6797_radio_tx_diag_span_emit(buf, offset, "send_duration", &snapshot.send_duration);
	offset = mt6797_radio_tx_diag_span_emit(buf, offset, "credit_wait", &snapshot.credit_wait);
	return mt6797_radio_tx_diag_span_emit(buf, offset, "worker_duration", &snapshot.worker_duration);
}
static DEVICE_ATTR_RO(tx_diagnostics);

static struct attribute *mt6797_radio_tx_diag_attrs[] = {
	&dev_attr_tx_diagnostics.attr,
	NULL,
};

static const struct attribute_group mt6797_radio_tx_diag_group = {
	.attrs = mt6797_radio_tx_diag_attrs,
};

static void mt6797_radio_purge_tx(struct mt6797_wifi_radio *radio)
{
	struct sk_buff *skb;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&radio->lock, flags);
		skb = skb_dequeue(&radio->tx_queue);
		if (skb) {
			radio->tx_diag.purged++;
			radio->tx_diag.queue_depth--;
		}
		spin_unlock_irqrestore(&radio->lock, flags);
		if (!skb)
			break;
		DEV_STATS_INC(radio->netdev, tx_dropped);
		dev_kfree_skb_any(skb);
	}
}

/* radio->lock held. */
static int mt6797_radio_scan_event(struct mt6797_wifi_radio *radio,
				   const struct mt6797_wifi_event_view *event)
{
	enum mt6797_wifi_scan_event_result disposition;
	int ret = 0;

	if (radio->armed) {
		ret = mt6797_wifi_scan_event(&radio->scan, event, &disposition);
		if (ret || disposition == MT6797_WIFI_SCAN_EVENT_COMPLETE) {
			radio->scan_error = ret;
			complete(&radio->scan_done);
		}
	}
	return ret;
}

static int mt6797_radio_event(void *context,
			    const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_radio *radio = context;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&radio->lock, flags);
	ret = mt6797_radio_scan_event(radio, event);
	spin_unlock_irqrestore(&radio->lock, flags);
	return ret;
}

static int mt6797_radio_frame(void *context,
			    const struct mt6797_wifi_rx_view *frame)
{
	struct mt6797_wifi_radio *radio = context;
	struct wiphy *wiphy = radio->wdev.wiphy;
	struct mt6797_wifi_rx_metadata metadata;
	struct mt6797_wifi_bss_view view;
	struct cfg80211_inform_bss data = {};
	struct cfg80211_bss *bss;
	enum nl80211_band band;
	int frequency;

	if (!READ_ONCE(radio->armed) ||
	    frame->kind != MT6797_WIFI_RX_IEEE80211 || !frame->packet)
		return 0;
	if (mt6797_wifi_rx_metadata_decode(frame->packet,
					  frame->packet_length, &metadata) ||
	    mt6797_wifi_bss_parse(&metadata, &view))
		return 0;
	band = metadata.band == MT6797_WIFI_RX_BAND_2GHZ ?
		NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
	frequency = ieee80211_channel_to_frequency(metadata.channel, band);
	data.chan = ieee80211_get_channel(wiphy, frequency);
	if (!data.chan || data.chan->flags & IEEE80211_CHAN_DISABLED)
		return 0;
	data.signal = metadata.signal_mbm;
	data.boottime_ns = ktime_get_boottime_ns();
	/* The IRQ sink owns the borrowed frame only until this callback returns. */
	bss = cfg80211_inform_bss_frame_data(wiphy, &data,
		(struct ieee80211_mgmt *)metadata.frame, metadata.frame_length,
		GFP_ATOMIC);
	if (bss)
		cfg80211_put_bss(wiphy, bss);
	return 0;
}

static void mt6797_radio_abort(void *context, int reason)
{
	struct mt6797_wifi_radio *radio = context;
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	radio->scan_error = reason;
	radio->abort_scan = true;
	complete(&radio->scan_done);
	spin_unlock_irqrestore(&radio->lock, flags);
}

static void mt6797_radio_scan_work(struct work_struct *work)
{
	struct mt6797_wifi_radio *radio =
		container_of(work, struct mt6797_wifi_radio, scan_work);
	struct mt6797_wifi_hif_runtime_sink sink = {
		.event = mt6797_radio_event, .frame = mt6797_radio_frame,
		.abort = mt6797_radio_abort, .context = radio,
	};
	struct mt6797_wifi_scan_channel channels[MT6797_WIFI_SCAN_MAX_CHANNELS];
	struct cfg80211_scan_info info = {};
	struct cfg80211_scan_request *request = radio->request;
	u8 command[MT6797_WIFI_SCAN_COMMAND_LENGTH];
	unsigned long flags;
	unsigned int offset = 0, count, index;
	bool attached = false, sent, needs_fence, on_link;
	int ret = 0;

	spin_lock_irqsave(&radio->lock, flags);
	on_link = radio->scan_on_link;
	spin_unlock_irqrestore(&radio->lock, flags);
	/*
	 * Unassociated, the scan owns the HIF sink for its whole lifetime.
	 * Associated, the link sink forwards scan events and frames, and the
	 * lifecycle lock is taken only per command so key, PM and management
	 * traffic are not held off for the length of the scan.
	 */
	if (!on_link) {
		mutex_lock(&radio->irq->lifecycle_lock);
		ret = mt6797_wifi_hif_runtime_sink_attach(radio->irq, &sink);
		if (ret)
			goto out;
		attached = true;
	}
	while (offset < request->n_channels) {
		count = min_t(unsigned int, request->n_channels - offset,
			      ARRAY_SIZE(channels));
		for (index = 0; index < count; index++) {
			struct ieee80211_channel *channel = request->channels[offset + index];

			channels[index].band = channel->band == NL80211_BAND_2GHZ ?
				MT6797_WIFI_SCAN_BAND_2GHZ : MT6797_WIFI_SCAN_BAND_5GHZ;
			channels[index].number = channel->hw_value;
		}
		spin_lock_irqsave(&radio->lock, flags);
		if (radio->abort_scan || radio->stopping ||
		    (on_link && (!radio->connection || radio->connection->cancel))) {
			ret = -ECANCELED;
			spin_unlock_irqrestore(&radio->lock, flags);
			break;
		}
		reinit_completion(&radio->scan_done);
		radio->scan = (struct mt6797_wifi_scan_transaction) {};
		radio->scan_error = 0;
		ret = mt6797_wifi_scan_start(&radio->scan, channels, count,
			++radio->command_sequence, ++radio->scan_sequence,
			command, sizeof(command));
		radio->armed = !ret;
		spin_unlock_irqrestore(&radio->lock, flags);
		if (ret)
			break;
		if (on_link)
			mutex_lock(&radio->irq->lifecycle_lock);
		ret = mt6797_wifi_hif_runtime_send(radio->irq,
			MT6797_WIFI_MCR_WTDR1, command, sizeof(command));
		if (on_link)
			mutex_unlock(&radio->irq->lifecycle_lock);
		sent = !ret;
		if (!ret && !wait_for_completion_timeout(&radio->scan_done,
			msecs_to_jiffies(MT6797_WIFI_SCAN_TIMEOUT_MS)))
			ret = -ETIMEDOUT;
		spin_lock_irqsave(&radio->lock, flags);
		if (!ret)
			ret = radio->scan_error ?: (radio->abort_scan ? -ECANCELED : 0);
		radio->armed = false;
		needs_fence = ret && radio->scan.state != MT6797_WIFI_SCAN_DONE;
		if (ret && sent && radio->scan.state == MT6797_WIFI_SCAN_WAIT_DONE &&
		    mt6797_wifi_hif_irq_mode(radio->irq) == MT6797_WIFI_HIF_IRQ_RUNTIME) {
			int cancel_ret = mt6797_wifi_scan_timeout(&radio->scan,
				++radio->command_sequence, command, sizeof(command));

			spin_unlock_irqrestore(&radio->lock, flags);
			if (on_link)
				mutex_lock(&radio->irq->lifecycle_lock);
			if (!cancel_ret)
				mt6797_wifi_hif_runtime_send(radio->irq,
					MT6797_WIFI_MCR_WTDR1, command,
					MT6797_WIFI_SCAN_CANCEL_LENGTH);
			if (on_link)
				mutex_unlock(&radio->irq->lifecycle_lock);
		} else {
			spin_unlock_irqrestore(&radio->lock, flags);
		}
		if (ret) {
			/* No cancellation acknowledgement means firmware state is uncertain. */
			if (needs_fence) {
				if (on_link)
					mutex_lock(&radio->irq->lifecycle_lock);
				mt6797_wifi_hif_runtime_fail(radio->irq, ret);
				if (on_link)
					mutex_unlock(&radio->irq->lifecycle_lock);
			}
			break;
		}
		offset += count;
	}
out:
	if (attached)
		mt6797_wifi_hif_runtime_sink_detach(radio->irq, radio);
	if (!on_link)
		mutex_unlock(&radio->irq->lifecycle_lock);
	spin_lock_irqsave(&radio->lock, flags);
	info.aborted = ret || radio->abort_scan || radio->stopping;
	radio->request = NULL;
	radio->scan_on_link = false;
	spin_unlock_irqrestore(&radio->lock, flags);
	cfg80211_scan_done(request, &info);
}

static int mt6797_radio_scan(struct wiphy *wiphy,
			    struct cfg80211_scan_request *request)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	unsigned long flags;
	unsigned int index;
	int ret = 0;

	/*
	 * The firmware scan is passive, so no probe request is sent: a lone
	 * wildcard SSID (wpa_supplicant always asks for one) finds the same
	 * beaconing BSSs, and probe request IEs have nothing to go into, while
	 * a named SSID for a hidden network cannot be served. iw adds
	 * COLOCATED_6GHZ by default; there is no 6 GHz band.
	 */
	if (request->n_ssids > 1 ||
	    (request->n_ssids && request->ssids[0].ssid_len) ||
	    (request->flags & ~NL80211_SCAN_FLAG_COLOCATED_6GHZ))
		return -EOPNOTSUPP;
	if (!request->n_channels)
		return -EINVAL;
	for (index = 0; index < request->n_channels; index++)
		if (request->channels[index]->band > NL80211_BAND_5GHZ ||
		    request->channels[index]->flags & IEEE80211_CHAN_DISABLED)
			return -EINVAL;
	spin_lock_irqsave(&radio->lock, flags);
	if (radio->stopping)
		ret = -ESHUTDOWN;
	/* While a join is in progress it owns the HIF; once associated, share. */
	else if (radio->request || (radio->connection &&
		 (!radio->connection->connected || radio->connection->cancel)))
		ret = -EBUSY;
	else {
		radio->request = request;
		radio->abort_scan = false;
		radio->scan_on_link = radio->connection;
		queue_work(system_long_wq, &radio->scan_work);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return ret;
}

static void mt6797_radio_abort_scan(struct wiphy *wiphy,
				    struct wireless_dev *wdev)
{
	mt6797_radio_abort(wiphy_priv(wiphy), -ECANCELED);
}

static bool mt6797_radio_connection_cancelled(struct mt6797_wifi_radio *radio)
{
	unsigned long flags;
	bool cancelled;

	spin_lock_irqsave(&radio->lock, flags);
	cancelled = radio->stopping || !radio->connection || radio->connection->cancel;
	spin_unlock_irqrestore(&radio->lock, flags);
	return cancelled;
}

/* radio->lock held. Nonblocking cancellation is safe in RX and while holding
 * connection_lock: a running timer gets the connection only after taking that
 * mutex. Stop/unregister synchronously cancel outside the connection mutex.
 * Keep generation/cooldown so stale work cannot restart or complete a query.
 */
static void mt6797_radio_sa_query_stop(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;

	if (connection) {
		connection->sa_query_active = false;
		connection->sa_query_count = 0;
		connection->sa_query_attempts = 0;
		connection->sa_query_deadline = 0;
		connection->sa_query_next = 0;
		memzero_explicit(connection->sa_query_ids, sizeof(connection->sa_query_ids));
		memzero_explicit(&connection->sa_query_owner, sizeof(connection->sa_query_owner));
		memzero_explicit(&connection->sa_query_wtbl, sizeof(connection->sa_query_wtbl));
	}
	cancel_delayed_work(&radio->sa_query_work);
}

static void mt6797_radio_link_abort(void *context, int reason)
{
	struct mt6797_wifi_radio *radio = context;
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	if (radio->scan_on_link) {
		radio->scan_error = reason;
		radio->abort_scan = true;
		complete(&radio->scan_done);
	}
	if (radio->connection) {
		radio->connection->cancel = true;
		mt6797_radio_sa_query_stop(radio);
		complete(&radio->connection->pm_completion);
		if (radio->connection->management_waiting) {
			radio->connection->management_error = reason < 0 ? reason : -EIO;
			complete(&radio->connection->management_completion);
		}
		radio->connection->reason = WLAN_REASON_UNSPECIFIED;
		if (radio->connection->key_waiting) {
			radio->connection->key_error = reason < 0 ? reason : -EIO;
			complete(&radio->connection->key_completion);
		}
		queue_work(system_long_wq, &radio->connection_work);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
}

/* Unsolicited firmware key readiness has no key ID or transaction cookie. */
static void mt6797_radio_authorize_pending(struct mt6797_wifi_radio *radio);

static int mt6797_radio_link_event(void *context,
	const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_radio *radio = context;
	struct mt6797_radio_connection *connection;
	struct mt6797_wifi_ais_mgmt_tx_status status;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&radio->lock, flags);
	ret = mt6797_radio_scan_event(radio, event);
	if (ret) {
		spin_unlock_irqrestore(&radio->lock, flags);
		return ret;
	}
	connection = radio->connection;
	if (connection && connection->management_waiting &&
	    !connection->management_done && !connection->management_error) {
		ret = mt6797_wifi_ais_mgmt_tx_event_match(&connection->ais.auth.pid_owner,
			&connection->management_lease, event, &disposition, &status);
		if (!ret && disposition == MT6797_WIFI_AIS_EVENT_COMPLETE &&
		    status.sequence_number != connection->management_sequence)
			disposition = MT6797_WIFI_AIS_EVENT_IGNORED;
		if (ret || disposition == MT6797_WIFI_AIS_EVENT_COMPLETE) {
			connection->management_error = ret ?: (status.status ? -EIO : 0);
			connection->management_done = !ret;
			complete(&connection->management_completion);
		}
	}
	if (connection && connection->key_waiting) {
		ret = mt6797_wifi_ais_ccmp_ready_match(&connection->key_pending, event, &disposition);
		if (ret || disposition == MT6797_WIFI_AIS_EVENT_COMPLETE) {
			connection->key_waiting = false;
			connection->key_error = ret;
			connection->pairwise_ready = !ret;
			mt6797_radio_authorize_pending(radio);
			complete(&connection->key_completion);
		}
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return ret;
}

/* Classify only structurally complete WPA2-CCMP pairwise key messages. This
 * does not authenticate EAPOL; the supplicant still verifies its MIC and RSN.
 * Offsets include the four-byte 802.1X header, but no Ethernet header.
 */
static unsigned int mt6797_radio_eapol_pairwise_message(const u8 *payload,
	size_t length)
{
	u16 body_length, key_data_length, info, version;

	if (!payload || length < 99 || (payload[0] != 1 && payload[0] != 2) ||
	    payload[1] != 3 || payload[4] != 2)
		return 0;
	body_length = get_unaligned_be16(payload + 2);
	key_data_length = get_unaligned_be16(payload + 97);
	if (body_length != 95U + key_data_length || 4U + body_length > length)
		return 0;
	info = get_unaligned_be16(payload + 5);
	version = info & 7;
	/*
	 * Pairwise, MIC; no reserved/error/request/SMK bits. Descriptor version
	 * 2 (PSK), 3 (PSK-SHA256) or 0 (AKM-defined: SAE). Every AKM connect
	 * admits has a 16-octet MIC, so the offsets above hold for all three.
	 */
	if ((info & 0x0108) != 0x0108 || (info & 0xec30) ||
	    (version != 0 && version != 2 && version != 3))
		return 0;
	if (info & 0x0080)
		return 3;
	/* Some APs omit Secure on M3 and therefore on M4. M2 carries RSN data. */
	if (!(info & 0x1040) && !key_data_length)
		return 4;
	return 0;
}

static int mt6797_radio_data_frame(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_rx_view *view)
{
	struct mt6797_wifi_data_rx data;
	struct mt6797_radio_connection *connection;
	struct mt6797_radio_key *key;
	struct sk_buff *skb;
	unsigned long flags;
	bool secure;
	int ret;

	ret = mt6797_wifi_data_rx_decode(view->packet, view->packet_length, &data);
	if (ret) {
		/* Only descriptor metadata: no addresses, payloads or key material. */
		if (view->packet && view->packet_length >= MT6797_WIFI_RX_DESCRIPTOR_LENGTH) {
			u16 type = get_unaligned_le16(view->packet + 2);
			u16 control = 0;
			bool has_control = (type & BIT(12)) && view->packet_length >= 32;

			/* Group4 is first when present, including on translated RX. */
			if (has_control)
				control = get_unaligned_le16(view->packet + 16);
			dev_warn_ratelimited(wiphy_dev(radio->wdev.wiphy),
				"RX data rejected error=%d type=%#x header=%u status=%#x cipher=%u group4=%u control=%#x\n",
				ret, type, view->header_length,
				get_unaligned_le16(view->packet + 10),
				view->packet[9] >> 4, has_control, control);
		}
		goto drop;
	}
	if (data.length > radio->netdev->mtu + ETH_HLEN + VLAN_HLEN)
		goto drop;
	skb = netdev_alloc_skb_ip_align(radio->netdev, data.length);
	if (!skb)
		goto drop;
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (!connection || !connection->connected || connection->cancel || radio->stopping ||
	    !ether_addr_equal(data.transmitter, connection->bss->bssid) ||
	    data.bss_index != connection->ais.runtime.bss.bss_index ||
	    (!data.multicast && !ether_addr_equal(data.ethernet, radio->netdev->dev_addr)))
		goto reject;
	secure = connection->join.negotiated.security != MT6797_WIFI_AIS_SECURITY_OPEN;
	key = data.multicast ? &connection->group_key : &connection->pairwise_key;
	if (data.wlan_index != (data.multicast ? connection->ais.runtime.sta.ap_bmc_group.index :
	    connection->ais.runtime.sta.pairwise.index))
		goto reject;
	if (data.protected) {
		if (!secure || key->updating || !key->installed || key->index != data.key_index ||
		    (!data.multicast && !connection->pairwise_ready))
			goto reject;
	} else if (secure && !data.eapol) {
		goto reject;
	}
	/* Only AP-originated, individually addressed EAPOL can bypass the port. */
	if (data.eapol && (data.multicast ||
	    !ether_addr_equal(data.ethernet + ETH_ALEN, connection->bss->bssid)))
		goto reject;
	if (!connection->authorized && !data.eapol)
		goto reject;
	if (data.protected && mt6797_wifi_data_rx_replay(&key->rx_pn[data.tid], data.pn))
		goto reject;
	/* GEN3 secSetKeyCmdAction: reply to plaintext M3 with plaintext M4,
	 * even if the supplicant installs PTK/GTK before queuing its reply.
	 * Keep the choice for retransmitted M4; a later M3 replaces it.
	 */
	if (data.eapol) {
		unsigned int message = mt6797_radio_eapol_pairwise_message(
			data.payload, data.length - ETH_HLEN);

		if (message == 3)
			connection->pairwise_reply_plaintext = !data.protected;
		dev_info_ratelimited(wiphy_dev(radio->wdev.wiphy),
			"EAPOL RX pairwise_message=%u protected=%u length=%zu\n",
			message, data.protected, data.length - ETH_HLEN);
	}
	skb_put_data(skb, data.ethernet, ETH_HLEN);
	skb_put_data(skb, data.payload, data.length - ETH_HLEN);
	skb->protocol = eth_type_trans(skb, radio->netdev);
	skb->ip_summed = CHECKSUM_NONE;
	spin_unlock_irqrestore(&radio->lock, flags);
	DEV_STATS_INC(radio->netdev, rx_packets);
	DEV_STATS_ADD(radio->netdev, rx_bytes, data.length);
	netif_rx(skb);
	return 0;
reject:
	spin_unlock_irqrestore(&radio->lock, flags);
	dev_kfree_skb_any(skb);
drop:
	DEV_STATS_INC(radio->netdev, rx_dropped);
	return 0; /* Bad over-the-air data is not a fatal HIF transport error. */
}

static int mt6797_radio_beacon(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_rx_view *view)
{
	struct mt6797_wifi_ais_pm_params params;
	struct mt6797_radio_connection *connection;
	const u8 *frame = view->payload;
	unsigned long flags;
	enum mt6797_wifi_rx_band band;
	u8 channel;

	if (mt6797_wifi_ais_pm_beacon_parse(frame, view->payload_length, &params) ||
	    mt6797_wifi_rx_channel_decode(view->channel, &band, &channel))
		return 0;
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (connection && connection->connected && !connection->cancel && !radio->stopping &&
	    !connection->beacon_seen &&
	    band == connection->join.negotiated.channel.band &&
	    channel == connection->join.negotiated.channel.primary &&
	    ether_addr_equal(frame + 10, connection->bss->bssid) &&
	    ether_addr_equal(frame + 16, connection->bss->bssid) &&
	    (ether_addr_equal(frame + 4, radio->netdev->dev_addr) || is_broadcast_ether_addr(frame + 4))) {
		params.association_id = connection->join.negotiated.association_id;
		connection->pm = params;
		connection->beacon_seen = true;
		complete(&connection->pm_completion);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return 0;
}

/* connection_lock serializes this notification with key installation/teardown.
 * IRQ delivery supplies only the owned beacon metadata and completion.
 */
static int mt6797_radio_pm_send(struct mt6797_wifi_radio *radio, bool connected)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	u8 command[MT6797_WIFI_AIS_PM_CONNECTED_LENGTH];
	size_t length = connected ? sizeof(command) : MT6797_WIFI_AIS_PM_ABORT_LENGTH;
	int ret;

	mutex_lock(&radio->irq->lifecycle_lock);
	if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME ||
	    (connected && mt6797_radio_connection_cancelled(radio))) {
		ret = -ENOTCONN;
		goto out;
	}
	if (++radio->command_sequence > 255)
		radio->command_sequence = 3;
	ret = mt6797_wifi_ais_pm_build(&runtime->bss, &runtime->sta,
		&connection->pm, connected, radio->command_sequence, command, sizeof(command));
	if (ret)
		goto out;
	ret = mt6797_wifi_hif_runtime_send(radio->irq, MT6797_WIFI_MCR_WTDR1, command, length);
	if (ret)
		mt6797_wifi_hif_runtime_fail(radio->irq, ret);
	else
		connection->pm_started = connected;
out:
	mutex_unlock(&radio->irq->lifecycle_lock);
	return ret;
}

static int mt6797_radio_finish_pm(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	s64 remaining = ktime_ms_delta(connection->ais.channel_deadline, ktime_get());
	int ret;

	if (remaining <= 0 ||
	    !wait_for_completion_timeout(&connection->pm_completion,
		msecs_to_jiffies(min_t(s64, 1500, remaining))))
		return -ETIMEDOUT;
	if (mt6797_radio_connection_cancelled(radio))
		return -ECANCELED;
	if (ktime_ms_delta(connection->ais.channel_deadline, ktime_get()) <= 0)
		return -ETIMEDOUT;
	if (!connection->beacon_seen)
		return -EPROTO;
	ret = mt6797_radio_pm_send(radio, true);
	if (ret)
		return ret;
	if (mt6797_radio_connection_cancelled(radio))
		return -ECANCELED;
	ret = mt6797_wifi_ais_hif_release_channel(&connection->ais);
	if (!ret)
		dev_info(wiphy_dev(radio->wdev.wiphy),
			"associated beacon accepted; PM connected and join channel released\n");
	return ret;
}

static bool mt6797_radio_pmf_frame(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_rx_view *view);

static int mt6797_radio_link_frame(void *context,
	const struct mt6797_wifi_rx_view *view)
{
	struct mt6797_wifi_radio *radio = context;
	struct mt6797_radio_connection *connection;
	const u8 *frame = view->payload;
	unsigned long flags;
	u16 control;

	/* A scan while associated collects every beacon and probe response. */
	mt6797_radio_frame(radio, view);
	if (view->kind == MT6797_WIFI_RX_ETHERNET ||
	    view->kind == MT6797_WIFI_RX_NATIVE_DATA)
		return mt6797_radio_data_frame(radio, view);
	if (view->kind != MT6797_WIFI_RX_IEEE80211 || view->header_translated ||
	    view->header_length != 24 || !frame || view->payload_length < 26)
		return 0;
	control = get_unaligned_le16(frame);
	if ((control & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) == IEEE80211_STYPE_BEACON)
		return mt6797_radio_beacon(radio, view);
	if (mt6797_radio_pmf_frame(radio, view))
		return 0;
	/* Legacy joins do not admit protected management or reassemble fragments. */
	if (control & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_TODS |
		       IEEE80211_FCTL_FROMDS | IEEE80211_FCTL_MOREFRAGS |
		       IEEE80211_FCTL_PROTECTED | IEEE80211_FCTL_ORDER) ||
	    (get_unaligned_le16(frame + 22) & IEEE80211_SCTL_FRAG))
		return 0;
	if ((control & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) != IEEE80211_STYPE_DEAUTH &&
	    (control & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) != IEEE80211_STYPE_DISASSOC)
		return 0;
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (connection && ether_addr_equal(frame + 10, connection->bss->bssid) &&
	    ether_addr_equal(frame + 16, connection->bss->bssid) &&
	    (ether_addr_equal(frame + 4, radio->netdev->dev_addr) || is_broadcast_ether_addr(frame + 4))) {
		connection->cancel = true;
		complete(&connection->pm_completion);
		connection->peer_disconnect = true;
		connection->reason = get_unaligned_le16(frame + 24);
		queue_work(system_long_wq, &radio->connection_work);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return 0;
}

static int mt6797_radio_igtk_cleanup(struct mt6797_wifi_radio *radio);

/* Association needs its own response window inside the same channel grant. */
#define MT6797_RADIO_SAE_ASSOC_RESERVE_MS	600
#define MT6797_RADIO_SAE_MIN_BUDGET_MS		200
#define MT6797_RADIO_SAE_TX_TIMEOUT_MS		500

static int mt6797_radio_sae_request(struct mt6797_wifi_radio *radio,
	enum nl80211_external_auth_action action)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct cfg80211_external_auth_params request = {
		.action = action,
		.key_mgmt_suite = WLAN_AKM_SUITE_SAE,
	};

	memcpy(request.bssid, connection->bss->bssid, ETH_ALEN);
	request.ssid.ssid_len = connection->join.negotiated.ssid_length;
	memcpy(request.ssid.ssid, connection->join.negotiated.ssid, request.ssid.ssid_len);
	/* Fails without an NL80211_ATTR_SOCKET_OWNER connection owner. */
	return cfg80211_external_auth_request(radio->netdev, &request, GFP_KERNEL);
}

/*
 * Firmware-independent WPA3 authentication: wpa_supplicant computes SAE and
 * exchanges frames through mgmt_tx/cfg80211_rx_mgmt while this worker owns the
 * AIS join channel. The worker holds connection_lock; the mailbox callbacks
 * take only radio->lock, so cfg80211 can deliver them while we wait.
 */
static int mt6797_radio_sae(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_hif *ais = &connection->ais;
	struct mt6797_wifi_ais_hif_ext_auth_request tx = {};
	struct mt6797_wifi_ais_mgmt_tx_status tx_status;
	unsigned long flags;
	size_t length;
	ktime_t deadline;
	u16 status = WLAN_STATUS_UNSPECIFIED_FAILURE;
	u64 cookie;
	bool pending, ack, done = false;
	int ret, end;

	deadline = ktime_sub_ms(ais->channel_deadline, MT6797_RADIO_SAE_ASSOC_RESERVE_MS);
	if (ktime_ms_delta(deadline, ktime_get()) < MT6797_RADIO_SAE_MIN_BUDGET_MS) {
		dev_warn(wiphy_dev(radio->wdev.wiphy),
			"SAE skipped: join channel grant leaves %lld ms\n",
			ktime_ms_delta(ais->channel_deadline, ktime_get()));
		return -ETIMEDOUT;
	}
	ret = mt6797_wifi_ais_hif_ext_auth_begin(ais);
	if (ret)
		return ret;
	spin_lock_irqsave(&radio->lock, flags);
	connection->sae_active = !connection->cancel && !radio->stopping;
	ret = connection->sae_active ? 0 : -ECANCELED;
	spin_unlock_irqrestore(&radio->lock, flags);
	if (!ret && mt6797_radio_sae_request(radio, NL80211_EXTERNAL_AUTH_START))
		ret = -ENOTCONN;
	while (!ret) {
		if (mt6797_radio_connection_cancelled(radio)) {
			ret = -ECANCELED;
			break;
		}
		while (!mt6797_wifi_ais_hif_ext_auth_take(ais, connection->sae_scratch,
				sizeof(connection->sae_scratch), &length))
			cfg80211_rx_mgmt(&radio->wdev, connection->bss->channel->center_freq, 0,
				connection->sae_scratch, length, 0);
		spin_lock_irqsave(&radio->lock, flags);
		pending = connection->sae_tx_pending;
		if (pending) {
			length = connection->sae_frame_length;
			memcpy(connection->sae_scratch, connection->sae_frame, length);
			cookie = connection->sae_cookie;
			ack = connection->sae_tx_ack;
		}
		done = connection->sae_done;
		status = connection->sae_status;
		spin_unlock_irqrestore(&radio->lock, flags);
		if (pending) {
			connection->auth_sequence = (connection->auth_sequence + 1) & GENMASK(11, 0);
			tx.frame = connection->sae_scratch;
			tx.length = length;
			tx.sequence_number = connection->auth_sequence;
			tx.fixed_rate = connection->join.negotiated.default_rate;
			tx.timeout_ms = MT6797_RADIO_SAE_TX_TIMEOUT_MS;
			ret = mt6797_wifi_ais_hif_ext_auth_send(ais, &tx, &tx_status);
			spin_lock_irqsave(&radio->lock, flags);
			connection->sae_tx_pending = false;
			spin_unlock_irqrestore(&radio->lock, flags);
			if (ack)
				cfg80211_mgmt_tx_status(&radio->wdev, cookie, connection->sae_scratch,
					length, !ret, GFP_KERNEL);
			/* Unacknowledged or malformed frames are retried by SAE itself. */
			if (ret == -EREMOTEIO || ret == -EINVAL)
				ret = 0;
			if (ret)
				break;
			continue;
		}
		if (done)
			break;
		if (ktime_ms_delta(deadline, ktime_get()) <= 0) {
			ret = -ETIMEDOUT;
			break;
		}
		mt6797_wifi_ais_hif_ext_auth_wait(ais, deadline);
	}
	spin_lock_irqsave(&radio->lock, flags);
	connection->sae_active = false;
	connection->sae_tx_pending = false;
	spin_unlock_irqrestore(&radio->lock, flags);
	memzero_explicit(connection->sae_frame, sizeof(connection->sae_frame));
	memzero_explicit(connection->sae_scratch, sizeof(connection->sae_scratch));
	end = mt6797_wifi_ais_hif_ext_auth_end(ais, done ? status : WLAN_STATUS_UNSPECIFIED_FAILURE);
	if (!done && ret != -ENOTCONN)
		mt6797_radio_sae_request(radio, NL80211_EXTERNAL_AUTH_ABORT);
	if (!ret)
		ret = end;
	if (ret)
		dev_warn(wiphy_dev(radio->wdev.wiphy),
			"SAE authentication failed error=%d status=%u tx=%u acked=%u rx=%u dropped=%u\n",
			ret, done ? status : 0, ais->ext_auth.tx_frames, ais->ext_auth.tx_acked,
			ais->ext_auth.rx_frames, ais->ext_auth.rx_dropped);
	if (!ret && connection->sae_pmkid_set) {
		size_t ies_length = connection->join.association.ies_length;

		ret = mt6797_wifi_ais_rsn_set_pmkid(connection->ies, &ies_length,
			sizeof(connection->ies), connection->sae_pmkid);
		if (!ret)
			connection->join.association.ies_length = ies_length;
	}
	return ret;
}

static void mt6797_radio_connection_work(struct work_struct *work)
{
	struct mt6797_wifi_radio *radio = container_of(work, struct mt6797_wifi_radio, connection_work);
	struct mt6797_radio_connection *connection;
	struct mt6797_wifi_scan_hif_result scan_result;
	struct mt6797_wifi_scan_hif_request scan = {};
	struct mt6797_wifi_ais_hif_request start = {};
	struct mt6797_wifi_ais_hif_auth_request auth = {};
	struct mt6797_wifi_ais_hif_auth_result auth_result;
	struct mt6797_wifi_ais_join_params negotiated;
	struct mt6797_wifi_hif_runtime_sink sink = {
		.event = mt6797_radio_link_event, .frame = mt6797_radio_link_frame,
		.abort = mt6797_radio_link_abort, .context = radio,
	};
	unsigned long flags;
	unsigned int sequence;
	s64 auth_remaining;
	bool connected, peer_disconnect;
	u16 disconnect_reason;
	u16 status = WLAN_STATUS_UNSPECIFIED_FAILURE;
	int ret = -ECANCELED;
	int teardown_ret;

	mutex_lock(&radio->connection_lock);
	connection = radio->connection;
	if (!connection)
		goto unlock;
	if (mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	if (connection->connected)
		goto unlock;
	sequence = radio->command_sequence + 1;
	if (sequence > 245)
		sequence = 3;
	/* Reserve scan/cancel, AIS start/cleanup and BSS/activation commands. */
	radio->command_sequence = sequence + 9;
	scan.command_sequence = sequence;
	scan.cancel_sequence = sequence + 1;
	scan.scan_sequence = ++radio->scan_sequence;
	scan.band = connection->join.negotiated.channel.band;
	scan.channel = connection->join.negotiated.channel.primary;
	scan.timeout_ms = 5000;
	scan.max_age_ms = 5000;
	memcpy(scan.key.bssid, connection->bss->bssid, ETH_ALEN);
	memcpy(scan.key.ssid, connection->join.negotiated.ssid, connection->join.negotiated.ssid_length);
	scan.key.ssid_length = connection->join.negotiated.ssid_length;
	ret = mt6797_wifi_scan_hif_init(&connection->scan, radio->irq, NULL);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	ret = mt6797_wifi_scan_hif_run(&connection->scan, &scan, &scan_result);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	connection->join.negotiated.rcpi = scan_result.target.rcpi;
	ret = mt6797_wifi_ais_hif_init(&connection->ais, radio->irq, radio->config,
		&radio->capability, sequence + 2, ++radio->channel_token, NULL);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	connection->ais.auth.pid_owner.next_pid = radio->management_pid;
	start.target_owner = &connection->scan.target_owner;
	start.target = &scan_result.target;
	start.now_ms = ktime_to_ms(ktime_get());
	start.response_timeout_ms = 1000;
	ret = mt6797_wifi_ais_hif_start(&connection->ais, &start);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	auth_remaining = ktime_ms_delta(connection->ais.channel_deadline, ktime_get());
	if (auth_remaining <= 0) {
		ret = -ETIMEDOUT;
		goto cleanup;
	}
	if (connection->sae) {
		ret = mt6797_radio_sae(radio);
	} else {
		auth.response_timeout_ms = min_t(s64, 1000, auth_remaining);
		auth.sequence_number = ++connection->auth_sequence;
		auth.fixed_rate = connection->join.negotiated.default_rate;
		ret = mt6797_wifi_ais_hif_open_auth(&connection->ais, &auth, &auth_result);
	}
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	connection->join.sequence_number = (connection->auth_sequence + 1) & GENMASK(11, 0);
	connection->management_sequence = connection->join.sequence_number;
	ret = mt6797_wifi_ais_hif_associate(&connection->ais, &connection->join, &negotiated);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	connection->join.negotiated = negotiated;
	mutex_lock(&radio->irq->lifecycle_lock);
	ret = mt6797_wifi_hif_runtime_sink_attach(radio->irq, &sink);
	connection->link_attached = !ret;
	mutex_unlock(&radio->irq->lifecycle_lock);
	if (ret)
		goto cleanup;
	spin_lock_irqsave(&radio->lock, flags);
	if (!connection->cancel && !radio->stopping)
		connection->connected = true;
	connection->authorized = connection->join.negotiated.security == MT6797_WIFI_AIS_SECURITY_OPEN;
	connected = connection->connected;
	spin_unlock_irqrestore(&radio->lock, flags);
	if (!connected)
		goto cleanup;
	netif_carrier_on(radio->netdev);
	netif_wake_queue(radio->netdev);
	cfg80211_connect_result(radio->netdev, connection->bss->bssid,
		connection->join.association.ies, connection->join.association.ies_length,
		connection->ais.join.response + MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH,
		connection->ais.join.response_length - MT6797_WIFI_AIS_ASSOC_RESPONSE_FIXED_LENGTH,
		WLAN_STATUS_SUCCESS, GFP_KERNEL);
	ret = mt6797_radio_finish_pm(radio);
	if (ret)
		dev_warn(wiphy_dev(radio->wdev.wiphy),
			"PM connection setup failed error=%d beacon=%u started=%u\n",
			ret, connection->beacon_seen, connection->pm_started);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
	goto unlock;

cleanup:
	spin_lock_irqsave(&radio->lock, flags);
	connection->cancel = true;
	mt6797_radio_sa_query_stop(radio);
	netif_stop_queue(radio->netdev);
	netif_carrier_off(radio->netdev);
	spin_unlock_irqrestore(&radio->lock, flags);
	mt6797_radio_purge_tx(radio);
	if (connection->link_attached) {
		/*
		 * A scan admitted while associated receives its events through the
		 * link sink; cancel is set, so no new one starts. Let it finish.
		 */
		flush_work(&radio->scan_work);
		mutex_lock(&radio->irq->lifecycle_lock);
		mt6797_wifi_hif_runtime_sink_detach(radio->irq, radio);
		mutex_unlock(&radio->irq->lifecycle_lock);
	}
	teardown_ret = mt6797_radio_igtk_cleanup(radio);
	if (teardown_ret)
		dev_warn(wiphy_dev(radio->wdev.wiphy),
			 "IGTK removal on teardown failed error=%d\n", teardown_ret);
	if (connection->ais.initialized) {
		if (connection->pm_started) {
			teardown_ret = mt6797_radio_pm_send(radio, false);
			if (teardown_ret)
				dev_warn(wiphy_dev(radio->wdev.wiphy),
					 "PM disconnect on teardown failed error=%d\n",
					 teardown_ret);
		}
		radio->management_pid = connection->ais.auth.pid_owner.next_pid;
		if (connection->ais.runtime.state == MT6797_WIFI_AIS_RUNTIME_STA_STATE1 ||
		    connection->ais.runtime.state == MT6797_WIFI_AIS_RUNTIME_STA_STATE3) {
			teardown_ret = mt6797_wifi_ais_hif_stop(&connection->ais);
			if (teardown_ret) {
				/*
				 * The firmware may still be joined to the BSS, which
				 * blocks later scans and joins. Fence the transport
				 * so recovery restarts the firmware from a known state.
				 */
				dev_warn(wiphy_dev(radio->wdev.wiphy),
					 "leaving the BSS failed error=%d state=%d; restarting WLAN\n",
					 teardown_ret, connection->ais.runtime.state);
				mutex_lock(&radio->irq->lifecycle_lock);
				mt6797_wifi_hif_runtime_fail(radio->irq, teardown_ret);
				mutex_unlock(&radio->irq->lifecycle_lock);
			}
		}
	}
	if (connection->scan.initialized)
		mt6797_wifi_scan_hif_close(&connection->scan);
	spin_lock_irqsave(&radio->lock, flags);
	connected = connection->connected;
	peer_disconnect = connection->peer_disconnect;
	disconnect_reason = connection->reason;
	spin_unlock_irqrestore(&radio->lock, flags);
	if (!connected) {
		if (connection->ais.join.status_code)
			status = connection->ais.join.status_code;
		else if (connection->ais.auth.response.status_code)
			status = connection->ais.auth.response.status_code;
		cfg80211_connect_result(radio->netdev, connection->bss->bssid,
			NULL, 0, NULL, 0, status, GFP_KERNEL);
	} else {
		cfg80211_disconnected(radio->netdev, disconnect_reason ?: WLAN_REASON_DEAUTH_LEAVING,
			NULL, 0, !peer_disconnect, GFP_KERNEL);
	}
	spin_lock_irqsave(&radio->lock, flags);
	radio->connection = NULL;
	spin_unlock_irqrestore(&radio->lock, flags);
	cfg80211_put_bss(radio->wdev.wiphy, connection->bss);
	kfree_sensitive(connection);
unlock:
	mutex_unlock(&radio->connection_lock);
}

static int mt6797_radio_connect(struct wiphy *wiphy, struct net_device *netdev,
	struct cfg80211_connect_params *sme)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_connection *connection;
	const struct cfg80211_bss_ies *peer_ies;
	const u8 *rsn;
	struct mt6797_wifi_ais_join_params *params;
	struct mt6797_wifi_ais_rsn_request policy = {};
	struct mt6797_wifi_ais_rsn_result negotiated = {};
	unsigned long flags;
	size_t offset, rates_length;
	int ret = -EOPNOTSUPP;
	bool secure = sme->privacy;
	bool have_rsn = false, have_rsnx = false;

	if (!sme->ssid || !sme->ssid_len || sme->ssid_len > 32 || sme->ie_len > 1024 ||
	    (sme->ie_len && !sme->ie) ||
	    (sme->crypto.control_port_ethertype && sme->crypto.control_port_ethertype != htons(ETH_P_PAE)) ||
	    sme->key_len || sme->pbss || sme->prev_bssid || sme->crypto.control_port_over_nl80211 ||
	    (sme->mfp != NL80211_MFP_NO && sme->mfp != NL80211_MFP_REQUIRED) || sme->want_1x ||
	    (sme->flags & ~(ASSOC_REQ_DISABLE_HT | ASSOC_REQ_DISABLE_VHT |
		ASSOC_REQ_DISABLE_HE | ASSOC_REQ_DISABLE_EHT | ASSOC_REQ_DISABLE_UHR |
		CONNECT_REQ_EXTERNAL_AUTH_SUPPORT)) ||
	    (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM && sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC &&
	     sme->auth_type != NL80211_AUTHTYPE_SAE))
		return -EOPNOTSUPP;
	if (secure) {
		/* wpa_supplicant 2.11 reports SAE as WPA3, 2.10 as WPA2. */
		if ((sme->crypto.wpa_versions != NL80211_WPA_VERSION_2 &&
		     (sme->crypto.wpa_versions != NL80211_WPA_VERSION_3 ||
		      sme->crypto.akm_suites[0] != WLAN_AKM_SUITE_SAE)) ||
		    sme->crypto.n_ciphers_pairwise != 1 || sme->crypto.ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP ||
		    sme->crypto.cipher_group != WLAN_CIPHER_SUITE_CCMP || sme->crypto.n_akm_suites != 1)
			return -EOPNOTSUPP;
	} else if (sme->crypto.wpa_versions || sme->crypto.n_ciphers_pairwise ||
		   sme->crypto.cipher_group || sme->crypto.n_akm_suites ||
		   sme->mfp != NL80211_MFP_NO || sme->auth_type == NL80211_AUTHTYPE_SAE) {
		return -EOPNOTSUPP;
	}
	/* User IEs cannot override SSID/rates or advertise unsupported HT/VHT/WMM. */
	for (offset = 0; offset < sme->ie_len; offset += sme->ie[offset + 1] + 2) {
		if (sme->ie_len - offset < 2 || sme->ie[offset + 1] > sme->ie_len - offset - 2)
			return -EINVAL;
		if (sme->ie[offset] == WLAN_EID_RSNX) {
			/* SAE hash-to-element capability; the AP validates its use. */
			if (!secure || have_rsnx || !sme->ie[offset + 1] || sme->ie[offset + 1] > 2)
				return -EOPNOTSUPP;
			have_rsnx = true;
			continue;
		}
		/*
		 * Supported Operating Classes: wpa_supplicant derives it from this
		 * wiphy's channels once it has the AP's scan result.
		 */
		if (sme->ie[offset] == WLAN_EID_SUPPORTED_REGULATORY_CLASSES) {
			if (!sme->ie[offset + 1])
				return -EINVAL;
			continue;
		}
		if (sme->ie[offset] != WLAN_EID_RSN && sme->ie[offset] != WLAN_EID_EXT_CAPABILITY)
			return -EOPNOTSUPP;
		if (sme->ie[offset] == WLAN_EID_RSN) {
			if (have_rsn)
				return -EINVAL;
			have_rsn = true;
		}
	}
	rsn = cfg80211_find_ie(WLAN_EID_RSN, sme->ie, sme->ie_len);
	if (secure && !rsn)
		return -EOPNOTSUPP;
	if (!secure && rsn)
		return -EINVAL;
	/* Reject what the station alone makes invalid before touching the BSS. */
	if (secure) {
		struct mt6797_wifi_ais_rsn own;

		if (sme->crypto.akm_suites[0] != WLAN_AKM_SUITE_PSK &&
		    sme->crypto.akm_suites[0] != WLAN_AKM_SUITE_PSK_SHA256 &&
		    sme->crypto.akm_suites[0] != WLAN_AKM_SUITE_SAE)
			return -EOPNOTSUPP;
		if (mt6797_wifi_ais_rsn_parse(rsn, &own))
			return -EOPNOTSUPP;
	}
	policy.sta_ie = rsn;
	policy.akm = sme->crypto.akm_suites[0];
	policy.mfp_required = sme->mfp == NL80211_MFP_REQUIRED;
	policy.sae_auth = sme->auth_type == NL80211_AUTHTYPE_SAE;
	policy.external_auth = sme->flags & CONNECT_REQ_EXTERNAL_AUTH_SUPPORT;
	connection = kzalloc(sizeof(*connection), GFP_KERNEL);
	if (!connection)
		return -ENOMEM;
	init_completion(&connection->key_completion);
	init_completion(&connection->pm_completion);
	init_completion(&connection->management_completion);
	connection->management_sequence = 2; /* Authentication and association used 1/2. */
	connection->control_port_no_encrypt = sme->crypto.control_port_no_encrypt;
	connection->bss = cfg80211_get_bss(wiphy, sme->channel, sme->bssid,
		sme->ssid, sme->ssid_len, IEEE80211_BSS_TYPE_ESS,
		secure ? IEEE80211_PRIVACY_ON : IEEE80211_PRIVACY_OFF);
	if (!connection->bss) {
		ret = -ENOENT;
		goto free_connection;
	}
	if (connection->bss->channel->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR))
		goto put_bss;
	params = &connection->join.negotiated;
	params->channel.band = connection->bss->channel->band == NL80211_BAND_2GHZ ? 1 : 2;
	params->channel.primary = ieee80211_frequency_to_channel(connection->bss->channel->center_freq);
	params->ssid_length = sme->ssid_len;
	memcpy(params->ssid, sme->ssid, sme->ssid_len);
	params->listen_interval = 10;
	params->disconnect_threshold = 10;
	params->security = secure ? MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP : MT6797_WIFI_AIS_SECURITY_OPEN;
	rcu_read_lock();
	peer_ies = rcu_dereference(connection->bss->ies);
	ret = peer_ies ? mt6797_wifi_ais_join_rates_parse(peer_ies->data, peer_ies->len,
		connection->bss->capability, params) : -ENOENT;
	if (!ret && secure) {
		policy.ap_ie = cfg80211_find_ie(WLAN_EID_RSN, peer_ies->data, peer_ies->len);
		ret = mt6797_wifi_ais_rsn_policy(&policy, &negotiated);
	}
	rcu_read_unlock();
	if (ret)
		goto put_bss;
	/* Firmware keeps WPA2-PSK/CCMP auth mode: GEN3 has no SAE/PMF mode and
	 * the data path is CCMP either way; PMF and SAE are host-side.
	 */
	connection->pmf_negotiated = negotiated.pmf;
	connection->sae = negotiated.sae;
	connection->ies[0] = WLAN_EID_SSID;
	connection->ies[1] = sme->ssid_len;
	memcpy(connection->ies + 2, sme->ssid, sme->ssid_len);
	offset = sme->ssid_len + 2;
	ret = mt6797_wifi_ais_join_rates_ies_build(params, connection->ies + offset,
		sizeof(connection->ies) - offset, &rates_length);
	if (ret)
		goto put_bss;
	offset += rates_length;
	if (sme->ie_len)
		memcpy(connection->ies + offset, sme->ie, sme->ie_len);
	connection->join.association.ies = connection->ies;
	connection->join.association.ies_length = offset + sme->ie_len;
	connection->join.association.listen_interval = params->listen_interval;
	connection->join.association.capability = WLAN_CAPABILITY_ESS |
		(secure ? WLAN_CAPABILITY_PRIVACY : 0) |
		(params->short_preamble ? WLAN_CAPABILITY_SHORT_PREAMBLE : 0) |
		(params->short_slot ? WLAN_CAPABILITY_SHORT_SLOT_TIME : 0);
	connection->join.timeout_ms = 1500;
	connection->join.sequence_number = 2;
	spin_lock_irqsave(&radio->lock, flags);
	ret = radio->stopping ? -ESHUTDOWN : radio->request || radio->connection ? -EBUSY : 0;
	if (!ret) {
		radio->connection = connection;
		radio->tx_diag.connection_generation++;
		queue_work(system_long_wq, &radio->connection_work);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	if (!ret)
		return 0;
put_bss:
	cfg80211_put_bss(wiphy, connection->bss);
free_connection:
	kfree(connection);
	return ret;
}

static int mt6797_radio_disconnect(struct wiphy *wiphy, struct net_device *netdev,
	u16 reason)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	if (radio->connection) {
		radio->connection->cancel = true;
		mt6797_radio_sa_query_stop(radio);
		complete(&radio->connection->pm_completion);
		radio->connection->reason = reason;
		queue_work(system_long_wq, &radio->connection_work);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return 0;
}

/* connection_lock owns the station and serializes key operations with teardown. */
static int mt6797_radio_key_connection(struct mt6797_wifi_radio *radio,
	bool pairwise, const u8 *mac)
{
	struct mt6797_radio_connection *connection = radio->connection;

	if (mt6797_radio_connection_cancelled(radio) || !connection->connected ||
	    !connection->link_attached)
		return -ENOTCONN;
	if (connection->join.negotiated.security != MT6797_WIFI_AIS_SECURITY_WPA2_PSK_CCMP)
		return -EOPNOTSUPP;
	if (pairwise ? !mac || !ether_addr_equal(mac, connection->bss->bssid) :
	    mac && !is_broadcast_ether_addr(mac))
		return -EINVAL;
	return 0;
}

static int mt6797_radio_key_send(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_ais_key_params *params)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	u8 command[MT6797_WIFI_AIS_KEY_COMMAND_LENGTH] = {};
	bool wait_ready = params->pairwise && !params->remove;
	unsigned long flags;
	int ret;

	mutex_lock(&radio->irq->lifecycle_lock);
	if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME ||
	    mt6797_radio_connection_cancelled(radio)) {
		ret = -ENOTCONN;
		goto out;
	}
	if (++radio->command_sequence > 255)
		radio->command_sequence = 3;
	ret = mt6797_wifi_ais_ccmp_key_build(&runtime->bss, &runtime->sta,
		params, radio->command_sequence, command, sizeof(command));
	if (ret)
		goto out;
	if (wait_ready) {
		ret = mt6797_wifi_ais_sta_pending_init(&runtime->bss, &runtime->sta,
			radio->command_sequence, &connection->key_pending);
		if (ret)
			goto out;
		reinit_completion(&connection->key_completion);
		spin_lock_irqsave(&radio->lock, flags);
		connection->key_error = 0;
		connection->pairwise_ready = false;
		connection->key_waiting = true;
		connection->pairwise_attempted = true;
		connection->pairwise_wtbl = runtime->sta.pairwise;
		spin_unlock_irqrestore(&radio->lock, flags);
	}
	ret = mt6797_wifi_hif_runtime_send(radio->irq, MT6797_WIFI_MCR_WTDR1,
		command, sizeof(command));
	/* No secret command buffer is retained while waiting for the IRQ. */
	memzero_explicit(command, sizeof(command));
	if (!ret && wait_ready &&
	    !wait_for_completion_timeout(&connection->key_completion, msecs_to_jiffies(1000)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&radio->lock, flags);
	connection->key_waiting = false;
	if (!ret && wait_ready)
		ret = connection->key_error ?: (connection->pairwise_ready ? 0 : -EIO);
	if (!ret && (connection->cancel || radio->stopping))
		ret = -ECANCELED;
	spin_unlock_irqrestore(&radio->lock, flags);
	/* A possibly consumed command cannot be retried in this firmware lifetime. */
	if (ret)
		mt6797_wifi_hif_runtime_fail(radio->irq, ret);
out:
	memzero_explicit(command, sizeof(command));
	mutex_unlock(&radio->irq->lifecycle_lock);
	return ret;
}

static bool mt6797_radio_sta_owner_same(const struct mt6797_wifi_ais_sta_pending *a,
	const struct mt6797_wifi_ais_sta_pending *b)
{
	return a->generation == b->generation && a->bss_cookie == b->bss_cookie &&
		a->sta_cookie == b->sta_cookie && a->bss_index == b->bss_index &&
		a->sta_index == b->sta_index && ether_addr_equal(a->peer_addr, b->peer_addr);
}

static bool mt6797_radio_wtbl_same(const struct mt6797_wifi_ais_wtbl_handle *a,
	const struct mt6797_wifi_ais_wtbl_handle *b)
{
	return a->generation == b->generation && a->cookie == b->cookie &&
		a->owner_cookie == b->owner_cookie && a->role == b->role && a->index == b->index;
}

/* connection_lock pins this association for callbacks/cleanup. RX instead holds
 * radio->lock and requires connected && !cancel: teardown publishes cancel
 * under that lock before mutating/releasing the connection's runtime handles.
 */
static int mt6797_radio_igtk_owner(struct mt6797_wifi_radio *radio,
	struct mt6797_wifi_ais_sta_pending *owner)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	const struct mt6797_wifi_ais_sta_pending *old = &connection->igtk_owner;
	int ret;

	ret = mt6797_wifi_ais_sta_pending_init(&runtime->bss, &runtime->sta, 0, owner);
	if (ret)
		return ret;
	if (!ether_addr_equal(owner->peer_addr, connection->bss->bssid))
		return -ESTALE;
	if (connection->igtk_history_count &&
	    (!mt6797_radio_sta_owner_same(old, owner) ||
	     !mt6797_radio_wtbl_same(&connection->igtk_wtbl, &runtime->sta.ap_bmc_group)))
		return -ESTALE;
	return 0;
}

/* Called under radio->lock or with connection_lock pinning key callbacks. */
static bool mt6797_radio_igtk_ready(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_sta_pending owner;

	return connection && !connection->igtk_updating &&
		(connection->igtk[0].installed || connection->igtk[1].installed) &&
		!mt6797_radio_igtk_owner(radio, &owner);
}

/* radio->lock held. Every key a secure data path needs is in place. */
static bool mt6797_radio_keys_ready(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;

	return connection->join.negotiated.security == MT6797_WIFI_AIS_SECURITY_OPEN ||
		(connection->pairwise_key.installed && connection->pairwise_ready &&
		 connection->group_key.installed &&
		 (!connection->pmf_negotiated || mt6797_radio_igtk_ready(radio)));
}

/*
 * radio->lock held. wpa_supplicant authorizes the port right after the PTK,
 * before it installs the GTK and IGTK; apply that authorization once the last
 * of those keys is in place.
 */
static void mt6797_radio_authorize_pending(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;

	if (connection && connection->authorize_pending && !connection->cancel &&
	    !radio->stopping && mt6797_radio_keys_ready(radio)) {
		connection->authorize_pending = false;
		connection->authorized = true;
	}
}

/* radio->lock protects key/connection flags; connection_lock pins TX lifetime. */
static bool mt6797_radio_management_ready(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_sta_pending owner;
	struct mt6797_wifi_ais_runtime *runtime;

	if (!connection || !connection->connected || connection->cancel || radio->stopping ||
	    !connection->pmf_negotiated || !mt6797_radio_igtk_ready(radio) ||
	    !connection->pairwise_ready || !connection->pairwise_key.installed || connection->pairwise_key.updating)
		return false;
	runtime = &connection->ais.runtime;
	return !mt6797_wifi_ais_sta_pending_init(&runtime->bss, &runtime->sta, 0, &owner) &&
		ether_addr_equal(owner.peer_addr, connection->bss->bssid) &&
		mt6797_radio_sta_owner_same(&connection->key_pending, &owner) &&
		mt6797_radio_wtbl_same(&connection->pairwise_wtbl, &runtime->sta.pairwise);
}

/* One protected action per worker invocation. The PID is released only after
 * a matched TX-done or a proved-not-sent failure. Timeout/ambiguous transport
 * fences the firmware lifetime. TX-done proves completion, not encryption.
 */
static int mt6797_radio_sa_query_tx(struct mt6797_wifi_radio *radio, u8 action,
	const u8 id[2], u64 generation, bool *submitted)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	struct mt6797_wifi_ais_mgmt_pid_owner *owner = &connection->ais.auth.pid_owner;
	struct mt6797_wifi_ais_mgmt_tx_params params = {
		.fixed_rate = connection->join.negotiated.default_rate, .sa_query_protected = true,
	};
	u8 frame[28] = {}, packet[MT6797_WIFI_TX_DESCRIPTOR_LENGTH + sizeof(frame)];
	unsigned long flags;
	unsigned int timeout_ms = 1000;
	s64 remaining;
	size_t length;
	bool opened = false, held = false, uncertain = false;
	int ret, release;

	if (submitted)
		*submitted = false;
	mutex_lock(&radio->irq->lifecycle_lock);
	spin_lock_irqsave(&radio->lock, flags);
	ret = mt6797_radio_management_ready(radio) ? 0 : -ENOTCONN;
	spin_unlock_irqrestore(&radio->lock, flags);
	if (ret || mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME) {
		ret = ret ?: -ESHUTDOWN;
		goto out;
	}
	ret = mt6797_wifi_ais_mgmt_pid_open(owner, &runtime->bss, &runtime->sta);
	if (ret)
		goto out;
	opened = true;
	ret = mt6797_wifi_ais_mgmt_pid_acquire(owner, &connection->management_lease);
	if (ret)
		goto out;
	held = true;
	connection->management_sequence = (connection->management_sequence + 1) & GENMASK(11, 0);
	params.sequence_number = connection->management_sequence;
	put_unaligned_le16(IEEE80211_STYPE_ACTION | IEEE80211_FCTL_PROTECTED, frame);
	memcpy(frame + 4, runtime->sta.peer_addr, ETH_ALEN);
	memcpy(frame + 10, runtime->bss.own_addr, ETH_ALEN);
	memcpy(frame + 16, runtime->sta.peer_addr, ETH_ALEN);
	put_unaligned_le16(params.sequence_number << 4, frame + 22);
	frame[24] = WLAN_CATEGORY_SA_QUERY;
	frame[25] = action;
	memcpy(frame + 26, id, 2);
	ret = mt6797_wifi_ais_mgmt_tx_build(owner, &connection->management_lease,
		&runtime->bss, &runtime->sta, &params, frame, sizeof(frame), packet, sizeof(packet), &length);
	if (ret)
		goto out;
	spin_lock_irqsave(&radio->lock, flags);
	if (connection->cancel || radio->stopping) {
		spin_unlock_irqrestore(&radio->lock, flags);
		ret = -ECANCELED;
		goto out;
	}
	if (action == WLAN_ACTION_SA_QUERY_REQUEST) {
		remaining = ktime_us_delta(connection->sa_query_deadline, ktime_get());
		if (!connection->sa_query_active || connection->sa_query_generation != generation || remaining <= 0) {
			spin_unlock_irqrestore(&radio->lock, flags);
			ret = -ECANCELED;
			goto out;
		}
		timeout_ms = min_t(s64, 1000, DIV_ROUND_UP(remaining, 1000));
	}
	reinit_completion(&connection->management_completion);
	connection->management_error = 0;
	connection->management_done = false;
	connection->management_waiting = true;
	spin_unlock_irqrestore(&radio->lock, flags);
	ret = mt6797_wifi_hif_runtime_send(radio->irq, MT6797_WIFI_MCR_WTDR1, packet, length);
	/* EAGAIN is the credit preflight's proved-not-sent result. An AP can retry
	 * its request; do not hold or accumulate an unbounded response queue.
	 */
	uncertain = ret != -EAGAIN;
	if (submitted)
		*submitted = uncertain;
	if (!ret && !wait_for_completion_timeout(&connection->management_completion, msecs_to_jiffies(timeout_ms)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&radio->lock, flags);
	connection->management_waiting = false;
	if (!ret) {
		ret = connection->management_error;
		if (connection->management_done)
			uncertain = false;
		else
			ret = ret ?: -EIO;
	}
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	if (uncertain) {
		mt6797_wifi_ais_mgmt_pid_mark_terminal(owner);
		mt6797_wifi_hif_runtime_fail(radio->irq, ret ?: -EIO);
	} else {
		release = held ? mt6797_wifi_ais_mgmt_pid_release(owner, &connection->management_lease) : 0;
		if (!release && opened)
			release = mt6797_wifi_ais_mgmt_pid_close(owner);
		if (release) {
			ret = ret ?: release;
			mt6797_wifi_ais_mgmt_pid_mark_terminal(owner);
			mt6797_wifi_hif_runtime_fail(radio->irq, ret);
		}
	}
	mutex_unlock(&radio->irq->lifecycle_lock);
	return ret;
}

static int mt6797_radio_sa_query_response(struct mt6797_wifi_radio *radio, const u8 id[2])
{
	return mt6797_radio_sa_query_tx(radio, WLAN_ACTION_SA_QUERY_RESPONSE, id, 0, NULL);
}

static void mt6797_radio_management_work(struct work_struct *work)
{
	struct mt6797_wifi_radio *radio = container_of(work, struct mt6797_wifi_radio, management_work);
	unsigned long flags;
	u8 id[2];
	bool respond = false;

	mutex_lock(&radio->connection_lock);
	spin_lock_irqsave(&radio->lock, flags);
	if (radio->connection && radio->connection->sa_query_response_pending) {
		respond = mt6797_radio_management_ready(radio);
		memcpy(id, radio->connection->sa_query_id, sizeof(id));
		radio->connection->sa_query_response_pending = false;
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	if (respond)
		mt6797_radio_sa_query_response(radio, id);
	mutex_unlock(&radio->connection_lock);
}

/* All query state is protected by radio->lock. The worker additionally holds
 * connection_lock so neither key callbacks nor teardown can replace its peer.
 */
static bool mt6797_radio_sa_query_current(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;

	return mt6797_radio_management_ready(radio) && connection->authorized &&
		connection->sa_query_active &&
		mt6797_radio_sta_owner_same(&connection->sa_query_owner, &connection->key_pending) &&
		mt6797_radio_wtbl_same(&connection->sa_query_wtbl, &connection->pairwise_wtbl);
}

static void mt6797_radio_sa_query_schedule(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	ktime_t next = connection->sa_query_next;
	s64 remaining;

	if (ktime_after(next, connection->sa_query_deadline) ||
	    connection->sa_query_attempts == ARRAY_SIZE(connection->sa_query_ids))
		next = connection->sa_query_deadline;
	remaining = ktime_us_delta(next, ktime_get());
	mod_delayed_work(system_long_wq, &radio->sa_query_work,
		remaining > 0 ? usecs_to_jiffies(remaining) : 0);
}

static void mt6797_radio_sa_query_start(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_mgmt_rx *trigger)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	ktime_t now = ktime_get();

	if (!mt6797_radio_management_ready(radio) || !connection->authorized ||
	    connection->sa_query_active || ktime_before(now, connection->sa_query_next_start) ||
	    connection->sa_query_generation == U64_MAX ||
	    !ether_addr_equal(trigger->frame + 4, radio->netdev->dev_addr) ||
	    !ether_addr_equal(trigger->frame + 10, connection->bss->bssid) ||
	    !ether_addr_equal(trigger->frame + 16, connection->bss->bssid) ||
	    trigger->bss_index != runtime->bss.bss_index || trigger->wlan_index != runtime->sta.pairwise.index)
		return;
	connection->sa_query_generation++;
	connection->sa_query_owner = connection->key_pending;
	connection->sa_query_wtbl = connection->pairwise_wtbl;
	connection->sa_query_deadline = ktime_add_ms(now, 1024); /* 1000 TU */
	connection->sa_query_next = now;
	connection->sa_query_next_start = ktime_add_ms(now, 10000); /* Supplicant DoS rate limit. */
	connection->sa_query_count = 0;
	connection->sa_query_attempts = 0;
	connection->sa_query_active = true;
	mod_delayed_work(system_long_wq, &radio->sa_query_work, 0);
}

static void mt6797_radio_sa_query_match(struct mt6797_wifi_radio *radio, const u8 id[2])
{
	struct mt6797_radio_connection *connection = radio->connection;
	unsigned int i;

	if (!mt6797_radio_sa_query_current(radio) ||
	    !ktime_before(ktime_get(), connection->sa_query_deadline))
		return;
	for (i = 0; i < connection->sa_query_count; i++) {
		if (!memcmp(connection->sa_query_ids[i], id, 2)) {
			mt6797_radio_sa_query_stop(radio);
			break;
		}
	}
}

static void mt6797_radio_sa_query_timeout(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;

	mt6797_radio_sa_query_stop(radio);
	connection->cancel = true;
	complete(&connection->pm_completion);
	connection->peer_disconnect = false; /* Local validation timed out; trigger was unauthenticated. */
	connection->reason = WLAN_REASON_PREV_AUTH_NOT_VALID;
	queue_work(system_long_wq, &radio->connection_work);
}

static void mt6797_radio_sa_query_work(struct work_struct *work)
{
	struct mt6797_wifi_radio *radio = container_of(to_delayed_work(work), struct mt6797_wifi_radio, sa_query_work);
	struct mt6797_radio_connection *connection;
	unsigned long flags;
	unsigned int i, attempt;
	u64 generation;
	ktime_t now;
	u16 candidate;
	u8 id[2];
	bool submitted;

	mutex_lock(&radio->connection_lock);
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (!connection || !connection->sa_query_active)
		goto out;
	if (!mt6797_radio_sa_query_current(radio)) {
		mt6797_radio_sa_query_stop(radio);
		goto out;
	}
	now = ktime_get();
	if (!ktime_before(now, connection->sa_query_deadline)) {
		mt6797_radio_sa_query_timeout(radio);
		goto out;
	}
	if (ktime_before(now, connection->sa_query_next) ||
	    connection->sa_query_attempts == ARRAY_SIZE(connection->sa_query_ids)) {
		mt6797_radio_sa_query_schedule(radio);
		goto out;
	}
	/* Random IDs are unique within this bounded query even if entropy collides.
	 * Register before submission so an early authenticated response can match.
	 */
	candidate = get_random_u32();
	for (attempt = 0; attempt <= connection->sa_query_count; attempt++, candidate++) {
		for (i = 0; i < connection->sa_query_count; i++)
			if (get_unaligned_le16(connection->sa_query_ids[i]) == candidate)
				break;
		if (i == connection->sa_query_count)
			break;
	}
	put_unaligned_le16(candidate, id);
	memcpy(connection->sa_query_ids[connection->sa_query_count++], id, 2);
	connection->sa_query_attempts++;
	connection->sa_query_next = ktime_add_us(now, 201 * 1024);
	generation = connection->sa_query_generation;
	spin_unlock_irqrestore(&radio->lock, flags);
	mt6797_radio_sa_query_tx(radio, WLAN_ACTION_SA_QUERY_REQUEST, id, generation, &submitted);
	spin_lock_irqsave(&radio->lock, flags);
	if (connection->sa_query_active && connection->sa_query_generation == generation) {
		/* A preflight failure never creates a matchable transaction ID. */
		if (!submitted) {
			connection->sa_query_count--;
			memzero_explicit(connection->sa_query_ids[connection->sa_query_count], 2);
		}
		if (!mt6797_radio_sa_query_current(radio))
			mt6797_radio_sa_query_stop(radio);
		else if (!ktime_before(ktime_get(), connection->sa_query_deadline))
			mt6797_radio_sa_query_timeout(radio);
		else
			mt6797_radio_sa_query_schedule(radio);
	}
out:
	spin_unlock_irqrestore(&radio->lock, flags);
	mutex_unlock(&radio->connection_lock);
}

/* True means PMF policy handled or rejected this frame: never fall through
 * to the legacy unauthenticated disconnect path after PMF is negotiated.
 */
static bool mt6797_radio_pmf_frame(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_rx_view *view)
{
	struct mt6797_radio_connection *connection;
	struct mt6797_wifi_mgmt_rx management;
	struct mt6797_wifi_ais_sta_pending owner;
	struct mt6797_wifi_ais_runtime *runtime;
	struct mt6797_radio_key *key;
	unsigned long flags;
	u64 *last;
	bool handled = false;
	int ret;

	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (!connection || !connection->pmf_negotiated)
		goto out;
	handled = true;
	if (!connection->connected || connection->cancel || radio->stopping)
		goto out;
	ret = mt6797_wifi_mgmt_rx_decode(view->packet, view->packet_length, &management);
	if (ret) {
		if (!mt6797_wifi_mgmt_rx_sa_query_trigger(view->packet, view->packet_length, &management))
			mt6797_radio_sa_query_start(radio, &management);
		goto out;
	}
	runtime = &connection->ais.runtime;
	if (!ether_addr_equal(management.frame + 10, connection->bss->bssid) ||
	    !ether_addr_equal(management.frame + 16, connection->bss->bssid) ||
	    management.bss_index != runtime->bss.bss_index ||
	    (management.multicast ? !is_broadcast_ether_addr(management.frame + 4) :
	     !ether_addr_equal(management.frame + 4, radio->netdev->dev_addr)))
		goto out;
	if (management.multicast) {
		/* The complete MMIE ID chooses the independent host replay space. */
		if (management.key_index < 4 || management.key_index > 5 || connection->igtk_updating)
			goto out;
		key = &connection->igtk[management.key_index - 4];
		if (management.wlan_index != runtime->sta.ap_bmc_group.index ||
		    mt6797_radio_igtk_owner(radio, &owner))
			goto out;
		last = &key->rx_pn[0];
	} else {
		key = &connection->pairwise_key;
		if (!connection->pairwise_ready || management.wlan_index != runtime->sta.pairwise.index ||
		    mt6797_wifi_ais_sta_pending_init(&runtime->bss, &runtime->sta, 0, &owner) ||
		    !mt6797_radio_sta_owner_same(&connection->key_pending, &owner) ||
		    !mt6797_radio_wtbl_same(&connection->pairwise_wtbl, &runtime->sta.pairwise))
			goto out;
		last = &key->mgmt_rx_pn;
	}
	if (!key->installed || key->updating || key->index != management.key_index ||
	    management.pn <= *last)
		goto out;
	/* Software BIP validation never trusts RXD cipher mode alone for MIC
	 * success. CCMP relies on descriptor integrity status and a preserved group1 PN.
	 * Neither path advances replay state on a failed ownership/integrity check.
	 */
	if (management.multicast && mt6797_wifi_mgmt_rx_bip_verify(&management, key->material))
		goto out;
	if (mt6797_wifi_data_rx_replay(last, management.pn))
		goto out;
	if (management.subtype == IEEE80211_STYPE_DEAUTH ||
	    management.subtype == IEEE80211_STYPE_DISASSOC) {
		connection->cancel = true;
		mt6797_radio_sa_query_stop(radio);
		complete(&connection->pm_completion);
		connection->peer_disconnect = true;
		connection->reason = get_unaligned_le16(management.frame + 24);
		queue_work(system_long_wq, &radio->connection_work);
	}
	if (management.subtype == IEEE80211_STYPE_ACTION &&
	    management.frame[24] == WLAN_CATEGORY_SA_QUERY &&
	    management.frame[25] == WLAN_ACTION_SA_QUERY_REQUEST &&
	    !connection->sa_query_response_pending) {
		memcpy(connection->sa_query_id, management.frame + 26, 2);
		connection->sa_query_response_pending = true;
		queue_work(system_long_wq, &radio->management_work);
	}
	if (management.subtype == IEEE80211_STYPE_ACTION &&
	    management.frame[24] == WLAN_CATEGORY_SA_QUERY &&
	    management.frame[25] == WLAN_ACTION_SA_QUERY_RESPONSE)
		mt6797_radio_sa_query_match(radio, management.frame + 26);
	/* Delivery of other robust action categories remains separate. */
out:
	spin_unlock_irqrestore(&radio->lock, flags);
	return handled;
}

static int mt6797_radio_igtk_connection(struct mt6797_wifi_radio *radio,
	const u8 *mac, struct mt6797_wifi_ais_sta_pending *owner)
{
	int ret = mt6797_radio_key_connection(radio, false, mac);

	if (ret)
		return ret;
	/* This slice must remain unreachable for the current non-PMF joins. */
	if (!radio->connection->pmf_negotiated)
		return -EOPNOTSUPP;
	return mt6797_radio_igtk_owner(radio, owner);
}

static void mt6797_radio_igtk_forget(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	memzero_explicit(connection->igtk, sizeof(connection->igtk));
	memzero_explicit(connection->igtk_history, sizeof(connection->igtk_history));
	connection->igtk_history_count = 0;
	connection->igtk_updating = false;
	memzero_explicit(&connection->igtk_owner, sizeof(connection->igtk_owner));
	memzero_explicit(&connection->igtk_wtbl, sizeof(connection->igtk_wtbl));
	connection->igtk_default = 0;
	if (connection->pmf_negotiated) {
		connection->authorized = false;
		connection->authorize_pending = false;
		mt6797_radio_sa_query_stop(radio);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
}

static int mt6797_radio_igtk_send(struct mt6797_wifi_radio *radio,
	const struct mt6797_wifi_ais_key_params *params, bool teardown)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_runtime *runtime = &connection->ais.runtime;
	u8 command[MT6797_WIFI_AIS_KEY_COMMAND_LENGTH] = {};
	int ret;

	mutex_lock(&radio->irq->lifecycle_lock);
	if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME ||
	    (!teardown && mt6797_radio_connection_cancelled(radio))) {
		ret = -ENOTCONN;
		goto out;
	}
	if (++radio->command_sequence > 255)
		radio->command_sequence = 3;
	ret = mt6797_wifi_ais_bip_key_build(&runtime->bss, &runtime->sta,
		params, radio->command_sequence, command, sizeof(command));
	if (ret)
		goto out;
	ret = mt6797_wifi_hif_runtime_send(radio->irq, MT6797_WIFI_MCR_WTDR1,
		command, sizeof(command));
	memzero_explicit(command, sizeof(command));
	/* There is no IGTK-specific firmware completion event. HIF acceptance
	 * orders this command; it does not prove BIP cryptography or replay checks.
	 */
	if (!ret && !teardown && mt6797_radio_connection_cancelled(radio))
		ret = -ECANCELED;
	if (ret)
		mt6797_wifi_hif_runtime_fail(radio->irq, ret);
out:
	memzero_explicit(command, sizeof(command));
	mutex_unlock(&radio->irq->lifecycle_lock);
	return ret;
}

static int mt6797_radio_add_igtk(struct mt6797_wifi_radio *radio,
	struct wireless_dev *wdev, int link_id, u8 index, bool pairwise,
	const u8 *mac, struct key_params *params)
{
	struct mt6797_radio_connection *connection;
	struct mt6797_wifi_ais_sta_pending owner;
	struct mt6797_wifi_ais_key_params request = { .index = index };
	struct mt6797_radio_key *key;
	unsigned long flags;
	unsigned int i;
	int ret;

	if (wdev != &radio->wdev || link_id != -1 || (index != 4 && index != 5) ||
	    pairwise || params->mode != NL80211_KEY_RX_TX || params->vlan_id ||
	    !params->key || params->key_len != MT6797_WIFI_AIS_CCMP_KEY_LENGTH ||
	    (params->seq_len != 0 && params->seq_len != 6) ||
	    (params->seq_len && !params->seq))
		return -EINVAL;
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_igtk_connection(radio, mac, &owner);
	if (ret)
		goto out;
	connection = radio->connection;
	key = &connection->igtk[index - 4];
	/* Duplicate handshakes are host no-ops, even if their supplied IPN
	 * differs. Do not reset either the firmware or host replay state.
	 */
	if (key->installed && !memcmp(key->material, params->key, sizeof(key->material))) {
		ret = 0;
		goto out;
	}
	/* This includes other-ID aliases, same-ID A->B->A replacement, and
	 * reinstall after deletion. Keep history until association teardown.
	 */
	for (i = 0; i < connection->igtk_history_count; i++) {
		if (!memcmp(connection->igtk_history[i], params->key, sizeof(key->material))) {
			ret = -EKEYREJECTED;
			goto out;
		}
	}
	if (connection->igtk_history_count == ARRAY_SIZE(connection->igtk_history)) {
		ret = -ENOSPC;
		goto out;
	}
	request.material = params->key;
	request.material_length = params->key_len;
	if (params->seq_len)
		memcpy(request.rsc, params->seq, params->seq_len);
	spin_lock_irqsave(&radio->lock, flags);
	connection->igtk_updating = true;
	spin_unlock_irqrestore(&radio->lock, flags);
	ret = mt6797_radio_igtk_send(radio, &request, false);
	if (ret) {
		/* An uncertain transport is fenced before host ownership is erased. */
		if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME ||
		    mt6797_radio_connection_cancelled(radio))
			mt6797_radio_igtk_forget(radio);
		else {
			spin_lock_irqsave(&radio->lock, flags);
			connection->igtk_updating = false;
			spin_unlock_irqrestore(&radio->lock, flags);
		}
		goto out;
	}
	spin_lock_irqsave(&radio->lock, flags);
	memzero_explicit(key, sizeof(*key));
	memcpy(key->material, params->key, sizeof(key->material));
	if (params->seq_len)
		memcpy(key->receive_sequence, params->seq, params->seq_len);
	key->rx_pn[0] = mt6797_wifi_data_rx_pn(key->receive_sequence);
	key->index = index;
	key->installed = true;
	memcpy(connection->igtk_history[connection->igtk_history_count++], params->key,
		sizeof(key->material));
	connection->igtk_owner = owner;
	connection->igtk_wtbl = connection->ais.runtime.sta.ap_bmc_group;
	connection->igtk_updating = false;
	mt6797_radio_authorize_pending(radio);
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	mutex_unlock(&radio->connection_lock);
	return ret;
}

static int mt6797_radio_del_igtk(struct mt6797_wifi_radio *radio,
	const u8 *mac, u8 index)
{
	struct mt6797_radio_connection *connection;
	struct mt6797_wifi_ais_sta_pending owner;
	struct mt6797_wifi_ais_key_params request = { .index = index, .remove = true };
	unsigned long flags;
	int ret;

	if (index != 4 && index != 5)
		return -EINVAL;
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_igtk_connection(radio, mac, &owner);
	if (ret)
		goto out;
	connection = radio->connection;
	if (!connection->igtk[index - 4].installed) {
		ret = -ENOENT;
		goto out;
	}
	spin_lock_irqsave(&radio->lock, flags);
	connection->igtk_updating = true;
	spin_unlock_irqrestore(&radio->lock, flags);
	ret = mt6797_radio_igtk_send(radio, &request, false);
	if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME ||
	    mt6797_radio_connection_cancelled(radio)) {
		mt6797_radio_igtk_forget(radio);
	} else {
		spin_lock_irqsave(&radio->lock, flags);
		connection->igtk_updating = false;
		if (!ret) {
			/* Retire only this ID. History and the other key's IPN survive. */
			memzero_explicit(&connection->igtk[index - 4], sizeof(connection->igtk[0]));
			if (connection->igtk_default == index)
				connection->igtk_default = 0;
			if (!mt6797_radio_igtk_ready(radio)) {
				connection->authorized = false;
				connection->authorize_pending = false;
				mt6797_radio_sa_query_stop(radio);
			}
		}
		spin_unlock_irqrestore(&radio->lock, flags);
	}
out:
	mutex_unlock(&radio->connection_lock);
	return ret;
}

static int mt6797_radio_set_default_mgmt_key(struct wiphy *wiphy,
	struct wireless_dev *wdev, int link_id, u8 index)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_wifi_ais_sta_pending owner;
	unsigned long flags;
	int ret;

	if (wdev != &radio->wdev || link_id != -1 || (index != 4 && index != 5))
		return -EINVAL;
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_igtk_connection(radio, NULL, &owner);
	if (!ret && !radio->connection->igtk[index - 4].installed)
		ret = -ENOENT;
	if (!ret) {
		/* A station receives BIP; its robust unicast TX uses the PTK.
		 * There is no separate GEN3 default-management-key command to send.
		 */
		spin_lock_irqsave(&radio->lock, flags);
		radio->connection->igtk_default = index;
		spin_unlock_irqrestore(&radio->lock, flags);
	}
	mutex_unlock(&radio->connection_lock);
	return ret;
}

/* Called after RX sink detachment and before AIS releases its WTBL handles. */
static int mt6797_radio_igtk_cleanup(struct mt6797_wifi_radio *radio)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_ais_sta_pending owner;
	struct mt6797_wifi_ais_key_params request = { .remove = true };
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(connection->igtk); i++) {
		if (!connection->igtk[i].installed)
			continue;
		request.index = i + 4;
		ret = mt6797_radio_igtk_owner(radio, &owner);
		if (!ret)
			ret = mt6797_radio_igtk_send(radio, &request, true);
		if (ret) {
			/* Never release uncertain firmware key ownership for reuse. */
			mutex_lock(&radio->irq->lifecycle_lock);
			mt6797_wifi_hif_runtime_fail(radio->irq, ret);
			mutex_unlock(&radio->irq->lifecycle_lock);
			break;
		}
	}
	mt6797_radio_igtk_forget(radio);
	return ret;
}

static int mt6797_radio_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
	int link_id, u8 index, bool pairwise, const u8 *mac, struct key_params *params)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_key *key;
	struct mt6797_wifi_ais_key_params request = {
		.index = index, .pairwise = pairwise,
	};
	unsigned long flags;
	unsigned int tid;
	int ret;

	if (params && params->cipher == WLAN_CIPHER_SUITE_AES_CMAC)
		return mt6797_radio_add_igtk(radio, wdev, link_id, index, pairwise, mac, params);
	if (wdev != &radio->wdev || link_id != -1 || index > 3 || (pairwise && index) ||
	    !params || params->cipher != WLAN_CIPHER_SUITE_CCMP ||
	    params->mode != NL80211_KEY_RX_TX || params->vlan_id || !params->key ||
	    params->key_len != MT6797_WIFI_AIS_CCMP_KEY_LENGTH ||
	    (params->seq_len != 0 && params->seq_len != 6) ||
	    (params->seq_len && !params->seq))
		return -EINVAL;
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_key_connection(radio, pairwise, mac);
	if (ret)
		goto out;
	key = pairwise ? &radio->connection->pairwise_key : &radio->connection->group_key;
	/* A handshake retransmission must not reset firmware packet counters. */
	if (key->installed && !memcmp(key->material, params->key, sizeof(key->material))) {
		ret = key->index == index ? 0 : -EOPNOTSUPP;
		goto out;
	}
	/* Firmware readiness cannot distinguish a delayed PTK event from a rekey. */
	if (pairwise && radio->connection->pairwise_attempted) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	request.material = params->key;
	request.material_length = params->key_len;
	if (params->seq_len)
		memcpy(request.rsc, params->seq, params->seq_len);
	spin_lock_irqsave(&radio->lock, flags);
	key->updating = true;
	spin_unlock_irqrestore(&radio->lock, flags);
	ret = mt6797_radio_key_send(radio, &request);
	spin_lock_irqsave(&radio->lock, flags);
	key->updating = false;
	if (!ret) {
		memzero_explicit(key, sizeof(*key));
		memcpy(key->material, params->key, sizeof(key->material));
		if (params->seq_len)
			memcpy(key->receive_sequence, params->seq, params->seq_len);
		for (tid = 0; tid < ARRAY_SIZE(key->rx_pn); tid++)
			key->rx_pn[tid] = mt6797_wifi_data_rx_pn(key->receive_sequence);
		key->mgmt_rx_pn = mt6797_wifi_data_rx_pn(key->receive_sequence);
		key->index = index;
		key->installed = true;
		mt6797_radio_authorize_pending(radio);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	mutex_unlock(&radio->connection_lock);
	return ret;
}

static int mt6797_radio_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
	int link_id, u8 index, bool pairwise, const u8 *mac)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_key *key;
	struct mt6797_wifi_ais_key_params request = {
		.index = index, .pairwise = pairwise, .remove = true,
	};
	unsigned long flags;
	int ret;

	if (wdev != &radio->wdev || link_id != -1 || index > 5 || (pairwise && index))
		return -EINVAL;
	if (index >= 4)
		return mt6797_radio_del_igtk(radio, mac, index);
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_key_connection(radio, pairwise, mac);
	if (ret)
		goto out;
	key = pairwise ? &radio->connection->pairwise_key : &radio->connection->group_key;
	if (!key->installed || key->index != index) {
		ret = -ENOENT; /* An old GTK ID must not delete the replacement's slot. */
		goto out;
	}
	spin_lock_irqsave(&radio->lock, flags);
	key->updating = true;
	spin_unlock_irqrestore(&radio->lock, flags);
	ret = mt6797_radio_key_send(radio, &request);
	spin_lock_irqsave(&radio->lock, flags);
	key->updating = false;
	if (!ret) {
		memzero_explicit(key, sizeof(*key));
		radio->connection->authorized = false;
		radio->connection->authorize_pending = false;
		mt6797_radio_sa_query_stop(radio);
		if (pairwise) {
			radio->connection->pairwise_ready = false;
			memzero_explicit(&radio->connection->pairwise_wtbl, sizeof(radio->connection->pairwise_wtbl));
		}
	}
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	mutex_unlock(&radio->connection_lock);
	return ret;
}

static int mt6797_radio_set_default_key(struct wiphy *wiphy, struct net_device *netdev,
	int link_id, u8 index, bool unicast, bool multicast)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	int ret;

	/* Infrastructure CCMP TX always uses the AP's pairwise WTBL entry. */
	if (netdev != radio->netdev || link_id != -1 || unicast || !multicast || index > 3)
		return -EOPNOTSUPP;
	mutex_lock(&radio->connection_lock);
	ret = mt6797_radio_key_connection(radio, false, NULL);
	if (!ret && (!radio->connection->group_key.installed ||
	    radio->connection->group_key.index != index))
		ret = -ENOENT;
	mutex_unlock(&radio->connection_lock);
	return ret;
}

static int mt6797_radio_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
	const u8 *mac, struct station_parameters *params)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_connection *connection;
	unsigned long flags;
	bool authorize;
	int ret;

	if (wdev != &radio->wdev || !mac || !params)
		return -EINVAL;
	ret = cfg80211_check_station_change(wiphy, params, CFG80211_STA_AP_STA);
	if (ret)
		return ret;
	mutex_lock(&radio->connection_lock);
	connection = radio->connection;
	if (mt6797_radio_connection_cancelled(radio) || !connection->connected) {
		ret = -ENOTCONN;
		goto out;
	}
	if (!ether_addr_equal(mac, connection->bss->bssid)) {
		ret = -ENOENT;
		goto out;
	}
	if (!(params->sta_flags_mask & BIT(NL80211_STA_FLAG_AUTHORIZED)))
		goto out;
	authorize = params->sta_flags_set & BIT(NL80211_STA_FLAG_AUTHORIZED);
	spin_lock_irqsave(&radio->lock, flags);
	if (connection->cancel || radio->stopping)
		ret = -ENOTCONN;
	else if (!authorize) {
		connection->authorized = false;
		connection->authorize_pending = false;
		mt6797_radio_sa_query_stop(radio);
	} else if (mt6797_radio_keys_ready(radio)) {
		connection->authorized = true;
	} else if (connection->pairwise_key.installed) {
		/* The group keys follow the PTK; data waits for them. */
		connection->authorize_pending = true;
	} else {
		ret = -ENOKEY;
	}
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	mutex_unlock(&radio->connection_lock);
	return ret;
}

/* Only SAE authentication frames for the current AIS join are accepted. */
static int mt6797_radio_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
	struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_connection *connection;
	unsigned long flags;
	int ret;

	if (wdev != &radio->wdev || !params || !cookie || !params->buf ||
	    params->offchan || params->wait || params->n_csa_offsets ||
	    params->link_id >= 0)
		return -EOPNOTSUPP;
	if (params->len < MT6797_WIFI_AIS_AUTH_FRAME_LENGTH ||
	    params->len > MT6797_WIFI_AIS_SAE_FRAME_MAX)
		return -EINVAL;
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (!connection || !connection->sae_active || connection->sae_done ||
	    connection->cancel || radio->stopping) {
		ret = -ENOTCONN;
	} else if ((params->chan && params->chan != connection->bss->channel) ||
		   mt6797_wifi_ais_sae_auth_tx_check(&connection->ais.auth.pending,
			params->buf, params->len)) {
		ret = -EINVAL;
	} else if (connection->sae_tx_pending) {
		ret = -EBUSY;
	} else {
		memcpy(connection->sae_frame, params->buf, params->len);
		connection->sae_frame_length = params->len;
		connection->sae_cookie = *cookie = ++radio->mgmt_cookie;
		connection->sae_tx_ack = !params->dont_wait_for_ack;
		connection->sae_tx_pending = true;
		mt6797_wifi_ais_hif_ext_auth_kick(&connection->ais);
		ret = 0;
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return ret;
}

static int mt6797_radio_external_auth(struct wiphy *wiphy, struct net_device *netdev,
	struct cfg80211_external_auth_params *params)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(wiphy);
	struct mt6797_radio_connection *connection;
	unsigned long flags;
	int ret = 0;

	if (netdev != radio->netdev || !params)
		return -EINVAL;
	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (!connection || !connection->sae_active || connection->sae_done ||
	    connection->cancel || radio->stopping) {
		ret = -ENOTCONN;
	} else if (!ether_addr_equal(params->bssid, connection->bss->bssid) ||
		   (params->ssid.ssid_len &&
		    (params->ssid.ssid_len != connection->join.negotiated.ssid_length ||
		     memcmp(params->ssid.ssid, connection->join.negotiated.ssid,
			    params->ssid.ssid_len)))) {
		ret = -EINVAL;
	} else {
		connection->sae_status = params->status;
		if (params->status == WLAN_STATUS_SUCCESS && params->pmkid) {
			memcpy(connection->sae_pmkid, params->pmkid, sizeof(connection->sae_pmkid));
			connection->sae_pmkid_set = true;
		}
		connection->sae_done = true;
		mt6797_wifi_ais_hif_ext_auth_kick(&connection->ais);
	}
	spin_unlock_irqrestore(&radio->lock, flags);
	return ret;
}

static const struct cfg80211_ops mt6797_radio_ops = {
	.scan = mt6797_radio_scan,
	.abort_scan = mt6797_radio_abort_scan,
	.connect = mt6797_radio_connect,
	.disconnect = mt6797_radio_disconnect,
	.add_key = mt6797_radio_add_key,
	.del_key = mt6797_radio_del_key,
	.set_default_key = mt6797_radio_set_default_key,
	.set_default_mgmt_key = mt6797_radio_set_default_mgmt_key,
	.change_station = mt6797_radio_change_station,
	.mgmt_tx = mt6797_radio_mgmt_tx,
	.external_auth = mt6797_radio_external_auth,
};

static const struct ieee80211_txrx_stypes
mt6797_radio_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = BIT(IEEE80211_STYPE_AUTH >> 4),
		.rx = BIT(IEEE80211_STYPE_AUTH >> 4),
	},
};

static int mt6797_radio_open(struct net_device *netdev)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(netdev->ieee80211_ptr->wiphy);

	if (mt6797_wifi_hif_irq_mode(radio->irq) != MT6797_WIFI_HIF_IRQ_RUNTIME)
		return -EIO;
	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
	return 0;
}

static int mt6797_radio_stop(struct net_device *netdev)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(netdev->ieee80211_ptr->wiphy);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	mt6797_radio_abort(radio, -ECANCELED);
	mt6797_radio_disconnect(radio->wdev.wiphy, netdev, WLAN_REASON_DEAUTH_LEAVING);
	flush_work(&radio->scan_work);
	flush_work(&radio->connection_work);
	flush_work(&radio->tx_work);
	flush_work(&radio->management_work);
	cancel_delayed_work_sync(&radio->sa_query_work);
	mt6797_radio_purge_tx(radio);
	return 0;
}

static void mt6797_radio_tx_credit_wait(struct mt6797_wifi_radio *radio, bool first)
{
	unsigned long flags, remaining;
	u64 started;

	spin_lock_irqsave(&radio->lock, flags);
	started = ktime_get_ns();
	radio->tx_diag.credit_wait_attempts++;
	if (first)
		radio->tx_diag.credit_wait_packets++;
	radio->tx_diag.credit_wait_active = true;
	radio->tx_diag.credit_wait_started_ns = started;
	spin_unlock_irqrestore(&radio->lock, flags);
	/* A wakeup reports credit availability, not packet/airtime completion. */
	remaining = wait_for_completion_timeout(&radio->irq->tx_credit_completion,
						msecs_to_jiffies(50));
	spin_lock_irqsave(&radio->lock, flags);
	mt6797_radio_tx_diag_add(&radio->tx_diag.credit_wait, ktime_get_ns() - started);
	radio->tx_diag.credit_wait_active = false;
	if (remaining)
		radio->tx_diag.credit_wait_wakeups++;
	else
		radio->tx_diag.credit_wait_timeouts++;
	spin_unlock_irqrestore(&radio->lock, flags);
}

static int mt6797_radio_tx_packet(struct mt6797_wifi_radio *radio, struct sk_buff *skb)
{
	struct mt6797_radio_connection *connection = radio->connection;
	struct mt6797_wifi_tx_params params = {};
	unsigned long flags;
	ktime_t deadline;
	size_t length;
	u16 ether_type;
	u8 *packet;
	bool secure, waited = false;
	int ret;

	if (skb_linearize(skb))
		return -ENOMEM;
	if (skb->len < ETH_HLEN || skb->len > radio->netdev->mtu + ETH_HLEN)
		return -EMSGSIZE;
	ether_type = get_unaligned_be16(skb->data + 12);
	if (ether_type < ETH_P_802_3_MIN || ether_type == ETH_P_8021Q || ether_type == ETH_P_8021AD)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&radio->lock, flags);
	if (!connection || !connection->connected || connection->cancel || radio->stopping ||
	    !ether_addr_equal(skb->data + ETH_ALEN, radio->netdev->dev_addr)) {
		ret = -ENOTCONN;
		goto unlock;
	}
	secure = connection->join.negotiated.security != MT6797_WIFI_AIS_SECURITY_OPEN;
	if (ether_type == ETH_P_PAE) {
		unsigned int message = mt6797_radio_eapol_pairwise_message(
			skb->data + ETH_HLEN, skb->len - ETH_HLEN);
		bool plaintext_reply = message == 4 && connection->pairwise_reply_plaintext;

		if (!ether_addr_equal(skb->data, connection->bss->bssid)) {
			ret = -EINVAL;
			goto unlock;
		}
		params.traffic = MT6797_WIFI_TX_EAPOL;
		params.protected = secure && !connection->control_port_no_encrypt && !plaintext_reply &&
			connection->pairwise_key.installed && connection->pairwise_ready;
		if (message == 4)
			dev_info_ratelimited(wiphy_dev(radio->wdev.wiphy),
				"EAPOL pairwise reply protected=%u no_encrypt=%u plaintext_m3=%u\n",
				params.protected, connection->control_port_no_encrypt,
				connection->pairwise_reply_plaintext);
	} else {
		if (!connection->authorized || (secure &&
		    (!connection->pairwise_key.installed || !connection->pairwise_ready))) {
			ret = -EACCES;
			goto unlock;
		}
		params.traffic = ether_type == ETH_P_ARP ? MT6797_WIFI_TX_ARP : MT6797_WIFI_TX_DATA;
		params.protected = secure;
	}
	/* Even multicast Ethernet travels to this AP over the pairwise link. */
	params.wlan_index = connection->ais.runtime.sta.pairwise.index;
	params.own_mac_index = connection->ais.runtime.bss.own_mac_index;
	if (params.traffic != MT6797_WIFI_TX_DATA)
		params.fixed_rate = connection->join.negotiated.default_rate;
	ret = 0;
unlock:
	spin_unlock_irqrestore(&radio->lock, flags);
	if (ret)
		return ret;
	packet = kmalloc(MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	ret = mt6797_wifi_tx_build(packet, MT6797_WIFI_RUNTIME_MAX_PACKET_LENGTH,
		skb->data, skb->len, &params, &length);
	if (ret)
		goto free_packet;
	deadline = ktime_add_ms(ktime_get(), 1000);
	mutex_lock(&radio->irq->lifecycle_lock);
	for (;;) {
		if (mt6797_radio_connection_cancelled(radio)) {
			ret = -ECANCELED;
			break;
		}
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			ret = -ETIMEDOUT;
			mt6797_wifi_hif_runtime_fail(radio->irq, ret);
			break;
		}
		ret = mt6797_wifi_hif_runtime_send(radio->irq, MT6797_WIFI_MCR_WTDR1, packet, length);
		if (ret != -EAGAIN)
			break;
		/* Short bounded waits also notice disconnect/terminal without an IRQ. */
		mt6797_radio_tx_credit_wait(radio, !waited);
		waited = true;
	}
	mutex_unlock(&radio->irq->lifecycle_lock);
free_packet:
	kfree_sensitive(packet); /* May contain EAPOL handshake material. */
	return ret;
}

static void mt6797_radio_tx_work(struct work_struct *work)
{
	struct mt6797_wifi_radio *radio = container_of(work, struct mt6797_wifi_radio, tx_work);
	struct sk_buff *skb;
	unsigned long flags;
	u64 now;
	int ret;

	spin_lock_irqsave(&radio->lock, flags);
	radio->tx_diag.worker_active = true;
	radio->tx_diag.worker_started_ns = ktime_get_ns();
	spin_unlock_irqrestore(&radio->lock, flags);
	for (;;) {
		/* Drop the mutex between packets so keys/disconnect are not starved. */
		mutex_lock(&radio->connection_lock);
		spin_lock_irqsave(&radio->lock, flags);
		skb = skb_dequeue(&radio->tx_queue);
		if (skb) {
			const struct mt6797_radio_tx_cb *stamp = (const void *)skb->cb;

			now = ktime_get_ns();
			radio->tx_diag.dequeued++;
			radio->tx_diag.queue_depth--;
			mt6797_radio_tx_diag_add(&radio->tx_diag.queue_wait, now - stamp->enqueued_ns);
			radio->tx_diag.send_active = true;
			radio->tx_diag.send_started_ns = now;
		}
		spin_unlock_irqrestore(&radio->lock, flags);
		if (!skb) {
			mutex_unlock(&radio->connection_lock);
			break;
		}
		ret = mt6797_radio_tx_packet(radio, skb);
		if (ret) {
			DEV_STATS_INC(radio->netdev, tx_dropped);
		} else {
			DEV_STATS_INC(radio->netdev, tx_packets);
			DEV_STATS_ADD(radio->netdev, tx_bytes, skb->len);
		}
		dev_kfree_skb_any(skb);
		spin_lock_irqsave(&radio->lock, flags);
		mt6797_radio_tx_diag_add(&radio->tx_diag.send_duration,
			ktime_get_ns() - radio->tx_diag.send_started_ns);
		radio->tx_diag.send_active = false;
		if (ret) {
			radio->tx_diag.send_failed++;
			if (ret == -ECANCELED)
				radio->tx_diag.send_cancelled++;
			else if (ret == -ENOTCONN)
				radio->tx_diag.send_disconnected++;
			else if (ret == -ETIMEDOUT)
				radio->tx_diag.send_timed_out++;
		} else {
			/* Success here is only a HIF submission, never a TX completion. */
			radio->tx_diag.hif_submitted++;
		}
		if (!radio->stopping && radio->connection && radio->connection->connected &&
		    !radio->connection->cancel && skb_queue_len(&radio->tx_queue) < MT6797_RADIO_TX_WAKE_THRESHOLD)
			netif_wake_queue(radio->netdev);
		spin_unlock_irqrestore(&radio->lock, flags);
		mutex_unlock(&radio->connection_lock);
	}
	spin_lock_irqsave(&radio->lock, flags);
	mt6797_radio_tx_diag_add(&radio->tx_diag.worker_duration,
		ktime_get_ns() - radio->tx_diag.worker_started_ns);
	radio->tx_diag.worker_active = false;
	spin_unlock_irqrestore(&radio->lock, flags);
}

static netdev_tx_t mt6797_radio_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct mt6797_wifi_radio *radio = wiphy_priv(netdev->ieee80211_ptr->wiphy);
	struct mt6797_radio_tx_cb *stamp = (void *)skb->cb;
	unsigned long flags;

	BUILD_BUG_ON(sizeof(*stamp) > sizeof(skb->cb));
	if (!pskb_may_pull(skb, ETH_HLEN))
		goto drop;
	spin_lock_irqsave(&radio->lock, flags);
	if (radio->stopping || !radio->connection || !radio->connection->connected ||
	    radio->connection->cancel || (!radio->connection->authorized &&
	    get_unaligned_be16(skb->data + 12) != ETH_P_PAE)) {
		spin_unlock_irqrestore(&radio->lock, flags);
		goto drop;
	}
	if (skb_queue_len(&radio->tx_queue) >= MT6797_RADIO_TX_QUEUE_LIMIT) {
		radio->tx_diag.busy++;
		netif_stop_queue(netdev);
		spin_unlock_irqrestore(&radio->lock, flags);
		return NETDEV_TX_BUSY;
	}
	/* skb->cb belongs to the driver only after it accepts this packet. */
	stamp->enqueued_ns = ktime_get_ns();
	skb_queue_tail(&radio->tx_queue, skb);
	radio->tx_diag.enqueued++;
	radio->tx_diag.queue_depth++;
	if (radio->tx_diag.queue_depth > radio->tx_diag.queue_high_water)
		radio->tx_diag.queue_high_water = radio->tx_diag.queue_depth;
	if (skb_queue_len(&radio->tx_queue) >= MT6797_RADIO_TX_QUEUE_LIMIT)
		netif_stop_queue(netdev);
	queue_work(system_long_wq, &radio->tx_work);
	spin_unlock_irqrestore(&radio->lock, flags);
	return NETDEV_TX_OK;
drop:
	spin_lock_irqsave(&radio->lock, flags);
	radio->tx_diag.rejected++;
	spin_unlock_irqrestore(&radio->lock, flags);
	dev_kfree_skb_any(skb);
	DEV_STATS_INC(netdev, tx_dropped);
	return NETDEV_TX_OK;
}

static const struct net_device_ops mt6797_radio_netdev_ops = {
	.ndo_open = mt6797_radio_open,
	.ndo_stop = mt6797_radio_stop,
	.ndo_start_xmit = mt6797_radio_xmit,
	.ndo_validate_addr = eth_validate_addr,
};

struct mt6797_wifi_radio *mt6797_wifi_radio_register(struct device *dev,
	struct mt6797_wifi_hif_irq *irq,
	struct mt6797_wifi_runtime_config *config,
	const struct mt6797_wifi_capability *capability)
{
	static const int rates[] = { 10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540 };
	/* BIP-CMAC-128 is the PMF IGTK; firmware keys it as GEN3 algorithm 6. */
	static const u32 ciphers[] = { WLAN_CIPHER_SUITE_CCMP, WLAN_CIPHER_SUITE_AES_CMAC };
	static const u8 channels_5ghz[] = {
		36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
		116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
	};
	struct wiphy *wiphy = wiphy_new(&mt6797_radio_ops, sizeof(struct mt6797_wifi_radio));
	struct mt6797_wifi_radio *radio;
	unsigned int band, index;
	int ret;

	if (!wiphy)
		return ERR_PTR(-ENOMEM);
	radio = wiphy_priv(wiphy);
	radio->irq = irq;
	mutex_init(&radio->connection_lock);
	radio->config = config;
	radio->capability = *capability;
	radio->wdev.wiphy = wiphy;
	radio->wdev.iftype = NL80211_IFTYPE_STATION;
	radio->command_sequence = 2; /* Capability=1, basic configuration=2. */
	spin_lock_init(&radio->lock);
	init_completion(&radio->scan_done);
	INIT_WORK(&radio->scan_work, mt6797_radio_scan_work);
	INIT_WORK(&radio->connection_work, mt6797_radio_connection_work);
	INIT_WORK(&radio->tx_work, mt6797_radio_tx_work);
	INIT_WORK(&radio->management_work, mt6797_radio_management_work);
	INIT_DELAYED_WORK(&radio->sa_query_work, mt6797_radio_sa_query_work);
	skb_queue_head_init(&radio->tx_queue);
	radio->tx_diag.lifetime_start_ns = ktime_get_ns();
	set_wiphy_dev(wiphy, dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	/* Only the wildcard SSID; IEs are accepted and unused (passive scan). */
	wiphy->max_scan_ssids = 1;
	wiphy->max_scan_ie_len = IEEE80211_MAX_DATA_LEN;
	wiphy->cipher_suites = ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(ciphers);
	/* WPA3-Personal: user space runs SAE via NL80211_CMD_EXTERNAL_AUTH. */
	wiphy->features |= NL80211_FEATURE_SAE;
	wiphy->mgmt_stypes = mt6797_radio_mgmt_stypes;
	for (index = 0; index < ARRAY_SIZE(rates); index++) {
		radio->rates[index].bitrate = rates[index];
		radio->rates[index].hw_value = index;
	}
	for (band = 0; band < 2; band++) {
		struct ieee80211_supported_band *supported = &radio->bands[band];

		if (band && capability->hw_5ghz_disabled)
			continue;
		supported->channels = radio->channels[band];
		supported->n_channels = band ? ARRAY_SIZE(channels_5ghz) : 14;
		supported->bitrates = radio->rates + (band ? 4 : 0);
		supported->n_bitrates = ARRAY_SIZE(rates) - (band ? 4 : 0);
		for (index = 0; index < supported->n_channels; index++) {
			struct ieee80211_channel *channel = &supported->channels[index];

			channel->band = band;
			channel->hw_value = band ? channels_5ghz[index] : index + 1;
			channel->center_freq = ieee80211_channel_to_frequency(channel->hw_value, band);
			/* Receive-only until the TX power policy and data path exist. */
			channel->max_power = 0;
		}
		wiphy->bands[band] = supported;
	}
	radio->netdev = alloc_etherdev(0);
	if (!radio->netdev) {
		ret = -ENOMEM;
		goto free_wiphy;
	}
	strscpy(radio->netdev->name, "wlan%d", IFNAMSIZ);
	radio->netdev->netdev_ops = &mt6797_radio_netdev_ops;
	/* Netdev registration/removal owns publication and active sysfs readers. */
	radio->netdev->sysfs_groups[0] = &mt6797_radio_tx_diag_group;
	radio->netdev->ieee80211_ptr = &radio->wdev;
	radio->wdev.netdev = radio->netdev;
	SET_NETDEV_DEV(radio->netdev, dev);
	if (is_valid_ether_addr(capability->mac_address))
		eth_hw_addr_set(radio->netdev, capability->mac_address);
	else
		eth_hw_addr_random(radio->netdev);
	/* AIS must use the same station address as this netdev, including fallback. */
	ether_addr_copy(radio->capability.mac_address, radio->netdev->dev_addr);
	ether_addr_copy(wiphy->perm_addr, radio->netdev->dev_addr);
	netif_carrier_off(radio->netdev);
	ret = wiphy_register(wiphy);
	if (ret)
		goto free_netdev;
	ret = register_netdev(radio->netdev);
	if (ret)
		goto unregister_wiphy;
	return radio;
unregister_wiphy:
	wiphy_unregister(wiphy);
free_netdev:
	free_netdev(radio->netdev);
free_wiphy:
	wiphy_free(wiphy);
	return ERR_PTR(ret);
}

void mt6797_wifi_radio_unregister(struct mt6797_wifi_radio *radio)
{
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	radio->stopping = true;
	if (radio->connection)
		radio->connection->cancel = true;
	radio->abort_scan = true;
	complete(&radio->scan_done);
	spin_unlock_irqrestore(&radio->lock, flags);
	flush_work(&radio->scan_work);
	queue_work(system_long_wq, &radio->connection_work);
	flush_work(&radio->connection_work);
	flush_work(&radio->tx_work);
	flush_work(&radio->management_work);
	cancel_delayed_work_sync(&radio->sa_query_work);
	mt6797_radio_purge_tx(radio);
	unregister_netdev(radio->netdev);
	wiphy_unregister(radio->wdev.wiphy);
	free_netdev(radio->netdev);
	wiphy_free(radio->wdev.wiphy);
}
