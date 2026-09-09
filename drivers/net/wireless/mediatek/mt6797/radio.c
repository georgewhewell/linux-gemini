// SPDX-License-Identifier: GPL-2.0-only
/* cfg80211 station interface and asynchronous, firmware-backed passive scan. */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/if_vlan.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>
#include "ais_hif.h"
#include "ais_key.h"
#include "capability.h"
#include "data_rx.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "radio.h"
#include "rx_metadata.h"
#include "scan.h"
#include "scan_hif.h"
#include "tx_diag.h"

struct mt6797_radio_key {
	u8 material[MT6797_WIFI_AIS_CCMP_KEY_LENGTH];
	u8 receive_sequence[6];
	u64 rx_pn[16];
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
	struct mt6797_wifi_ais_sta_pending key_pending;
	struct completion key_completion;
	struct completion pm_completion;
	struct mt6797_wifi_ais_pm_params pm;
	bool beacon_seen;
	bool pm_started;
	int key_error;
	bool key_waiting;
	bool pairwise_ready;
	bool pairwise_attempted;
	bool authorized;
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
	int scan_error;
	bool armed;
	bool abort_scan;
	bool stopping;
};

#define MT6797_RADIO_TX_QUEUE_LIMIT 64U
#define MT6797_RADIO_TX_WAKE_THRESHOLD 32U

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

static int mt6797_radio_event(void *context,
			    const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_radio *radio = context;
	enum mt6797_wifi_scan_event_result disposition;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&radio->lock, flags);
	if (radio->armed) {
		ret = mt6797_wifi_scan_event(&radio->scan, event, &disposition);
		if (ret || disposition == MT6797_WIFI_SCAN_EVENT_COMPLETE) {
			radio->scan_error = ret;
			complete(&radio->scan_done);
		}
	}
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
	bool attached = false, sent, needs_fence;
	int ret;

	mutex_lock(&radio->irq->lifecycle_lock);
	ret = mt6797_wifi_hif_runtime_sink_attach(radio->irq, &sink);
	if (ret)
		goto out;
	attached = true;
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
		if (radio->abort_scan || radio->stopping) {
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
		ret = mt6797_wifi_hif_runtime_send(radio->irq,
			MT6797_WIFI_MCR_WTDR1, command, sizeof(command));
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
			if (!cancel_ret)
				mt6797_wifi_hif_runtime_send(radio->irq,
					MT6797_WIFI_MCR_WTDR1, command,
					MT6797_WIFI_SCAN_CANCEL_LENGTH);
		} else {
			spin_unlock_irqrestore(&radio->lock, flags);
		}
		if (ret) {
			/* No cancellation acknowledgement means firmware state is uncertain. */
			if (needs_fence)
				mt6797_wifi_hif_runtime_fail(radio->irq, ret);
			break;
		}
		offset += count;
	}
