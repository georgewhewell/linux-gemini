// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 GEN3 retained AHB/SDIO-like HIF owner (vendor: ahb.c/gl_init.c). */
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
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
#include <linux/soc/mediatek/mt6797-consys-g2d-postpatch-owner.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w3-session.h>

#define W4_WHIER 0x0014
#define W4_WRDR0 0x0050
#define W4_WRDR1 0x0054
#define W4_WTDR1 0x0034
#define W4_WRPLR 0x0090
#define W4_WTQCR0 0x0130
#define W4_WHIER_DEFAULT 0xffffff47U
#define W4_WHCR_RX_ENHANCE_MODE_EN BIT(16)
#define W4_WHCR_MAX_HIF_RX_LEN_NUM 0x00003f00U
#define W4_WHCR_RECV_MAILBOX_RD_CLR_EN BIT(2)
#define W4_WHCR_W_INT_CLR_CTRL BIT(1)
#define W4_RX_MAX 2352U
#define W4_RX_BUDGET 32U
#define IEEE80211_HDRLEN 24U
#define W4_WLAN_INDEX_DEFAULT 31U
#define W4_STA_INDEX_AIS_AP 0U
#define W4_JOIN_TIMEOUT_MS 1000U
#define W4_SCAN_POLL_INTERVAL_MS 20U
#define W4_SCAN_POLL_TIMEOUT_MS 1500U

static bool enable_experimental_wifi_w4_platform;
module_param_named(enable_experimental_wifi_w4_platform,
		enable_experimental_wifi_w4_platform, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w4_platform,
	"allow MT6797 W4 retained WLAN platform ownership (default: false)");

/* The vendor's AIS -> SAA chronology, condensed to the open-system path. */
enum w4_join_state {
	W4_JOIN_IDLE,
	W4_JOIN_WAIT_AUTH2,
	W4_JOIN_WAIT_ASSOC2,
	W4_JOIN_CONNECTED,
};

struct mt6797_wifi_w4 {
	struct device *dev, *controller_dev, *retained_owner_dev;
	void __iomem *portal;
	struct regulator *bt_paldo, *wifi_paldo;
	struct wiphy *wiphy;
	struct net_device *ndev;
	struct wireless_dev *wdev;
	struct mutex lock;
	struct work_struct rx_work;
	struct delayed_work join_timeout, scan_poll;
	int irq;
	struct mt6797_consys_runtime_controller_wifi_lease runtime;
	const struct mt6797_consys_stp_session_lease *stp_session;
	struct mt6797_wifi_ahb_w3_session session;
	struct cfg80211_scan_request *scan_request;
	u8 candidate_bssid[ETH_ALEN];
	u8 candidate_channel;
	u8 wlan_index;
	u8 sta_index;
	u8 join_ssid[IEEE80211_MAX_SSID_LEN];
	u8 join_ssid_len;
	u8 assoc_req_ies[IEEE80211_MAX_SSID_LEN + 16];
	u8 assoc_req_ie_len;
	unsigned int scan_poll_count;
	unsigned long scan_poll_deadline;
	enum w4_join_state join_state;
	bool candidate_valid, retained_owner_released;
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
	int ret;
	if (!w4 || !buf || !len || len > W4_RX_MAX + sizeof(u32))
		return -EINVAL;
	transfer = ALIGN(len, sizeof(u32));
	if (transfer >= MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		transfer = ALIGN(transfer, MT6797_WIFI_AHB_SDIO_BLOCK_SIZE);
	if (transfer > W4_RX_MAX + MT6797_WIFI_AHB_SDIO_BLOCK_SIZE)
		return -EMSGSIZE;
	if (transfer < MT6797_WIFI_AHB_SDIO_BLOCK_SIZE) {
		if (write)
			ret = mt6797_wifi_ahb_cmd53_write_pack(port, transfer, &cmd);
		else
			ret = mt6797_wifi_ahb_cmd53_read_pack(port, transfer, &cmd);
		if (ret)
			return ret;
	} else {
		cmd = transfer / MT6797_WIFI_AHB_SDIO_BLOCK_SIZE |
			((u32)port << MT6797_WIFI_AHB_CMD_ADDR_SHIFT) | BIT(27) |
			BIT(MT6797_WIFI_AHB_CMD_FUNC_SHIFT) | (write ? BIT(31) : 0);
	}
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
	u8 data[sizeof(*val)];
	int ret;

	if (!w4 || !val || off > MT6797_WIFI_AHB_CMD_ADDR_MAX || off & 3)
		return -EINVAL;
	/* Vendor kalDevRegRead()/HAL_MCR_RD is a function-one CMD53 transfer.
	 * The mapped AHB aperture holds only the command engine; MCR offsets are
	 * SDIO register addresses, never MMIO offsets into that aperture.
	 */
	ret = w4_cmd53(w4, false, off, data, sizeof(data));
	if (!ret)
		*val = get_unaligned_le32(data);
	return ret;
}
static int w4_mcr_write(void *ctx, u32 off, u32 val)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	u8 data[sizeof(val)];

