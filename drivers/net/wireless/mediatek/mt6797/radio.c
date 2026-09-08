// SPDX-License-Identifier: GPL-2.0-only
/* cfg80211 station interface and asynchronous, firmware-backed passive scan. */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include "capability.h"
#include "hif.h"
#include "hif_regs.h"
#include "hif_runtime.h"
#include "radio.h"
#include "rx_metadata.h"
#include "scan.h"

struct mt6797_wifi_radio {
	struct wireless_dev wdev;
	struct net_device *netdev;
	struct mt6797_wifi_hif_irq *irq;
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
	int scan_error;
	bool armed;
	bool abort_scan;
	bool stopping;
};

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
		ret = mt6797_wifi_ahb_cmd53_write(radio->irq->hif,
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
				mt6797_wifi_ahb_cmd53_write(radio->irq->hif,
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
	else if (radio->request)
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

static const struct cfg80211_ops mt6797_radio_ops = {
	.scan = mt6797_radio_scan,
	.abort_scan = mt6797_radio_abort_scan,
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
	flush_work(&radio->scan_work);
	return 0;
}

static netdev_tx_t mt6797_radio_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	/* Carrier and queue remain off until association and data TX are implemented. */
	dev_kfree_skb(skb);
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
	const struct mt6797_wifi_capability *capability)
{
	static const int rates[] = { 10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540 };
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
	radio->wdev.wiphy = wiphy;
	radio->wdev.iftype = NL80211_IFTYPE_STATION;
	radio->command_sequence = 2; /* Capability=1, basic configuration=2. */
	spin_lock_init(&radio->lock);
	init_completion(&radio->scan_done);
	INIT_WORK(&radio->scan_work, mt6797_radio_scan_work);
	set_wiphy_dev(wiphy, dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 0;
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
	radio->netdev->ieee80211_ptr = &radio->wdev;
	radio->wdev.netdev = radio->netdev;
	SET_NETDEV_DEV(radio->netdev, dev);
	if (is_valid_ether_addr(capability->mac_address))
		eth_hw_addr_set(radio->netdev, capability->mac_address);
	else
		eth_hw_addr_random(radio->netdev);
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
	radio->abort_scan = true;
	complete(&radio->scan_done);
	spin_unlock_irqrestore(&radio->lock, flags);
	flush_work(&radio->scan_work);
	unregister_netdev(radio->netdev);
	wiphy_unregister(radio->wdev.wiphy);
	free_netdev(radio->netdev);
	wiphy_free(radio->wdev.wiphy);
}
