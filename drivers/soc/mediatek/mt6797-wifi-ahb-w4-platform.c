// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 GEN3 retained AHB/SDIO-like HIF owner (vendor: ahb.c/gl_init.c). */
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>
#include <net/cfg80211.h>

#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w3-session.h>

#define W4_WHIER 0x0014
#define W4_WRDR0 0x0050
#define W4_WRDR1 0x0054
#define W4_WTDR1 0x0034
#define W4_WRPLR 0x0090
#define W4_WTQCR0 0x0130
#define W4_WHIER_DEFAULT 0xffffff47U
#define W4_RX_MAX 2352U
#define W4_RX_BUDGET 32U

struct mt6797_wifi_w4 {
	struct device *dev, *controller_dev;
	void __iomem *portal;
	struct regulator *bt_paldo, *wifi_paldo;
	struct wiphy *wiphy;
	struct net_device *ndev;
	struct wireless_dev *wdev;
	struct mutex lock;
	struct work_struct rx_work;
	int irq;
	struct mt6797_consys_runtime_controller_wifi_lease runtime;
	const struct mt6797_consys_stp_session_lease *stp_session;
	struct mt6797_wifi_ahb_w3_session session;
	struct cfg80211_scan_request *scan_request;
	bool controller_started, w1_retained, opened;
};

static const struct ieee80211_channel w4_channels[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
};
static struct ieee80211_rate w4_rates[] = {
	{ .bitrate = 10 }, { .bitrate = 20 }, { .bitrate = 55 }, { .bitrate = 110 },
	{ .bitrate = 60 }, { .bitrate = 120 }, { .bitrate = 240 }, { .bitrate = 540 },
};
static struct ieee80211_supported_band w4_band = {
	.channels = (struct ieee80211_channel *)w4_channels,
	.n_channels = ARRAY_SIZE(w4_channels), .bitrates = w4_rates,
	.n_bitrates = ARRAY_SIZE(w4_rates),
};
static struct mt6797_wifi_w4 *w4_ndev(struct net_device *ndev)
{
	return *(struct mt6797_wifi_w4 **)netdev_priv(ndev);
}
static struct mt6797_wifi_w4 *w4_wiphy(struct wiphy *wiphy)
{
	return *(struct mt6797_wifi_w4 **)wiphy_priv(wiphy);
}