	if (!w4 || off > MT6797_WIFI_AHB_CMD_ADDR_MAX || off & 3)
		return -EINVAL;
	put_unaligned_le32(val, data);
	return w4_cmd53(w4, true, off, data, sizeof(data));
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
	u32 word;
	int ret;
	if (!w4 || !released) return -EINVAL;
	/* HAL_READ_TX_RELEASED_COUNT: four WTQCR words, two queues each. */
	for (i = 0; i < MT6797_WIFI_AHB_W3_TX_QUEUE_COUNT; i++) {
		ret = w4_mcr_read(w4, W4_WTQCR0 + 4 * (i / 2), &word);
		if (ret)
			return ret;
		released[i] = (word >> ((i & 1) * 16)) & U16_MAX;
	}
	return 0;
}
static int w4_rx_lengths(void *ctx, u16 *rx0, u16 *rx1)
{
	struct mt6797_wifi_w4 *w4 = ctx;
	u32 v;
	int ret;
	if (!w4 || !rx0 || !rx1) return -EINVAL;
	ret = w4_mcr_read(w4, W4_WRPLR, &v);
	if (ret)
		return ret;
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
static void w4_rx_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len);
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
	if (byte_count > len || byte_count < off || status)
		goto out;
	/* Vendor group order is 4,1,2,3 following HW_MAC_RX_DESC_T. */
	if (groups & BIT(3)) off += 12;
	if (groups & BIT(0)) off += 16;
	if (groups & BIT(1)) off += 8;
	if (groups & BIT(2)) off += 24;
	if (header_len & BIT(6)) off += 2;
	if (off >= byte_count)
		goto out;
	/* Vendor scan results are SW-defined management RX records, not events. */
	if (!(header_len & BIT(7)) && ((type >> 13) & 7) == 3 &&
	    byte_count - off >= 36 && ((buf[off] & 0xf0) == 0x80 ||
					 (buf[off] & 0xf0) == 0x50)) {
		struct ieee80211_channel *chan;
		struct cfg80211_bss *bss;
		int freq = ieee80211_channel_to_frequency(buf[5], NL80211_BAND_2GHZ);
		chan = ieee80211_get_channel(w4->wiphy, freq);
		if (chan) {
			bss = cfg80211_inform_bss_frame(w4->wiphy, chan,
				(struct ieee80211_mgmt *)(buf + off), byte_count - off,
				-4500, GFP_KERNEL);
			if (bss)
				cfg80211_put_bss(w4->wiphy, bss);
			ether_addr_copy(w4->candidate_bssid, buf + off + 16);
			w4->candidate_channel = buf[5];
			w4->candidate_valid = true;
		}
		goto out;
	}
	if (!(header_len & BIT(7)) && ((type >> 13) & 7) == 3) {
		w4_rx_mgmt(w4, buf + off, byte_count - off);
		goto out;
	}
	if (byte_count - off < ETH_HLEN)
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
	u32 whcr;
	int ret;

	if (!w4)
		return -EINVAL;
	/* Vendor nicSDIOInit() selects the ordinary, non-enhanced RX report
	 * format used below (one WRPLR length pair), with the default 16 slots.
	 * wlanAdapterStart() then leaves mailbox and interrupt status in read-clear
	 * mode before nicEnableInterrupt() writes WHLPCR.INT_SET. */
	ret = w4_mcr_read(w4, MT6797_WIFI_AHB_W3_MCR_WHCR, &whcr);
	if (ret)
		return ret;
	whcr &= ~(W4_WHCR_RX_ENHANCE_MODE_EN | W4_WHCR_MAX_HIF_RX_LEN_NUM |
		  W4_WHCR_RECV_MAILBOX_RD_CLR_EN | W4_WHCR_W_INT_CLR_CTRL);
	ret = w4_mcr_write(w4, MT6797_WIFI_AHB_W3_MCR_WHCR, whcr);
	if (ret)
		return ret;
	/* nic.c's normal interrupt enable mask, before W3 writes INT_SET. */
	return w4_mcr_write(w4, W4_WHIER, W4_WHIER_DEFAULT);
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