out:
	if (attached)
		mt6797_wifi_hif_runtime_sink_detach(radio->irq, radio);
	mutex_unlock(&radio->irq->lifecycle_lock);
	spin_lock_irqsave(&radio->lock, flags);
	info.aborted = ret || radio->abort_scan || radio->stopping;
	radio->request = NULL;
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

	/* iw adds COLOCATED_6GHZ by default; this wiphy has no 6 GHz band. */
	if (request->n_ssids || request->ie_len ||
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
	else if (radio->request || radio->connection)
		ret = -EBUSY;
	else {
		radio->request = request;
		radio->abort_scan = false;
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

static void mt6797_radio_link_abort(void *context, int reason)
{
	struct mt6797_wifi_radio *radio = context;
	unsigned long flags;

	spin_lock_irqsave(&radio->lock, flags);
	if (radio->connection) {
		radio->connection->cancel = true;
		complete(&radio->connection->pm_completion);
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
static int mt6797_radio_link_event(void *context,
	const struct mt6797_wifi_event_view *event)
{
	struct mt6797_wifi_radio *radio = context;
	struct mt6797_radio_connection *connection;
	enum mt6797_wifi_ais_event_disposition disposition;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&radio->lock, flags);
	connection = radio->connection;
	if (connection && connection->key_waiting) {
		ret = mt6797_wifi_ais_ccmp_ready_match(&connection->key_pending, event, &disposition);
		if (ret || disposition == MT6797_WIFI_AIS_EVENT_COMPLETE) {
			connection->key_waiting = false;
			connection->key_error = ret;
			connection->pairwise_ready = !ret;
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
	u16 body_length, key_data_length, info;

	if (!payload || length < 99 || (payload[0] != 1 && payload[0] != 2) ||
	    payload[1] != 3 || payload[4] != 2)
		return 0;
	body_length = get_unaligned_be16(payload + 2);
	key_data_length = get_unaligned_be16(payload + 97);
	if (body_length != 95U + key_data_length || 4U + body_length > length)
		return 0;
	info = get_unaligned_be16(payload + 5);
	/* Descriptor version2, pairwise, MIC; no reserved/error/request/SMK bits. */
	if ((info & 0x010f) != 0x010a || (info & 0xec30))
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

static int mt6797_radio_link_frame(void *context,
	const struct mt6797_wifi_rx_view *view)
{
	struct mt6797_wifi_radio *radio = context;
	struct mt6797_radio_connection *connection;
	const u8 *frame = view->payload;
	unsigned long flags;
	u16 control;

	if (view->kind == MT6797_WIFI_RX_ETHERNET ||
	    view->kind == MT6797_WIFI_RX_NATIVE_DATA)
		return mt6797_radio_data_frame(radio, view);
	if (view->kind != MT6797_WIFI_RX_IEEE80211 || view->header_translated ||
	    view->header_length != 24 || !frame || view->payload_length < 26)
		return 0;
	control = get_unaligned_le16(frame);
	if ((control & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) == IEEE80211_STYPE_BEACON)
		return mt6797_radio_beacon(radio, view);
	/* This radio does not negotiate protected management or reassemble fragments. */
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
	auth.response_timeout_ms = min_t(s64, 1000, auth_remaining);
	auth.sequence_number = 1;
	auth.fixed_rate = connection->join.negotiated.default_rate;
	ret = mt6797_wifi_ais_hif_open_auth(&connection->ais, &auth, &auth_result);
	if (ret || mt6797_radio_connection_cancelled(radio))
		goto cleanup;
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
	netif_stop_queue(radio->netdev);
	netif_carrier_off(radio->netdev);
	spin_unlock_irqrestore(&radio->lock, flags);
	mt6797_radio_purge_tx(radio);
	if (connection->link_attached) {
		mutex_lock(&radio->irq->lifecycle_lock);
		mt6797_wifi_hif_runtime_sink_detach(radio->irq, radio);
		mutex_unlock(&radio->irq->lifecycle_lock);
	}
	if (connection->ais.initialized) {
		if (connection->pm_started)
			mt6797_radio_pm_send(radio, false);
		radio->management_pid = connection->ais.auth.pid_owner.next_pid;
		if (connection->ais.runtime.state == MT6797_WIFI_AIS_RUNTIME_STA_STATE1 ||
		    connection->ais.runtime.state == MT6797_WIFI_AIS_RUNTIME_STA_STATE3)
			mt6797_wifi_ais_hif_stop(&connection->ais);
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
	unsigned long flags;
	size_t offset, rates_length;
	int ret = -EOPNOTSUPP;
	bool secure = sme->privacy;
	bool have_rsn = false;

	if (!sme->ssid || !sme->ssid_len || sme->ssid_len > 32 || sme->ie_len > 1024 ||
	    (sme->ie_len && !sme->ie) ||
	    (sme->crypto.control_port_ethertype && sme->crypto.control_port_ethertype != htons(ETH_P_PAE)) ||
	    sme->key_len || sme->pbss || sme->prev_bssid || sme->crypto.control_port_over_nl80211 ||
	    sme->mfp != NL80211_MFP_NO || sme->want_1x ||
	    (sme->flags & ~(ASSOC_REQ_DISABLE_HT | ASSOC_REQ_DISABLE_VHT |
		ASSOC_REQ_DISABLE_HE | ASSOC_REQ_DISABLE_EHT | ASSOC_REQ_DISABLE_UHR)) ||
	    (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM && sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC))
		return -EOPNOTSUPP;
	if (secure) {
		if (sme->crypto.wpa_versions != NL80211_WPA_VERSION_2 ||
		    sme->crypto.n_ciphers_pairwise != 1 || sme->crypto.ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP ||
		    sme->crypto.cipher_group != WLAN_CIPHER_SUITE_CCMP ||
		    sme->crypto.n_akm_suites != 1 || sme->crypto.akm_suites[0] != WLAN_AKM_SUITE_PSK)
			return -EOPNOTSUPP;
	} else if (sme->crypto.wpa_versions || sme->crypto.n_ciphers_pairwise ||
		   sme->crypto.cipher_group || sme->crypto.n_akm_suites) {
		return -EOPNOTSUPP;
	}
	/* User IEs cannot override SSID/rates or advertise unsupported HT/VHT/WMM. */
	for (offset = 0; offset < sme->ie_len; offset += sme->ie[offset + 1] + 2) {
		if (sme->ie_len - offset < 2 || sme->ie[offset + 1] > sme->ie_len - offset - 2)
			return -EINVAL;
		if (sme->ie[offset] != WLAN_EID_RSN && sme->ie[offset] != WLAN_EID_EXT_CAPABILITY)
			return -EOPNOTSUPP;
		if (sme->ie[offset] == WLAN_EID_RSN) {
			if (have_rsn)
				return -EINVAL;
			have_rsn = true;
		}
	}
	rsn = cfg80211_find_ie(WLAN_EID_RSN, sme->ie, sme->ie_len);
	if (secure) {
		static const u8 ccmp[] = { 0, 0x0f, 0xac, 4 };
		static const u8 psk[] = { 0, 0x0f, 0xac, 2 };
		if (!rsn || rsn[1] < 20 || get_unaligned_le16(rsn + 2) != 1 ||
		    memcmp(rsn + 4, ccmp, 4) || get_unaligned_le16(rsn + 8) != 1 ||
		    memcmp(rsn + 10, ccmp, 4) || get_unaligned_le16(rsn + 14) != 1 ||
		    memcmp(rsn + 16, psk, 4) || (get_unaligned_le16(rsn + 20) & 0xc0))
			return -EOPNOTSUPP;
	} else if (rsn) {
		return -EINVAL;
	}
	connection = kzalloc(sizeof(*connection), GFP_KERNEL);
	if (!connection)
		return -ENOMEM;
	init_completion(&connection->key_completion);
	init_completion(&connection->pm_completion);
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
	rcu_read_unlock();
	if (ret)
		goto put_bss;
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
		key->index = index;
		key->installed = true;
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

	if (wdev != &radio->wdev || link_id != -1 || index > 3 || (pairwise && index))
		return -EINVAL;
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
		if (pairwise)
			radio->connection->pairwise_ready = false;
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
	else if (authorize && connection->join.negotiated.security != MT6797_WIFI_AIS_SECURITY_OPEN &&
		 (!connection->pairwise_key.installed || !connection->pairwise_ready ||
		  !connection->group_key.installed))
		ret = -ENOKEY;
	else
		connection->authorized = authorize;
	spin_unlock_irqrestore(&radio->lock, flags);
out:
	mutex_unlock(&radio->connection_lock);
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
	.change_station = mt6797_radio_change_station,
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
	static const u32 ciphers[] = { WLAN_CIPHER_SUITE_CCMP };
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
	skb_queue_head_init(&radio->tx_queue);
	radio->tx_diag.lifetime_start_ns = ktime_get_ns();
	set_wiphy_dev(wiphy, dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 0;
	wiphy->cipher_suites = ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(ciphers);
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
	mt6797_radio_purge_tx(radio);
	unregister_netdev(radio->netdev);
	wiphy_unregister(radio->wdev.wiphy);
	free_netdev(radio->netdev);
	wiphy_free(radio->wdev.wiphy);
}