/* Vendor ahb.c kalDevPort{Read,Write}: fixed function-one CMD53 portal. */
static int w4_cmd53(struct mt6797_wifi_w4 *w4, bool write, u16 port,
			    u8 *buf, size_t len)
{
	u32 cmd;
	size_t transfer, i;
	if (!w4 || !buf || !len || len > W4_RX_MAX + sizeof(u32))
		return -EINVAL;
	transfer = ALIGN(len, sizeof(u32));
	if (transfer >= MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		transfer = ALIGN(transfer, MT6797_WIFI_AHB_SDIO_BLOCK_SIZE);
	if (transfer > W4_RX_MAX + MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return -EMSGSIZE;
	if (transfer < MT6797_WIFI_AHB_SDIO_BLOCK_SIZE) {
		if (write)
			return mt6797_wifi_ahb_cmd53_write_pack(port, transfer, &cmd);
		return mt6797_wifi_ahb_cmd53_read_pack(port, transfer, &cmd);
	}
	cmd = transfer / MT6797_WIFI_AHB_SDIO_BLOCK_SIZE |
		((u32)port << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) | BIT(27) |
		BIT(MT6797_WIFI_AHB_CMD_FUNC_SHIFT) | (write ? BIT(31) : 0);
	writel(cmd, w4->portal + MT6797_WIFI_AHB_CMD_SETUP);
	wmb();
	if (write) {
		for (i = 0; i < len; i += sizeof(u32))
			writel(get_unaligned_le32(buf + i),
			       w4->portal + MT6797_WIFI_AHB_CMD53_DATA);
	} else {
		for (i = 0; i < transfer; i += sizeof(u32))
			put_unaligned_le32(readl(w4->portal + MT6797_WIFI_AHB_CMD53_DATA),
					   buf + i);
	}
	return 0;
}
static int w4_mcr_read(void *ctx, u32 off, u32 *val)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	if (!w4 || !val || off >= MT6797_WIFI_AHB_HIF_PORTAL_SIZE)
		return -EINVAL;
	*val = readl(w4->portal + off);
	return 0;
}
static int w4_mcr_write(void *ctx, u32 off, u32 val)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	if (!w4 || off >= MT6797_WIFI_AHB_HIF_PORTAL_SIZE)
		return -EINVAL;
	writel(val, w4->portal + off);
	return 0;
}
static int w4_hif_get(void *ctx) { return ctx ? 0 : -EINVAL; }
static void w4_hif_put(void *ctx) { }
static int w4_irq_request(void *ctx) { return ctx ? 0 : -EINVAL; }
static void w4_irq_free(void *ctx) { }
static int w4_irq_enable(void *ctx)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	if (!w4) return -EINVAL;
	enable_irq(w4->irq);
	return 0;
}
static int w4_irq_disable(void *ctx)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	if (!w4) return -EINVAL;
	disable_irq(w4->irq);
	return 0;
}
static int w4_tx_released(void *ctx, u32 released[])
{
	struct mt6797_wifi_w4 *w4 = ctx;
	unsigned int i;
	if (!w4 || !released) return -EINVAL;
	/* HAL_READ_TX_RELEASED_COUNT: four WTQCR words, two queues each. */
	for (i = 0; i < MT6797_WIFI_AHB_W3_TX_QUEUE_COUNT; i++)
		released[i] = (readl(w4->portal + W4_WTQCR0 + 4 * (i / 2)) >>
				((i & 1) * 16)) & U16_MAX;
	return 0;
}
static int w4_rx_lengths(void *ctx, u16 *rx0, u16 *rx1)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	u32 v;
	if (!w4 || !rx0 || !rx1) return -EINVAL;
	v = readl(w4->portal + W4_WRPLR);
	*rx0 = v & U16_MAX; *rx1 = v >> 16;
	return 0;
}
static int w4_rx_drain(void *ctx, unsigned int port, u16 len)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	u8 *buf;
	int ret;
	if (!w4 || port > 1 || !len || len > W4_RX_MAX) return -EMSGSIZE;
	buf = kzalloc(W4_RX_MAX + MT6797_WIFI_AHB_SDIO_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) return -ENOMEM;
	ret = w4_cmd53(w4, false, port ? W4_WRDR1 : W4_WRDR0, buf,
			len + sizeof(u32));
	kfree(buf);
	return ret;
}
/* nicRxFillRFB() distilled for header-translated Ethernet frames. */
static int w4_rx_deliver(struct mt6797_wifi_w4 *w4, unsigned int port, u16 len)
{
	u8 *buf;
	u16 byte_count, type;
	u16 status;
	unsigned int off = 16;
	u8 groups, header_len;
	struct sk_buff *skb;
	int ret;

	if (len < 16 || len > W4_RX_MAX)
		return -EMSGSIZE;
	buf = kzalloc(W4_RX_MAX + MT6797_WIFI_AHB_SDIO_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = w4_cmd53(w4, false, port ? W4_WRDR1 : W4_WRDR0, buf,
			len + sizeof(u32));
	if (ret)
		goto out;
	/* SW-defined firmware events carry WIFI_EVENT_T directly, no RXD. */
	if (len >= 8 && get_unaligned_le16(buf + 2) == 0xe000) {
		if (buf[4] == 0x0d && w4->scan_request) { /* EVENT_ID_SCAN_DONE */
			struct cfg80211_scan_info info = { };
			cfg80211_scan_done(w4->scan_request, &info);
			w4->scan_request = NULL;
		}
		goto out;
	}
	byte_count = get_unaligned_le16(buf);
	type = get_unaligned_le16(buf + 2);
	header_len = buf[6];
	status = get_unaligned_le16(buf + 10);
	groups = (type >> 9) & 0xf;
	if (byte_count > len || byte_count < off || !(header_len & BIT(7)) || status)
		goto out;
	/* Vendor group order is 4,1,2,3 following HW_MAC_RX_DESC_T. */
	if (groups & BIT(3)) off += 12;
	if (groups & BIT(0)) off += 16;
	if (groups & BIT(1)) off += 8;
	if (groups & BIT(2)) off += 24;
	if (header_len & BIT(6)) off += 2;
	if (off >= byte_count || byte_count - off < ETH_HLEN)
		goto out;
	skb = netdev_alloc_skb_ip_align(w4->ndev, byte_count - off);
	if (!skb) { ret = -ENOMEM; goto out; }
	skb_put_data(skb, buf + off, byte_count - off);
	skb->protocol = eth_type_trans(skb, w4->ndev);
	netif_rx(skb);
	w4->ndev->stats.rx_packets++;
	w4->ndev->stats.rx_bytes += byte_count - off;
out:
	kfree(buf);
	return ret;
}
static int w4_capability(void *ctx)
{
	/* W1 retained stream contains the exact vendor W2 NIC-capability poll. */
	return ctx && ((struct mt6797_wifi_w4 *)ctx)->w1_retained ? 0 : -EPIPE;
}
static int w4_configure(void *ctx)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	if (!w4) return -EINVAL;
	/* nic.c's normal interrupt enable mask, before W3 writes INT_SET. */
	writel(W4_WHIER_DEFAULT, w4->portal + W4_WHIER);
	return 0;
}
static const struct mt6797_wifi_ahb_w3_ops w4_hif_ops = {
	.hif_get = w4_hif_get, .hif_put = w4_hif_put,
	.host_irq_request = w4_irq_request, .host_irq_free = w4_irq_free,
	.host_irq_enable = w4_irq_enable, .host_irq_disable = w4_irq_disable,
	.mcr_read = w4_mcr_read, .mcr_write = w4_mcr_write,
	.tx_released = w4_tx_released, .rx_lengths = w4_rx_lengths,
	.rx_drain = w4_rx_drain, .capability_query = w4_capability,
	.configure = w4_configure,
};