/* Read-only MCR evidence.  WHCR is in vendor read-clear mode, so a WHISR
 * read is the hardware's documented acknowledgement, not a fabricated W1C. */
static void w4_diag_hif(struct mt6797_wifi_w4 *w4, const char *stage)
{
	u32 whlpcr = 0, whcr = 0, whisr = 0, whier = 0, wrplr = 0;
	int ret = 0, result;

	result = w4_mcr_read(w4, MT6797_WIFI_AHB_W3_MCR_WHLPCR, &whlpcr);
	if (result && !ret) ret = result;
	result = w4_mcr_read(w4, MT6797_WIFI_AHB_W3_MCR_WHCR, &whcr);
	if (result && !ret) ret = result;
	result = w4_mcr_read(w4, MT6797_WIFI_AHB_W3_MCR_WHISR, &whisr);
	if (result && !ret) ret = result;
	result = w4_mcr_read(w4, W4_WHIER, &whier);
	if (result && !ret) ret = result;
	result = w4_mcr_read(w4, W4_WRPLR, &wrplr);
	if (result && !ret) ret = result;
	dev_info(w4->dev,
		 "W4_DIAG stage=%s mcr-ret=%d whlpcr=%#x whcr=%#x whisr=%#x whier=%#x wrplr=%#x whisr-read-clear=1\n",
		 stage, ret, whlpcr, whcr, whisr, whier, wrplr);
}

static void w4_rx_work(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(work, struct mt6797_wifi_w4, rx_work);
	unsigned int i;
	u32 whisr;
	int ret;
	mutex_lock(&w4->lock);
	if (!w4->opened) goto out;
	/* W3 selected vendor read-clear mode.  HAL_MCR_RD(MCR_WHISR) is the
	 * acknowledgement which drops the level-triggered host line before RX. */
	ret = w4_mcr_read(w4, MT6797_WIFI_AHB_W3_MCR_WHISR, &whisr);
	if (ret) {
		dev_err(w4->dev, "W4_DIAG stage=irq-ack ret=%d\n", ret);
		goto out;
	}
	if (!whisr)
		goto out;
	dev_dbg(w4->dev, "W4_DIAG stage=irq-ack ret=0 whisr=%#x\n", whisr);
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
/*
 * Vendor nicRxWaitResponse() polls WRPLR/WRDR1 for command events.  Use the
 * same CMD53 mechanism only while one scan is pending: this diagnoses a
 * missing host EINT without guessing a GIC polarity or injecting an ACK.
 */
static void w4_scan_poll(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(to_delayed_work(work),
		struct mt6797_wifi_w4, scan_poll);
	u16 rx0, rx1;
	int ret;

	mutex_lock(&w4->lock);
	if (!w4->opened || !w4->scan_request)
		goto out;
	w4->scan_poll_count++;
	ret = w4_rx_lengths(w4, &rx0, &rx1);
	if (ret) {
		dev_err(w4->dev, "W4_DIAG stage=scan-poll ret=%d count=%u\n",
			ret, w4->scan_poll_count);
	} else if (rx0 || rx1) {
		dev_info(w4->dev,
			 "W4_DIAG stage=scan-poll ret=0 count=%u rx0=%u rx1=%u\n",
			 w4->scan_poll_count, rx0, rx1);
		if (rx0)
			w4_rx_deliver(w4, 0, rx0);
		if (rx1)
			w4_rx_deliver(w4, 1, rx1);
	}
	if (!w4->scan_request)
		goto out;
	if (time_after_eq(jiffies, w4->scan_poll_deadline)) {
		struct cfg80211_scan_info info = { .aborted = true };

		w4_diag_hif(w4, "scan-poll-timeout");
		dev_info(w4->dev, "W4_DIAG stage=scan-poll-timeout count=%u\n",
			 w4->scan_poll_count);
		cfg80211_scan_done(w4->scan_request, &info);
		w4->scan_request = NULL;
		goto out;
	}
	mod_delayed_work(system_wq, &w4->scan_poll,
		msecs_to_jiffies(W4_SCAN_POLL_INTERVAL_MS));
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
		w4->bt_paldo, w4->wifi_paldo, w4->portal);
	if (!ret) w4->w1_retained = true;
	return ret;
}
static void w4_stop_locked(struct mt6797_wifi_w4 *w4)
{
	if (!w4->opened) return;
	w4->join_state = W4_JOIN_IDLE;
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	netif_tx_stop_all_queues(w4->ndev);
	mt6797_wifi_ahb_w3_session_stop(&w4->session);
	/* Set before releasing the lock: a queued worker will only observe stop. */
	w4->opened = false;
	mt6797_consys_runtime_controller_wifi_session_put(&w4->runtime);
	if (w4->w1_retained) mt6797_wifi_ahb_w1_firmware_stop_retained(w4->wifi_paldo);
	w4->w1_retained = false;
	if (w4->controller_started) mt6797_consys_runtime_controller_stop(w4->controller_dev);
	w4->controller_started = false;
	if (w4->retained_owner_dev && !w4->retained_owner_released) {
		int ret = mt6797_consys_g2d_postpatch_owner_w4_release(
			w4->retained_owner_dev);
		if (ret)
			dev_err(w4->dev, "W4 retained owner teardown failed: %d\n", ret);
		else
			w4->retained_owner_released = true;
	}
}
static int w4_open(struct net_device *ndev)
{
	struct mt6797_wifi_w4 *w4 = w4_ndev(ndev);
	int ret;
	mutex_lock(&w4->lock);
	if (w4->opened) { ret = 0; goto out; }
	if (!mt6797_wifi_ahb_w3_session_enabled() ||
	    !device_property_read_bool(w4->dev,
		"mediatek,experimental-wifi-w3-session")) {
		ret = -EPERM;
		goto out;
	}
	ret = mt6797_consys_runtime_controller_start_with_postpatch_stage(
		w4->controller_dev, w4_firmware_stage, w4);
	if (ret) goto out;
	w4->controller_started = true;
	ret = mt6797_consys_runtime_controller_wifi_session_get(w4->controller_dev,
		&w4->runtime, &w4->stp_session);
	if (ret) goto fail;
	ret = mt6797_wifi_ahb_w3_session_start(&w4->session, &w4_hif_ops, w4);
	if (ret) { mt6797_consys_runtime_controller_wifi_session_put(&w4->runtime); goto fail; }
	w4_diag_hif(w4, "hif-ready");
	w4->opened = true;
	netif_tx_start_all_queues(ndev);
	dev_info(w4->dev, "W4 retained HIF session live: irq=%d whisr=%#x\n",
		w4->irq, w4->session.last_whisr);
	goto out;
fail:
	if (w4->w1_retained) mt6797_wifi_ahb_w1_firmware_stop_retained(w4->wifi_paldo);
	w4->w1_retained = false;
	mt6797_consys_runtime_controller_stop(w4->controller_dev);
	w4->controller_started = false;
	if (w4->retained_owner_dev && !w4->retained_owner_released) {
		int cleanup = mt6797_consys_g2d_postpatch_owner_w4_release(
			w4->retained_owner_dev);
		if (!cleanup)
			w4->retained_owner_released = true;
		else
			dev_err(w4->dev, "W4 retained owner abort teardown failed: %d\n",
				cleanup);
	}
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
	cancel_delayed_work_sync(&w4->scan_poll);
	cancel_delayed_work_sync(&w4->join_timeout);
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

	if (!READ_ONCE(w4->opened) || READ_ONCE(w4->join_state) != W4_JOIN_CONNECTED ||
	    length > W4_RX_MAX)
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
	frame[4] = READ_ONCE(w4->wlan_index);             /* AP STA-record WTBL index */
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
static int w4_send_open_sta(struct mt6797_wifi_w4 *w4)
{
	u8 cmd[8 + 124] = { };
	/* CMD_BSS_ACTIVATE_CTRL, vendor nic_cmd_event.h:899. */
	put_unaligned_le16(8 + 12, cmd); put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = 0x11; cmd[5] = 0x20; cmd[6] = 1; cmd[7] = 2;
	cmd[8] = 0; cmd[9] = 1; cmd[10] = 0; cmd[11] = 0;
	ether_addr_copy(cmd + 12, w4->ndev->dev_addr); cmd[18] = 31;
	if (w4_cmd53(w4, true, W4_WTDR1, cmd, 20)) return -EIO;
	/* CMD_UPDATE_STA_RECORD_T: vendor state-1 allocation before Auth-1.
	 * The 124-byte body is the exact 3.18 layout; its WLAN indices are at
	 * offsets 54/55, not the earlier shortened-command offsets.
	 */
	memset(cmd, 0, sizeof(cmd));
	put_unaligned_le16(sizeof(cmd), cmd); put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = 0x13; cmd[5] = 0x20; cmd[6] = 1; cmd[7] = 3;
	cmd[8] = w4->sta_index; cmd[9] = BIT(0) | BIT(6); /* legacy AP */
	ether_addr_copy(cmd + 10, w4->candidate_bssid);
	cmd[20] = 0;                  /* AIS BSS index */
	cmd[28] = 1;                  /* STA_STATE_1 */
	cmd[62] = w4->wlan_index;     /* ucWlanIndex */
	cmd[63] = W4_WLAN_INDEX_DEFAULT; /* ucBMCWlanIndex */
	return w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
}
/* The management descriptors follow nic_tx.c's 802.11-normal, long format. */
static int w4_send_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len)
{
	const unsigned int txd_len = 28;
	const unsigned int wire_len = txd_len + len;
	const unsigned int total = ALIGN(wire_len, sizeof(u32));
	u8 *frame;
	int ret;

	if (wire_len > W4_RX_MAX)
		return -EMSGSIZE;
	frame = kzalloc(total, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	put_unaligned_le16(wire_len, frame);
	frame[3] = BIT(7) | BIT(3); /* TC4: MCU port, queue one */
	frame[4] = w4->wlan_index;
	frame[5] = BIT(7) | (2 << 5) | (IEEE80211_HDRLEN >> 1);
	frame[8] = ((mgmt[0] >> 4) & 0xf) | ((mgmt[0] & 0x0c) << 2);
	frame[10] = 0xff;
	put_unaligned_le16(30, frame + 12);
	memcpy(frame + txd_len, mgmt, len);
	ret = w4_cmd53(w4, true, W4_WTDR1, frame, total);
	kfree(frame);
	return ret;
}

static void w4_mgmt_header(struct mt6797_wifi_w4 *w4, u8 *frame, u16 fc)
{
	put_unaligned_le16(fc, frame);
	ether_addr_copy(frame + 4, w4->candidate_bssid);
	ether_addr_copy(frame + 10, w4->ndev->dev_addr);
	ether_addr_copy(frame + 16, w4->candidate_bssid);
}

static int w4_send_auth1(struct mt6797_wifi_w4 *w4)
{
	u8 frame[IEEE80211_HDRLEN + 6] = { };

	/* authComposeAuthFrameHeaderAndFF(): open-system transaction sequence 1. */
	w4_mgmt_header(w4, frame, IEEE80211_STYPE_AUTH);
	put_unaligned_le16(0, frame + IEEE80211_HDRLEN);
	put_unaligned_le16(1, frame + IEEE80211_HDRLEN + 2);
	return w4_send_mgmt(w4, frame, sizeof(frame));
}

static int w4_send_assoc1(struct mt6797_wifi_w4 *w4)
{
	u8 frame[IEEE80211_HDRLEN + 4 + IEEE80211_MAX_SSID_LEN + 16] = { };
	u8 *ie = frame + IEEE80211_HDRLEN + 4;
	static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96,
		0x0c, 0x18, 0x30, 0x48, 0x60, 0x6c };
	size_t len;

	/* assocComposeReAssocReqFrameHeaderAndFF(), non-reassociation case. */
	w4_mgmt_header(w4, frame, IEEE80211_STYPE_ASSOC_REQ);
	put_unaligned_le16(WLAN_CAPABILITY_ESS, frame + IEEE80211_HDRLEN);
	put_unaligned_le16(10, frame + IEEE80211_HDRLEN + 2);
	*ie++ = WLAN_EID_SSID;
	*ie++ = w4->join_ssid_len;
	memcpy(ie, w4->join_ssid, w4->join_ssid_len);
	ie += w4->join_ssid_len;
	*ie++ = WLAN_EID_SUPP_RATES;
	*ie++ = sizeof(rates);
	memcpy(ie, rates, sizeof(rates));
	ie += sizeof(rates);
	len = ie - frame;
	w4->assoc_req_ie_len = len - IEEE80211_HDRLEN - 4;
	memcpy(w4->assoc_req_ies, frame + IEEE80211_HDRLEN + 4,
	       w4->assoc_req_ie_len);
	return w4_send_mgmt(w4, frame, len);
}