static void w4_rx_work(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(work, struct mt6797_wifi_w4, rx_work);
	unsigned int i;
	mutex_lock(&w4->lock);
	if (!w4->opened) goto out;
	/* Bound the drain as nicRxProcessRFBs does; never leave a wedged port. */
	for (i = 0; i < W4_RX_BUDGET; i++) {
		u16 rx0, rx1;
		if (w4_rx_lengths(w4, &rx0, &rx1) || (!rx0 && !rx1)) break;
		if (rx0) w4_rx_deliver(w4, 0, rx0);
		if (rx1) w4_rx_deliver(w4, 1, rx1);
	}
out:
	mutex_unlock(&w4->lock);
}
static irqreturn_t w4_irq_thread(int irq, void *data)
{
	struct mt6797_wifi_w4 *w4 = data;
	if (!READ_ONCE(w4->opened)) return IRQ_NONE;
	schedule_work(&w4->rx_work);
	return IRQ_HANDLED;
}
static int w4_firmware_stage(void *ctx,
	const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	int ret = mt6797_wifi_ahb_w1_firmware_start_retained(w4->dev, lease,
		w4->bt_paldo, w4->wifi_paldo);
	if (!ret) w4->w1_retained = true;
	return ret;
}
static void w4_stop_locked(struct mt6797_wifi_w4 *w4)
{
	if (!w4->opened) return;
	netif_tx_stop_all_queues(w4->ndev);
	mt6797_wifi_ahb_w3_session_stop(&w4->session);
	/* Set before releasing the lock: a queued worker will only observe stop. */
	w4->opened = false;
	mt6797_consys_runtime_controller_wifi_session_put(&w4->runtime);
	if (w4->w1_retained) mt6797_wifi_ahb_w1_firmware_stop_retained(w4->wifi_paldo);
	w4->w1_retained = false;
	if (w4->controller_started) mt6797_consys_runtime_controller_stop(w4->controller_dev);
	w4->controller_started = false;
}
static int w4_open(struct net_device *ndev)
{
	struct mt6797_wifi_w4 *w4 = w4_ndev(ndev);
	int ret;
	mutex_lock(&w4->lock);
	if (w4->opened) { ret = 0; goto out; }
	ret = mt6797_consys_runtime_controller_start_with_postpatch_stage(
		w4->controller_dev, w4_firmware_stage, w4);
	if (ret) goto out;
	w4->controller_started = true;
	ret = mt6797_consys_runtime_controller_wifi_session_get(w4->controller_dev,
		&w4->runtime, &w4->stp_session);
	if (ret) goto fail;
	ret = mt6797_wifi_ahb_w3_session_start(&w4->session, &w4_hif_ops, w4);
	if (ret) { mt6797_consys_runtime_controller_wifi_session_put(&w4->runtime); goto fail; }
	w4->opened = true; netif_tx_start_all_queues(ndev); goto out;
fail:
	if (w4->w1_retained) mt6797_wifi_ahb_w1_firmware_stop_retained(w4->wifi_paldo);
	w4->w1_retained = false;
	mt6797_consys_runtime_controller_stop(w4->controller_dev);
	w4->controller_started = false;
out:
	mutex_unlock(&w4->lock);
	return ret;
}
static int w4_stop(struct net_device *ndev)
{
	struct mt6797_wifi_w4 *w4 = w4_ndev(ndev);
	mutex_lock(&w4->lock);
	if (w4->scan_request) {
		struct cfg80211_scan_info info = { .aborted = true };
		cfg80211_scan_done(w4->scan_request, &info); w4->scan_request = NULL;
	}
	w4_stop_locked(w4);
	mutex_unlock(&w4->lock);
	/* The worker takes the same mutex, so waiting while holding it deadlocks. */
	cancel_work_sync(&w4->rx_work);
	return 0;
}
static netdev_tx_t w4_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mt6797_wifi_w4 *w4 = w4_ndev(ndev);
	u8 *frame;
	const unsigned int txd_len = 28; /* NIC_TX_DESC_LONG_FORMAT_LENGTH */
	const unsigned int wire_length = txd_len + skb->len;
	const unsigned int length = ALIGN(wire_length, sizeof(u32));
	int ret;

	if (!READ_ONCE(w4->opened) || length > W4_RX_MAX)
		goto drop;
	/* ndo_start_xmit is atomic: bounded portal ownership, never sleep. */
	if (!mutex_trylock(&w4->lock))
		return NETDEV_TX_BUSY;
	frame = kzalloc(length, GFP_ATOMIC);
	if (!frame) { mutex_unlock(&w4->lock); return NETDEV_TX_BUSY; }
	/* vendor HW_MAC_TX_DESC_T: LMAC/AC0, non-802.11 Ethernet-II, long TXD */
	put_unaligned_le16(wire_length, frame);            /* u2TxByteCount */
	frame[2] = (ETH_HLEN - ETH_TLEN + txd_len) >> 1;  /* EtherType word */
	frame[3] = 0;                                     /* LMAC, AC0 */
	frame[4] = 31;                                    /* NIC_TX_DEFAULT_WLAN_INDEX */
	frame[5] = BIT(4) | BIT(7);                       /* Ethernet-II, long */
	frame[7] = 0;                                     /* own-MAC zero */
	frame[10] = 0xff;                                 /* no lifetime limit */
	put_unaligned_le16(30, frame + 12);               /* retry count limit */
	if (skb_copy_bits(skb, 0, frame + txd_len, skb->len))
		ret = -EFAULT;
	else
		ret = w4_cmd53(w4, true, W4_WTDR1, frame, length);
	kfree(frame);
	mutex_unlock(&w4->lock);
	if (ret)
		goto drop;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}