static int w4_send_sta_state(struct mt6797_wifi_w4 *w4, u8 sta_state, u16 aid)
{
	u8 cmd[8 + 124] = { };

	put_unaligned_le16(sizeof(cmd), cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = 0x13; cmd[5] = 0x20; cmd[6] = 1; cmd[7] = 4;
	cmd[8] = w4->sta_index; cmd[9] = BIT(0) | BIT(6);
	ether_addr_copy(cmd + 10, w4->candidate_bssid);
	put_unaligned_le16(aid, cmd + 16);
	cmd[20] = 0;
	cmd[28] = sta_state;          /* state 2 after Auth-2; state 3 after Assoc-2 */
	cmd[62] = w4->wlan_index;
	cmd[63] = W4_WLAN_INDEX_DEFAULT;
	return w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
}

static void w4_join_timeout(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(to_delayed_work(work),
		struct mt6797_wifi_w4, join_timeout);
	enum nl80211_timeout_reason reason;

	mutex_lock(&w4->lock);
	if (w4->join_state != W4_JOIN_WAIT_AUTH2 &&
	    w4->join_state != W4_JOIN_WAIT_ASSOC2)
		goto out;
	reason = w4->join_state == W4_JOIN_WAIT_AUTH2 ?
		NL80211_TIMEOUT_AUTH : NL80211_TIMEOUT_ASSOC;
	w4->join_state = W4_JOIN_IDLE;
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	cfg80211_connect_timeout(w4->ndev, w4->candidate_bssid,
		w4->assoc_req_ies, w4->assoc_req_ie_len, GFP_KERNEL, reason);
out:
	mutex_unlock(&w4->lock);
}

/* saaFsmRunEventRxAuth()/RxAssoc(), invoked from the bounded RX worker. */
static void w4_rx_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len)
{
	u16 fc, status;

	if (len < IEEE80211_HDRLEN || !ether_addr_equal(mgmt + 10, w4->candidate_bssid) ||
	    !ether_addr_equal(mgmt + 4, w4->ndev->dev_addr))
		return;
	fc = get_unaligned_le16(mgmt);
	if (w4->join_state == W4_JOIN_WAIT_AUTH2 && len >= IEEE80211_HDRLEN + 6 &&
	    (fc & 0x00fc) == IEEE80211_STYPE_AUTH &&
	    get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 2) == 2) {
		status = get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 4);
		/* saaFsmRunEventRxAuth synchronises STA_STATE_2 before Assoc-1. */
		if (status || w4_send_sta_state(w4, 2, 0) || w4_send_assoc1(w4)) {
			w4->join_state = W4_JOIN_IDLE;
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
			cancel_delayed_work(&w4->join_timeout);
			cfg80211_connect_result(w4->ndev, w4->candidate_bssid, NULL, 0,
				NULL, 0, status ?: WLAN_STATUS_UNSPECIFIED_FAILURE, GFP_KERNEL);
			return;
		}
		w4->join_state = W4_JOIN_WAIT_ASSOC2;
		mod_delayed_work(system_wq, &w4->join_timeout,
			msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
		return;
	}
	if (w4->join_state == W4_JOIN_WAIT_ASSOC2 && len >= IEEE80211_HDRLEN + 6 &&
	    (fc & 0x00fc) == IEEE80211_STYPE_ASSOC_RESP) {
		status = get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 2);
		cancel_delayed_work(&w4->join_timeout);
		if (!status && w4_send_sta_state(w4, 3,
			get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 4) & 0x3fff))
			status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		w4->join_state = status ? W4_JOIN_IDLE : W4_JOIN_CONNECTED;
		if (status)
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		cfg80211_connect_result(w4->ndev, w4->candidate_bssid,
			w4->assoc_req_ies, w4->assoc_req_ie_len,
			mgmt + IEEE80211_HDRLEN + 6, len - IEEE80211_HDRLEN - 6,
			status, GFP_KERNEL);
	}
}
static int w4_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;
	mutex_lock(&w4->lock);
	if (!w4->opened || w4->scan_request) { mutex_unlock(&w4->lock); return -EBUSY; }
	w4_diag_hif(w4, "scan-pre");
	ret = w4_send_scan(w4, request);
	dev_info(w4->dev,
		 "W4_DIAG stage=scan-submit ret=%d channels=%u ssids=%u\n",
		 ret, request->n_channels, request->n_ssids);
	if (ret) { mutex_unlock(&w4->lock); return ret; }
	w4->scan_request = request;
	w4->scan_poll_count = 0;
	w4->scan_poll_deadline = jiffies + msecs_to_jiffies(W4_SCAN_POLL_TIMEOUT_MS);
	w4_diag_hif(w4, "scan-post");
	mod_delayed_work(system_wq, &w4->scan_poll,
		msecs_to_jiffies(W4_SCAN_POLL_INTERVAL_MS));
	mutex_unlock(&w4->lock);
	return 0;
}
static int w4_connect(struct wiphy *wiphy, struct net_device *ndev,
	struct cfg80211_connect_params *sme)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);

	if (!sme->ssid || sme->ssid_len > 32 || sme->key || sme->crypto.n_akm_suites)
		return -EOPNOTSUPP; /* bounded first slice: open-network AIS only */
	mutex_lock(&w4->lock);
	if (!w4->opened || !w4->candidate_valid ||
	    (sme->bssid && !ether_addr_equal(sme->bssid, w4->candidate_bssid))) {
		mutex_unlock(&w4->lock);
		return -ENETUNREACH;
	}
	if (w4->join_state == W4_JOIN_WAIT_AUTH2 ||
	    w4->join_state == W4_JOIN_WAIT_ASSOC2) {
		mutex_unlock(&w4->lock);
		return -EBUSY;
	}
	/* secPrivacySeekForEntry() uses the first free unicast WTBL entry (zero).
	 * 31 remains the vendor broadcast/default entry; it must not carry AP TX.
	 */
	w4->sta_index = W4_STA_INDEX_AIS_AP;
	w4->wlan_index = W4_STA_INDEX_AIS_AP;
	w4->join_ssid_len = sme->ssid_len;
	memcpy(w4->join_ssid, sme->ssid, w4->join_ssid_len);
	w4->assoc_req_ie_len = 0;
	if (w4_send_open_sta(w4) || w4_send_auth1(w4)) {
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		mutex_unlock(&w4->lock);
		return -EIO;
	}
	w4->join_state = W4_JOIN_WAIT_AUTH2;
	mod_delayed_work(system_wq, &w4->join_timeout,
		msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
	mutex_unlock(&w4->lock);
	return 0;
}
static int w4_disconnect(struct wiphy *wiphy, struct net_device *ndev, u16 reason)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	bool connected;

	mutex_lock(&w4->lock);
	connected = w4->join_state == W4_JOIN_CONNECTED;
	w4->join_state = W4_JOIN_IDLE;
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	mutex_unlock(&w4->lock);
	cancel_delayed_work_sync(&w4->join_timeout);
	if (connected && netif_running(ndev))
		cfg80211_disconnected(ndev, reason, NULL, 0, true, GFP_KERNEL);
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
	if (!READ_ONCE(enable_experimental_wifi_w4_platform) ||
	    !device_property_read_bool(&pdev->dev,
		"mediatek,experimental-wifi-w4-platform"))
		return -EPERM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != MT6797_WIFI_AHB_HIF_BASE ||
	    resource_size(res) != MT6797_WIFI_AHB_HIF_PORTAL_SIZE) return -EINVAL;
	w4 = devm_kzalloc(&pdev->dev, sizeof(*w4), GFP_KERNEL); if (!w4) return -ENOMEM;
	w4->dev = &pdev->dev; mutex_init(&w4->lock); INIT_WORK(&w4->rx_work, w4_rx_work);
	INIT_DELAYED_WORK(&w4->join_timeout, w4_join_timeout);
	INIT_DELAYED_WORK(&w4->scan_poll, w4_scan_poll);
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	w4->portal = devm_ioremap_resource(&pdev->dev, res); if (IS_ERR(w4->portal)) return PTR_ERR(w4->portal);
	w4->irq = platform_get_irq_byname(pdev, "wifi"); if (w4->irq < 0) return w4->irq;
	{
		u32 spec[3] = { };
		int spec_ret = of_property_read_u32_array(pdev->dev.of_node,
			"interrupts", spec, ARRAY_SIZE(spec));
		dev_info(&pdev->dev,
			 "W4_DIAG stage=irq-route irq=%d trigger=%#x spec-ret=%d spec=%#x/%#x/%#x\n",
			 w4->irq, irq_get_trigger_type(w4->irq), spec_ret,
			 spec[0], spec[1], spec[2]);
	}
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0); if (!node) return -EINVAL;
	controller = of_find_device_by_node(node); of_node_put(node); if (!controller) return -EPROBE_DEFER;
	w4->controller_dev = &controller->dev;
	/*
	 * G2d is the one matching true-chronology owner in this profile.  Do not
	 * encode this as a DT phandle: fw_devlink treats that vendor reference as
	 * a supplier edge while G2d is still resolving its controller suppliers,
	 * creating a bind-time cycle.  The explicit device_link below is created
	 * only after the fully bound G2d owner has published its retained state.
	 */
	node = of_find_compatible_node(NULL, NULL,
		"mediatek,mt6797-consys-g2d-postpatch-owner");
	if (!node) { ret = -EINVAL; goto put_controller; }
	controller = of_find_device_by_node(node); of_node_put(node);
	if (!controller) { ret = -EPROBE_DEFER; goto put_controller; }
	w4->retained_owner_dev = &controller->dev;
	ret = mt6797_consys_g2d_postpatch_owner_w4_validate(w4->retained_owner_dev);
	if (ret) goto put_owner;
	if (!device_link_add(&pdev->dev, w4->retained_owner_dev,
		DL_FLAG_AUTOREMOVE_CONSUMER)) { ret = -ENOMEM; goto put_owner; }
	w4->bt_paldo = devm_regulator_get(&pdev->dev, "bt-paldo");
	if (IS_ERR(w4->bt_paldo)) { ret = PTR_ERR(w4->bt_paldo); goto put_owner; }
	w4->wifi_paldo = devm_regulator_get(&pdev->dev, "wifi-paldo");
	if (IS_ERR(w4->wifi_paldo)) { ret = PTR_ERR(w4->wifi_paldo); goto put_owner; }
	ret = devm_request_threaded_irq(&pdev->dev, w4->irq, NULL, w4_irq_thread,
		IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(&pdev->dev), w4);
	if (ret) goto put_owner;
	w4->wiphy = wiphy_new(&w4_cfg80211_ops, sizeof(struct mt6797_wifi_w4 *));
	if (!w4->wiphy) { ret = -ENOMEM; goto put_owner; }
	*(struct mt6797_wifi_w4 **)wiphy_priv(w4->wiphy) = w4;
	w4->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	/* iw's ordinary wildcard probe is one SSID entry; zero makes nl80211
	 * reject it before ->scan() is reached.  CMD_SCAN_REQ implements one. */
	w4->wiphy->max_scan_ssids = 1;
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
	/*
	 * This probe does not run under RTNL or the wiphy mutex.  Let the normal
	 * netdev notifier establish the wireless_dev instead of calling the
	 * cfg80211-only registration helper, which explicitly requires both locks.
	 */
	ret = register_netdev(w4->ndev); if (ret) goto free_wdev;
	platform_set_drvdata(pdev, w4);
	dev_info(&pdev->dev, "GEN3 HIF owner registered %s\n", w4->ndev->name);
	return 0;
free_wdev: kfree(w4->wdev);
free_netdev: free_netdev(w4->ndev);
unregister_wiphy: wiphy_unregister(w4->wiphy);
free_wiphy: wiphy_free(w4->wiphy);
put_owner: put_device(w4->retained_owner_dev);
put_controller: put_device(w4->controller_dev); return ret;
}
static void mt6797_wifi_w4_remove(struct platform_device *pdev)
{
	struct mt6797_wifi_w4 *w4 = platform_get_drvdata(pdev);
	if (!w4) return;
	cancel_delayed_work_sync(&w4->scan_poll);
	/* unregister_netdev() takes RTNL and drives cfg80211's notifier teardown. */
	unregister_netdev(w4->ndev); kfree(w4->wdev); free_netdev(w4->ndev);
	wiphy_unregister(w4->wiphy); wiphy_free(w4->wiphy);
	mutex_lock(&w4->lock);
	if (!w4->retained_owner_released) {
		mt6797_consys_runtime_controller_stop(w4->controller_dev);
		if (!mt6797_consys_g2d_postpatch_owner_w4_release(w4->retained_owner_dev))
			w4->retained_owner_released = true;
	}
	mutex_unlock(&w4->lock);
	put_device(w4->retained_owner_dev); put_device(w4->controller_dev);
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