static const struct net_device_ops w4_netdev_ops = {
	.ndo_open = w4_open, .ndo_stop = w4_stop, .ndo_start_xmit = w4_xmit,
};
static int w4_send_scan(struct mt6797_wifi_w4 *w4,
			const struct cfg80211_scan_request *req)
{
	u8 cmd[8 + 48 + 64 + 2] = { };
	unsigned int i, n = min_t(unsigned int, req->n_channels, 32);
	unsigned int body = 48 + n * 2;
	u16 total = 8 + body;

	/* WIFI_CMD_T + CMD_SCAN_REQ from vendor scan_fsm.c:174-246. */
	put_unaligned_le16(total, cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = 0x1a;       /* CMD_ID_SCAN_REQ */
	cmd[5] = 0x20;       /* command packet */
	cmd[6] = 1;          /* set */
	cmd[7] = 1;          /* command sequence */
	cmd[8] = 1;          /* scan sequence */
	cmd[9] = 0;          /* AIS BSS index */
	cmd[10] = 1;         /* active scan */
	cmd[11] = req->n_ssids ? BIT(2) : BIT(0);
	if (req->n_ssids) {
		cmd[12] = min_t(unsigned int, req->ssids[0].ssid_len, 32);
		memcpy(cmd + 20, req->ssids[0].ssid, cmd[12]);
	}
	put_unaligned_le16(30, cmd + 14); /* dwell ms */
	cmd[52] = n ? 1 : 0;              /* specified channels/all */
	cmd[53] = n;
	for (i = 0; i < n; i++) {
		cmd[56 + 2 * i] = req->channels[i]->band == NL80211_BAND_2GHZ ? 1 : 2;
		cmd[57 + 2 * i] = ieee80211_frequency_to_channel(req->channels[i]->center_freq);
	}
	return w4_cmd53(w4, true, W4_WTDR1, cmd, ALIGN(total, 4));
}
static int w4_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;
	mutex_lock(&w4->lock);
	if (!w4->opened || w4->scan_request) { mutex_unlock(&w4->lock); return -EBUSY; }
	ret = w4_send_scan(w4, request);
	if (ret) { mutex_unlock(&w4->lock); return ret; }
	w4->scan_request = request;
	mutex_unlock(&w4->lock);
	return 0;
}
static int w4_connect(struct wiphy *wiphy, struct net_device *ndev,
	struct cfg80211_connect_params *sme) { return -EOPNOTSUPP; }
static int w4_disconnect(struct wiphy *wiphy, struct net_device *ndev, u16 reason)
{
	if (netif_running(ndev)) cfg80211_disconnected(ndev, reason, NULL, 0, true, GFP_KERNEL);
	return 0;
}
static const struct cfg80211_ops w4_cfg80211_ops = {
	.scan = w4_scan, .connect = w4_connect, .disconnect = w4_disconnect,
};
static int mt6797_wifi_w4_probe(struct platform_device *pdev)
{
	struct mt6797_wifi_w4 *w4;
	struct resource *res;
	struct device_node *node;
	struct platform_device *controller;
	int ret;
	if (!device_property_read_bool(&pdev->dev, "mediatek,experimental-wifi-w4-platform")) return -EPERM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != MT6797_WIFI_AHB_HIF_BASE ||
	    resource_size(res) != MT6797_WIFI_AHB_HIF_PORTAL_SIZE) return -EINVAL;
	w4 = devm_kzalloc(&pdev->dev, sizeof(*w4), GFP_KERNEL); if (!w4) return -ENOMEM;
	w4->dev = &pdev->dev; mutex_init(&w4->lock); INIT_WORK(&w4->rx_work, w4_rx_work);
	w4->portal = devm_ioremap_resource(&pdev->dev, res); if (IS_ERR(w4->portal)) return PTR_ERR(w4->portal);
	w4->irq = platform_get_irq_byname(pdev, "wifi"); if (w4->irq < 0) return w4->irq;
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0); if (!node) return -EINVAL;
	controller = of_find_device_by_node(node); of_node_put(node); if (!controller) return -EPROBE_DEFER;
	w4->controller_dev = &controller->dev;
	w4->bt_paldo = devm_regulator_get(&pdev->dev, "bt-paldo");
	if (IS_ERR(w4->bt_paldo)) { ret = PTR_ERR(w4->bt_paldo); goto put_controller; }
	w4->wifi_paldo = devm_regulator_get(&pdev->dev, "wifi-paldo");
	if (IS_ERR(w4->wifi_paldo)) { ret = PTR_ERR(w4->wifi_paldo); goto put_controller; }
	ret = devm_request_threaded_irq(&pdev->dev, w4->irq, NULL, w4_irq_thread,
		IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(&pdev->dev), w4);
	if (ret) goto put_controller;
	w4->wiphy = wiphy_new(&w4_cfg80211_ops, sizeof(struct mt6797_wifi_w4 *));
	if (!w4->wiphy) { ret = -ENOMEM; goto put_controller; }
	*(struct mt6797_wifi_w4 **)wiphy_priv(w4->wiphy) = w4;
	w4->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	w4->wiphy->bands[NL80211_BAND_2GHZ] = &w4_band; set_wiphy_dev(w4->wiphy, &pdev->dev);
	ret = wiphy_register(w4->wiphy); if (ret) goto free_wiphy;
	w4->ndev = alloc_netdev(sizeof(struct mt6797_wifi_w4 *), "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!w4->ndev) { ret = -ENOMEM; goto unregister_wiphy; }
	*(struct mt6797_wifi_w4 **)netdev_priv(w4->ndev) = w4;
	w4->ndev->netdev_ops = &w4_netdev_ops; SET_NETDEV_DEV(w4->ndev, &pdev->dev); eth_hw_addr_random(w4->ndev);
	w4->wdev = kzalloc(sizeof(*w4->wdev), GFP_KERNEL);
	if (!w4->wdev) { ret = -ENOMEM; goto free_netdev; }
	w4->wdev->wiphy = w4->wiphy;
	w4->wdev->iftype = NL80211_IFTYPE_STATION;
	w4->wdev->netdev = w4->ndev;
	w4->ndev->ieee80211_ptr = w4->wdev;
	ret = cfg80211_register_netdevice(w4->ndev); if (ret) goto free_wdev;
	platform_set_drvdata(pdev, w4);
	dev_info(&pdev->dev, "GEN3 HIF owner registered %s\n", w4->ndev->name);
	return 0;
free_wdev: kfree(w4->wdev);
free_netdev: free_netdev(w4->ndev);
unregister_wiphy: wiphy_unregister(w4->wiphy);
free_wiphy: wiphy_free(w4->wiphy);
put_controller: put_device(w4->controller_dev); return ret;
}
static void mt6797_wifi_w4_remove(struct platform_device *pdev)
{
	struct mt6797_wifi_w4 *w4 = platform_get_drvdata(pdev);
	if (!w4) return;
	cfg80211_unregister_netdevice(w4->ndev); kfree(w4->wdev); free_netdev(w4->ndev);
	wiphy_unregister(w4->wiphy); wiphy_free(w4->wiphy); put_device(w4->controller_dev);
}
static const struct of_device_id mt6797_wifi_w4_of_match[] = {
	{ .compatible = "mediatek,mt6797-wifi-ahb-w4-platform" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_wifi_w4_of_match);
static struct platform_driver mt6797_wifi_w4_driver = {
	.probe = mt6797_wifi_w4_probe, .remove = mt6797_wifi_w4_remove,
	.driver = { .name = "mt6797-wifi-ahb-w4-platform", .of_match_table = mt6797_wifi_w4_of_match },
};
module_platform_driver(mt6797_wifi_w4_driver);
MODULE_LICENSE("GPL");
