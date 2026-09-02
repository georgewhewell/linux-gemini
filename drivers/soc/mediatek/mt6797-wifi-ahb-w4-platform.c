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

#include "mt6797-wifi-gen3-wire.h"

#define W4_WHIER 0x0014
#define W4_WRDR0 0x0050
#define W4_WRDR1 0x0054
#define W4_WTDR1 0x0034
#define W4_WRPLR 0x0090
#define W4_WTQCR0 0x0130
#define W4_WHIER_DEFAULT 0xffffff47U
#define W4_RX_PKT_TYPE_SW_MASK 0xe00fU
#define W4_RX_PKT_TYPE_SW_EVENT 0xe000U
#define W4_RX_PKT_TYPE_SW_FRAME 0xe001U
#define W4_WHCR_RX_ENHANCE_MODE_EN BIT(16)
#define W4_WHCR_MAX_HIF_RX_LEN_NUM 0x00003f00U
#define W4_WHCR_RECV_MAILBOX_RD_CLR_EN BIT(2)
#define W4_WHCR_W_INT_CLR_CTRL BIT(1)
#define W4_RX_MAX 2352U
#define W4_TX_DESC_LEN 28U
#define W4_RX_BUDGET 32U
#define W4_RX_DESCRIPTOR_MIN 16U
#define W4_WIFI_EVENT_HEADER_LEN 8U
#define W4_EVENT_ID_SCAN_DONE 0x0dU
#define W4_EVENT_ID_CH_PRIVILEGE 0x10U
#define W4_SCAN_SEQUENCE 1U
#define W4_WIFI_CMD_HEADER_LEN 8U
#define W4_CMD_ID_BASIC_CONFIG 0x02U
#define W4_CMD_ID_SCAN_REQ_V2 0x03U
#define W4_CMD_ID_BSS_ACTIVATE_CTRL 0x11U
#define W4_CMD_ID_UPDATE_STA_RECORD 0x13U
#define W4_CMD_ID_CH_PRIVILEGE 0x1cU
/* nicTxCmd() writes CMD_PACKET_TYPE_ID; the legacy 0x20 header comment in
 * the vendor source does not describe the active command path.
 */
#define W4_CMD_PKT_TYPE 0xa0U
#define W4_SCAN_SSID_SLOT_LEN 36U
#define W4_SCAN_REQ_V2_IE_OFFSET 226U
#define W4_SCAN_MAX_IE_LEN 600U
#define W4_SCAN_REQ_V2_BASE_LEN \
	(W4_WIFI_CMD_HEADER_LEN + W4_SCAN_REQ_V2_IE_OFFSET)
#define W4_SCAN_REQ_V2_SSID0_OFFSET (W4_WIFI_CMD_HEADER_LEN + 8U)
#define W4_SCAN_REQ_V2_TIMER_OFFSET (W4_SCAN_REQ_V2_SSID0_OFFSET + 4U * W4_SCAN_SSID_SLOT_LEN)
#define W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET (W4_SCAN_REQ_V2_TIMER_OFFSET + 6U)
#define W4_SCAN_REQ_V2_CHANNEL_LIST_OFFSET (W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET + 2U)
#define W4_SCAN_REQ_V2_IE_LEN_OFFSET \
	(W4_SCAN_REQ_V2_BASE_LEN - sizeof(u16))
#define W4_SCAN_CHANNEL_2G4 1U
#define W4_SCAN_CHANNEL_SPECIFIED 4U
#define W4_RF_BAND_2G4 1U
#define W4_CMD_SEQUENCE_BASIC_CONFIG 1U
#define W4_CMD_SEQUENCE_ACTIVATE 2U
#define W4_CMD_SEQUENCE_SCAN 3U
#define W4_CMD_SEQUENCE_CHANNEL 4U
#define W4_EVENT_ID_DEBUG_MSG 0x27U
#define W4_DEBUG_MSG_HEADER_LEN 12U
#define IEEE80211_HDRLEN 24U
#define W4_WLAN_INDEX_DEFAULT 31U
#define W4_BMC_WLAN_INDEX_AIS 0U
#define W4_WLAN_INDEX_AIS_AP 1U
#define W4_STA_BMC_WLAN_INDEX_AIS_AP 2U
#define W4_STA_INDEX_AIS_AP 0U
#define W4_STA_STATE_1 0U
#define W4_STA_STATE_2 1U
#define W4_STA_STATE_3 2U
#define W4_PHY_TYPE_HR_DSSS BIT(0)
#define W4_PHY_TYPE_ERP BIT(1)
#define W4_PHY_TYPE_HR_DSSS_INDEX 0U
#define W4_PHY_TYPE_ERP_INDEX 1U
#define W4_RATE_SET_HR_DSSS GENMASK(3, 0)
#define W4_RATE_SET_OFDM GENMASK(13, 6)
#define W4_RATE_SET_ERP (W4_RATE_SET_HR_DSSS | W4_RATE_SET_OFDM)
#define W4_RATE_OFDM_6M 0x4bU
#define W4_JOIN_TIMEOUT_MS 1000U
#define W4_CHANNEL_TIMEOUT_MS 4500U
#define W4_CHANNEL_TOKEN 1U
#define W4_CHANNEL_REQUEST_INTERVAL_MS 4000U
#define W4_ASSOC_IE_MAX \
	(W4_RX_MAX - W4_TX_DESC_LEN - IEEE80211_HDRLEN - 4U)
#define W4_SCAN_POLL_INTERVAL_MS 20U
#define W4_SCAN_POLL_TIMEOUT_MS 30000U
#define W4_RX_STATUS_ERROR_MASK (BIT(1) | BIT(3) | BIT(4))

static bool enable_experimental_wifi_w4_platform;
module_param_named(enable_experimental_wifi_w4_platform,
		enable_experimental_wifi_w4_platform, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w4_platform,
	"allow MT6797 W4 retained WLAN platform ownership (default: false)");

/* The vendor's AIS -> SAA chronology with cfg80211 host MLME. */
enum w4_join_state {
	W4_JOIN_IDLE,
	W4_JOIN_WAIT_CHANNEL,
	W4_JOIN_AUTHENTICATING,
	W4_JOIN_ASSOCIATING,
	W4_JOIN_CONNECTED,
};

struct w4_bss_profile {
	u16 capability;
	u16 basic_rate_set;
	u16 desired_rate_set;
	u8 desired_phy_type_set;
	u8 non_ht_basic_phy_type;
	u8 hw_default_rate_code;
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
	u8 command_sequence;
	u8 join_ssid[IEEE80211_MAX_SSID_LEN];
	u8 join_ssid_len;
	u8 assoc_req_ies[W4_ASSOC_IE_MAX];
	u8 pending_auth[W4_RX_MAX - W4_TX_DESC_LEN];
	u16 assoc_req_ie_len;
	u16 pending_auth_len;
	u16 candidate_capability;
	u16 basic_rate_set;
	u16 desired_rate_set;
	u8 desired_phy_type_set;
	u8 non_ht_basic_phy_type;
	u8 hw_default_rate_code;
	u8 group_key_bitmap;
	u8 mgmt_key_bitmap;
	unsigned int scan_poll_count;
	unsigned long scan_poll_deadline;
	enum w4_join_state join_state;
	enum nl80211_auth_type auth_type;
	struct cfg80211_bss *assoc_bss;
	bool use_mfp;
	bool candidate_valid, retained_owner_released, channel_requested;
	bool controller_started, w1_retained, opened, pairwise_key_installed;
	bool protected_network, port_authorized, control_port_no_encrypt;
};

static const u32 w4_cipher_suites[] = {
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

static struct ieee80211_channel w4_channels[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2484, .hw_value = 14 },
};
static struct ieee80211_rate w4_rates[] = {
	{ .bitrate = 10 }, { .bitrate = 20 }, { .bitrate = 55 }, { .bitrate = 110 },
	{ .bitrate = 60 }, { .bitrate = 90 }, { .bitrate = 120 }, { .bitrate = 180 },
	{ .bitrate = 240 }, { .bitrate = 360 }, { .bitrate = 480 }, { .bitrate = 540 },
};
static struct ieee80211_supported_band w4_band = {
	.channels = w4_channels,
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
			ALIGN(len, sizeof(u32)) + sizeof(u32));
	kfree(buf);
	return ret;
}
static void w4_rx_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len);
static int w4_send_bss_info(struct mt6797_wifi_w4 *w4, bool connected);
static int w4_send_open_sta(struct mt6797_wifi_w4 *w4);
static int w4_send_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len);

static int w4_remove_key(struct mt6797_wifi_w4 *w4, bool pairwise,
			 u8 key_index)
{
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	u8 wlan_index = pairwise ? W4_WLAN_INDEX_AIS_AP :
		W4_STA_BMC_WLAN_INDEX_AIS_AP;
	int ret;

	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd),
					     ++w4->command_sequence, false,
					     pairwise, w4->candidate_bssid,
					     key_index, wlan_index,
					     MT6797_WIFI_GEN3_CIPHER_NONE,
					     NULL, 0, NULL, 0);
	if (ret < 0)
		return ret;
	return w4_cmd53(w4, true, W4_WTDR1, cmd, ret);
}

static void w4_purge_keys(struct mt6797_wifi_w4 *w4)
{
	u8 installed = w4->group_key_bitmap | w4->mgmt_key_bitmap;
	unsigned int i;

	if (w4->pairwise_key_installed && w4_remove_key(w4, true, 0))
		dev_warn(w4->dev, "failed to remove pairwise key\n");
	for (i = 0; i <= 5; i++)
		if ((installed & BIT(i)) && w4_remove_key(w4, false, i))
			dev_warn(w4->dev, "failed to remove group key %u\n", i);
	w4->pairwise_key_installed = false;
	w4->group_key_bitmap = 0;
	w4->mgmt_key_bitmap = 0;
}

static int w4_send_channel_request(struct mt6797_wifi_w4 *w4)
{
	u8 cmd[W4_WIFI_CMD_HEADER_LEN + 16] = { };
	int ret;

	put_unaligned_le16(sizeof(cmd), cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_CH_PRIVILEGE;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;
	cmd[7] = W4_CMD_SEQUENCE_CHANNEL;
	cmd[8] = 0;
	cmd[9] = W4_CHANNEL_TOKEN;
	cmd[10] = 0; /* CMD_CH_ACTION_REQ */
	cmd[11] = w4->candidate_channel;
	cmd[12] = 0; /* CHNL_EXT_SCN */
	cmd[13] = W4_RF_BAND_2G4;
	cmd[14] = 0; /* CW_20_40MHZ */
	cmd[17] = 0; /* CH_REQ_TYPE_JOIN */
	put_unaligned_le32(W4_CHANNEL_REQUEST_INTERVAL_MS, cmd + 20);
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
	if (!ret)
		w4->channel_requested = true;
	return ret;
}

static void w4_release_channel(struct mt6797_wifi_w4 *w4)
{
	u8 cmd[W4_WIFI_CMD_HEADER_LEN + 16] = { };
	int ret;

	if (!w4->channel_requested)
		return;
	put_unaligned_le16(sizeof(cmd), cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_CH_PRIVILEGE;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;
	cmd[7] = W4_CMD_SEQUENCE_CHANNEL;
	cmd[8] = 0;
	cmd[9] = W4_CHANNEL_TOKEN;
	cmd[10] = 1; /* CMD_CH_ACTION_ABORT */
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
	w4->channel_requested = false;
	if (ret)
		dev_warn(w4->dev, "failed to release join channel: %d\n", ret);
}
/* WIFI_EVENT_T is an eight-byte HIF event header, followed by event body.
 * Vendor nicRxProcessEventPacket() consumes the whole record before looking at
 * its EID; the legacy scan-done body is only a four-byte EVENT_SCAN_DONE_T.
 */
static int w4_rx_event(struct mt6797_wifi_w4 *w4, u16 len, const u8 *buf)
{
	u16 packet_length;
	u8 event_id, event_sequence;

	packet_length = get_unaligned_le16(buf);
	if (packet_length < W4_WIFI_EVENT_HEADER_LEN || packet_length > len)
		return -EPROTO;
	event_id = buf[4];
	event_sequence = buf[5];
	dev_info(w4->dev,
		 "W4_DIAG stage=rx-event len=%u packet-len=%u eid=%#x event-seq=%u\n",
		 len, packet_length, event_id, event_sequence);
	if (event_id == W4_EVENT_ID_DEBUG_MSG) {
		const u8 *debug = buf + W4_WIFI_EVENT_HEADER_LEN;
		u16 message_size;
		size_t available;

		if (packet_length < W4_WIFI_EVENT_HEADER_LEN + W4_DEBUG_MSG_HEADER_LEN)
			return -EPROTO;
		message_size = get_unaligned_le16(debug + 8);
		available = packet_length - W4_WIFI_EVENT_HEADER_LEN -
			W4_DEBUG_MSG_HEADER_LEN;
		/* Firmware diagnostics have historically included temporal key
		 * material.  Keep bounded metadata for fault diagnosis, but never
		 * forward an opaque firmware payload into the kernel log.
		 */
		dev_dbg(w4->dev,
			"W4_DIAG stage=fw-debug id=%u type=%u declared=%u available=%zu\n",
			 get_unaligned_le16(debug), debug[2], message_size, available);
		return 0;
	}
	if (event_id == W4_EVENT_ID_CH_PRIVILEGE) {
		const u8 *grant = buf + W4_WIFI_EVENT_HEADER_LEN;
		int ret;

		if (packet_length < W4_WIFI_EVENT_HEADER_LEN + 16)
			return -EPROTO;
		if (w4->join_state != W4_JOIN_WAIT_CHANNEL ||
		    grant[0] != 0 || grant[1] != W4_CHANNEL_TOKEN)
			return 0;
		if (grant[2] || grant[3] != w4->candidate_channel ||
		    grant[5] != W4_RF_BAND_2G4 || grant[9] != 0) {
			ret = -EIO;
		} else {
			ret = w4_send_open_sta(w4);
			if (!ret)
				ret = w4_send_mgmt(w4, w4->pending_auth,
						   w4->pending_auth_len);
		}
		if (ret) {
			w4_release_channel(w4);
			w4->join_state = W4_JOIN_IDLE;
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
			w4->pending_auth_len = 0;
			cancel_delayed_work(&w4->join_timeout);
			cfg80211_auth_timeout(w4->ndev, w4->candidate_bssid);
			return 0;
		}
		w4->pending_auth_len = 0;
		w4->join_state = W4_JOIN_AUTHENTICATING;
		mod_delayed_work(system_wq, &w4->join_timeout,
				 msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
		return 0;
	}
	if (event_id != W4_EVENT_ID_SCAN_DONE)
		return 0;
	/* scnEventScanDone() matches the scan command sequence in the body, not
	 * WIFI_EVENT_T.ucSeqNum.  Reading even a stale/malformed event is enough to
	 * release the RX0 queue; only a matching event completes this scan. */
	if (packet_length < W4_WIFI_EVENT_HEADER_LEN + 1)
		return -EPROTO;
	if (!w4->scan_request || buf[W4_WIFI_EVENT_HEADER_LEN] != W4_SCAN_SEQUENCE) {
		dev_info(w4->dev,
			 "W4_DIAG stage=scan-done-ignore body-seq=%u pending=%u\n",
			 buf[W4_WIFI_EVENT_HEADER_LEN], !!w4->scan_request);
		return 0;
	}
	{
		struct cfg80211_scan_info info = { };

		cfg80211_scan_done(w4->scan_request, &info);
		w4->scan_request = NULL;
	}
	dev_info(w4->dev, "W4_DIAG stage=scan-done body-seq=%u\n",
		 buf[W4_WIFI_EVENT_HEADER_LEN]);
	return 0;
}
/* nicRxFillRFB() distilled for header-translated Ethernet frames. */
static int w4_rx_deliver(struct mt6797_wifi_w4 *w4, unsigned int port, u16 len)
{
	u8 *buf;
	u16 byte_count, type;
	u16 status;
	unsigned int off = W4_RX_DESCRIPTOR_MIN;
	u8 groups, header_len;
	struct sk_buff *skb;
	int ret;

	if (!len || len > W4_RX_MAX)
		return -EMSGSIZE;
	buf = kzalloc(W4_RX_MAX + MT6797_WIFI_AHB_SDIO_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = w4_cmd53(w4, false, port ? W4_WRDR1 : W4_WRDR0, buf,
			ALIGN(len, sizeof(u32)) + sizeof(u32));
	if (ret)
		goto out;
	/* SW-defined firmware events carry WIFI_EVENT_T directly, no RXD. */
	if (len >= W4_WIFI_EVENT_HEADER_LEN &&
	    get_unaligned_le16(buf + 2) == W4_RX_PKT_TYPE_SW_EVENT) {
		ret = w4_rx_event(w4, len, buf);
		goto out;
	}
	if (len < W4_RX_DESCRIPTOR_MIN) {
		ret = -EPROTO;
		goto out;
	}
	byte_count = get_unaligned_le16(buf);
	type = get_unaligned_le16(buf + 2);
	header_len = buf[6];
	status = get_unaligned_le16(buf + 10);
	groups = (type >> 9) & 0xf;
	if (byte_count > len || byte_count < off ||
	    (status & W4_RX_STATUS_ERROR_MASK))
		goto out;
	/* Vendor group order is 4,1,2,3 following HW_MAC_RX_DESC_T. */
	if (groups & BIT(3))
		off += 16;
	if (groups & BIT(0)) off += 16;
	if (groups & BIT(1)) off += 8;
	if (groups & BIT(2)) off += 24;
	if (header_len & BIT(6)) off += 2;
	if (off >= byte_count)
		goto out;
	/* Vendor RXM_RXD_PKT_TYPE_SW_FRAME is (u2PktTYpe & 0xe00f) == 0xe001.
	 * Beacon/probe responses are SW-defined raw management records, not events. */
	if (!(header_len & BIT(7)) &&
	    (type & W4_RX_PKT_TYPE_SW_MASK) == W4_RX_PKT_TYPE_SW_FRAME &&
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
	if (!(header_len & BIT(7)) &&
	    (type & W4_RX_PKT_TYPE_SW_MASK) == W4_RX_PKT_TYPE_SW_FRAME) {
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
/* wlanUpdateBasicConfig(): CMD_BASIC_CONFIG has the eight-byte WIFI_CMD_T and
 * an eight-byte body: Native80211=0, reserved=0, RX/TX checksum masks=0. */
static int w4_send_basic_config(struct mt6797_wifi_w4 *w4)
{
	u8 cmd[W4_WIFI_CMD_HEADER_LEN + 8] = { };
	int ret;

	put_unaligned_le16(sizeof(cmd), cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_BASIC_CONFIG;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;
	cmd[7] = W4_CMD_SEQUENCE_BASIC_CONFIG;
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
	dev_info(w4->dev, "W4_DIAG stage=basic-config ret=%d cmd-seq=%u native80211=%u rx-csum=%u tx-csum=%u\n",
		 ret, cmd[7], cmd[8], get_unaligned_le16(cmd + 12),
		 get_unaligned_le16(cmd + 14));
	return ret;
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
	ret = w4_mcr_write(w4, W4_WHIER, W4_WHIER_DEFAULT);
	if (ret)
		return ret;
	/* wlanAdapterStart() issues this immediately after NIC capability, before
	 * network-address application and the later interrupt enable. */
	return w4_send_basic_config(w4);
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
	wiphy_lock(w4->wiphy);
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
	wiphy_unlock(w4->wiphy);
}

static bool w4_rx_poll_needed(struct mt6797_wifi_w4 *w4)
{
	return w4->join_state == W4_JOIN_WAIT_CHANNEL ||
		w4->join_state == W4_JOIN_AUTHENTICATING ||
		w4->join_state == W4_JOIN_ASSOCIATING ||
		w4->join_state == W4_JOIN_CONNECTED;
}

/* MT6797's host EINT does not reliably retrigger after scan completion. */
static void w4_scan_poll(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(to_delayed_work(work),
		struct mt6797_wifi_w4, scan_poll);
	unsigned int i;
	u16 rx0, rx1;
	int ret;

	wiphy_lock(w4->wiphy);
	mutex_lock(&w4->lock);
	if (!w4->opened || (!w4->scan_request && !w4_rx_poll_needed(w4)))
		goto out;
	if (w4->scan_request)
		w4->scan_poll_count++;
	for (i = 0; i < W4_RX_BUDGET; i++) {
		ret = w4_rx_lengths(w4, &rx0, &rx1);
		if (ret) {
			dev_err(w4->dev, "W4_DIAG stage=rx-poll ret=%d count=%u\n",
				ret, w4->scan_poll_count);
			break;
		}
		if (!rx0 && !rx1)
			break;
		if (rx0)
			w4_rx_deliver(w4, 0, rx0);
		if (rx1)
			w4_rx_deliver(w4, 1, rx1);
	}
	if (w4->scan_request &&
	    time_after_eq(jiffies, w4->scan_poll_deadline)) {
		struct cfg80211_scan_info info = { .aborted = true };

		w4_diag_hif(w4, "scan-poll-timeout");
		dev_info(w4->dev, "W4_DIAG stage=scan-poll-timeout count=%u\n",
			 w4->scan_poll_count);
		cfg80211_scan_done(w4->scan_request, &info);
		w4->scan_request = NULL;
	}
	if (w4->scan_request || w4_rx_poll_needed(w4))
		mod_delayed_work(system_wq, &w4->scan_poll,
				 msecs_to_jiffies(W4_SCAN_POLL_INTERVAL_MS));
out:
	mutex_unlock(&w4->lock);
	wiphy_unlock(w4->wiphy);
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
	w4_purge_keys(w4);
	if (w4->join_state == W4_JOIN_CONNECTED && w4_send_bss_info(w4, false))
		dev_warn(w4->dev, "failed to clear firmware BSS state\n");
	w4->join_state = W4_JOIN_IDLE;
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	w4->protected_network = false;
	w4->port_authorized = false;
	w4->control_port_no_encrypt = false;
	w4->use_mfp = false;
	w4->pending_auth_len = 0;
	w4_release_channel(w4);
	netif_carrier_off(w4->ndev);
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
	if (w4->retained_owner_released) {
		ret = -ESHUTDOWN;
		goto out;
	}
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
	w4->command_sequence = 4;
	w4->opened = true;
	netif_carrier_off(ndev);
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
	struct cfg80211_assoc_failure failure = { };

	wiphy_lock(w4->wiphy);
	mutex_lock(&w4->lock);
	if (w4->assoc_bss) {
		failure.bss[0] = w4->assoc_bss;
		w4->assoc_bss = NULL;
		cfg80211_assoc_failure(w4->ndev, &failure);
	}
	if (w4->scan_request) {
		struct cfg80211_scan_info info = { .aborted = true };
		cfg80211_scan_done(w4->scan_request, &info); w4->scan_request = NULL;
	}
	w4_stop_locked(w4);
	mutex_unlock(&w4->lock);
	wiphy_unlock(w4->wiphy);
	cancel_delayed_work_sync(&w4->scan_poll);
	cancel_delayed_work_sync(&w4->join_timeout);
	/* The worker takes the same mutex, so waiting while holding it deadlocks. */
	cancel_work_sync(&w4->rx_work);
	return 0;
}
static netdev_tx_t w4_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mt6797_wifi_w4 *w4 = w4_ndev(ndev);
	bool eapol = skb->protocol == htons(ETH_P_PAE);
	bool arp = skb->protocol == htons(ETH_P_ARP);
	bool protected;
	u8 *frame;
	const unsigned int txd_len = W4_TX_DESC_LEN;
	const unsigned int wire_length = txd_len + skb->len;
	const unsigned int length = ALIGN(wire_length, sizeof(u32));
	int ret;

	if (!READ_ONCE(w4->opened) || READ_ONCE(w4->join_state) != W4_JOIN_CONNECTED ||
	    length > W4_RX_MAX ||
	    (READ_ONCE(w4->protected_network) &&
	     (!READ_ONCE(w4->pairwise_key_installed) ||
	      !READ_ONCE(w4->port_authorized)) &&
	     skb->protocol != htons(ETH_P_PAE)))
		goto drop;
	/* ndo_start_xmit is atomic: bounded portal ownership, never sleep. */
	if (!mutex_trylock(&w4->lock))
		return NETDEV_TX_BUSY;
	frame = kzalloc(length, GFP_ATOMIC);
	if (!frame) { mutex_unlock(&w4->lock); return NETDEV_TX_BUSY; }
	/* This legacy association has no WMM IE and programs ucIsQoS=0, so the
	 * vendor queue manager selects TC5 (LMAC/AC4). EAPOL retains MCU/TC4.
	 */
	put_unaligned_le16(wire_length, frame);            /* u2TxByteCount */
	frame[2] = (ETH_HLEN - ETH_TLEN + txd_len) >> 1;  /* EtherType word */
	frame[3] = eapol ? BIT(7) | BIT(3) : 4U << 3;
	frame[4] = READ_ONCE(w4->wlan_index);             /* AP STA-record WTBL index */
	frame[5] = BIT(4) | BIT(7);                       /* Ethernet-II, long */
	protected = READ_ONCE(w4->protected_network) &&
		READ_ONCE(w4->pairwise_key_installed) &&
		(!eapol || !READ_ONCE(w4->control_port_no_encrypt));
	if (protected)
		frame[6] = BIT(7);                         /* protected frame */
	if (arp)
		frame[6] |= 6U << 4;                       /* vendor ARP priority */
	frame[7] = 0;                                     /* own-MAC zero */
	frame[10] = 0;                                    /* no lifetime limit */
	put_unaligned_le16(30U << 11, frame + 12);        /* retry count limit */
	if (eapol || arp) {
		frame[11] = BIT(7);                       /* fixed-rate enable */
		put_unaligned_le16(w4->hw_default_rate_code << 2, frame + 26);
	}
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
static int w4_activate_ais(struct mt6797_wifi_w4 *w4, u8 command_sequence)
{
	u8 cmd[W4_WIFI_CMD_HEADER_LEN + 12] = { };
	int ret;

	/* nicActivateNetwork() makes AIS BSS zero active before AIS asks SCN to
	 * scan.  The W4 scan path needs the same independent BSS activation. */
	put_unaligned_le16(sizeof(cmd), cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_BSS_ACTIVATE_CTRL;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;
	cmd[7] = command_sequence;
	cmd[8] = 0;                 /* AIS BSS index */
	cmd[9] = 1;                 /* active */
	cmd[10] = 0;                /* AIS network type */
	cmd[11] = 0;                /* own-MAC index */
	ether_addr_copy(cmd + 12, w4->ndev->dev_addr);
	/* nicActivateNetwork() allocates the first free WTBL entry for AIS BMC.
	 * On a fresh firmware session that is entry zero; the AP pairwise entry is
	 * allocated next.  Entry 31 is the no-peer/default TX sentinel and is not a
	 * usable BMC entry.
	 */
	cmd[18] = W4_BMC_WLAN_INDEX_AIS;
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
	dev_info(w4->dev, "W4_DIAG stage=ais-activate ret=%d cmd-seq=%u\n",
		 ret, command_sequence);
	return ret;
}
static int w4_send_scan(struct mt6797_wifi_w4 *w4,
			const struct cfg80211_scan_request *req)
{
	size_t command_len, wire_len;
	u8 *cmd;
	unsigned int i, n = min_t(unsigned int, req->n_channels, 32);
	u8 ssid_num = req->n_ssids && req->ssids[0].ssid_len ? 1 : 0;
	u32 ssid_len = ssid_num ? req->ssids[0].ssid_len : 0;
	u8 channel_type = n ? W4_SCAN_CHANNEL_SPECIFIED : W4_SCAN_CHANNEL_2G4;
	int ret;

	if (req->ie_len > W4_SCAN_MAX_IE_LEN)
		return -EMSGSIZE;
	command_len = W4_SCAN_REQ_V2_BASE_LEN + req->ie_len;
	wire_len = ALIGN(command_len, sizeof(u32));
	cmd = kzalloc(wire_len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	/* AIS emits WIFI_CMD_T + CMD_SCAN_REQ_V2, not the obsolete short request.
	 * Keep the full fixed 4-SSID/32-channel prefix through aucIE even with no
	 * optional IEs, exactly as scnSendScanReqV2()'s OFFSET_OF() length. */
	put_unaligned_le16(wire_len, cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_SCAN_REQ_V2;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;          /* set */
	cmd[7] = W4_CMD_SEQUENCE_SCAN;
	cmd[8] = W4_SCAN_SEQUENCE;
	cmd[9] = 0;          /* AIS BSS index */
	cmd[10] = 1;         /* active scan */
	cmd[11] = req->n_ssids ? BIT(2) : BIT(0);
	cmd[12] = ssid_num;
	put_unaligned_le32(ssid_len, cmd + W4_SCAN_REQ_V2_SSID0_OFFSET);
	if (ssid_len)
		memcpy(cmd + W4_SCAN_REQ_V2_SSID0_OFFSET + sizeof(ssid_len),
		       req->ssids[0].ssid, ssid_len);
	/* Vendor AIS uses zero for default probe delay, dwell and timeout. */
	put_unaligned_le16(0, cmd + W4_SCAN_REQ_V2_TIMER_OFFSET);
	put_unaligned_le16(0, cmd + W4_SCAN_REQ_V2_TIMER_OFFSET + 2);
	put_unaligned_le16(0, cmd + W4_SCAN_REQ_V2_TIMER_OFFSET + 4);
	cmd[W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET] = channel_type;
	cmd[W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET + 1] = n;
	for (i = 0; i < n; i++) {
		cmd[W4_SCAN_REQ_V2_CHANNEL_LIST_OFFSET + 2 * i] =
			req->channels[i]->band == NL80211_BAND_2GHZ ? 1 : 2;
		cmd[W4_SCAN_REQ_V2_CHANNEL_LIST_OFFSET + 2 * i + 1] =
			ieee80211_frequency_to_channel(req->channels[i]->center_freq);
	}
	put_unaligned_le16(req->ie_len, cmd + W4_SCAN_REQ_V2_IE_LEN_OFFSET);
	if (req->ie_len)
		memcpy(cmd + W4_SCAN_REQ_V2_BASE_LEN, req->ie, req->ie_len);
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, wire_len);
	dev_info(w4->dev,
		 "W4_DIAG stage=scan-cmd ret=%d cid=%#x tx-len=%zu wire-len=%zu cmd-seq=%u scan-seq=%u ssid-type=%#x ssid-num=%u ssid-len=%u ch-type=%u ch-count=%u ie-len=%zu\n",
		 ret, cmd[4], command_len, wire_len, cmd[7], cmd[8],
		 cmd[11], cmd[12], ssid_len, cmd[W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET],
		 cmd[W4_SCAN_REQ_V2_CHANNEL_TYPE_OFFSET + 1], req->ie_len);
	kfree(cmd);
	return ret;
}

/* rate.c's aucDataRate[] order is the firmware rate-set bit numbering. */
static const u8 w4_rate_codes[] = {
	0x02, 0x04, 0x0b, 0x16, 0x2c, 0x42, 0x0c,
	0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c,
};

static void w4_parse_rate_element(const struct element *elem, u16 *operational,
				  u16 *basic, bool *unknown_basic)
{
	unsigned int i, j;

	if (!elem)
		return;
	for (i = 0; i < elem->datalen; i++) {
		u8 rate = elem->data[i] & 0x7f;

		for (j = 0; j < ARRAY_SIZE(w4_rate_codes); j++)
			if (rate == w4_rate_codes[j])
				break;
		if (j == ARRAY_SIZE(w4_rate_codes)) {
			if (elem->data[i] & BIT(7))
				*unknown_basic = true;
			continue;
		}
		*operational |= BIT(j);
		if (elem->data[i] & BIT(7))
			*basic |= BIT(j);
	}
}

static u16 w4_default_rate_code(u16 basic_rate_set, u8 non_ht_phy)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(w4_rate_codes); i++)
		if (basic_rate_set & BIT(i))
			return i < 4 ? i : W4_RATE_OFDM_6M;
	return non_ht_phy == W4_PHY_TYPE_ERP_INDEX ? W4_RATE_OFDM_6M : 0;
}

static unsigned int w4_rate_set_to_codes(u16 rate_set, u16 basic_rate_set,
					 u8 *rates)
{
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(w4_rate_codes); i++)
		if (rate_set & BIT(i))
			rates[n++] = w4_rate_codes[i] |
				(basic_rate_set & BIT(i) ? BIT(7) : 0);
	return n;
}

static int w4_bss_profile_from_cfg80211(struct cfg80211_bss *bss,
					struct w4_bss_profile *profile)
{
	const struct element *supported, *extended, *erp;
	u16 operational = 0, basic = 0, supported_set;
	bool unknown_basic = false, has_erp;
	u8 phy_type_set = 0, non_ht_phy;

	if (!bss->channel || bss->channel->band != NL80211_BAND_2GHZ)
		return -EOPNOTSUPP;
	rcu_read_lock();
	supported = ieee80211_bss_get_elem(bss, WLAN_EID_SUPP_RATES);
	extended = ieee80211_bss_get_elem(bss, WLAN_EID_EXT_SUPP_RATES);
	erp = ieee80211_bss_get_elem(bss, WLAN_EID_ERP_INFO);
	w4_parse_rate_element(supported, &operational, &basic, &unknown_basic);
	w4_parse_rate_element(extended, &operational, &basic, &unknown_basic);
	has_erp = !!erp;
	rcu_read_unlock();
	if (!operational || unknown_basic)
		return -EOPNOTSUPP;
	if ((operational & W4_RATE_SET_OFDM) || has_erp)
		phy_type_set |= W4_PHY_TYPE_ERP;
	if (!(basic & W4_RATE_SET_OFDM) &&
	    (operational & W4_RATE_SET_HR_DSSS))
		phy_type_set |= W4_PHY_TYPE_HR_DSSS;
	if (!phy_type_set)
		return -EOPNOTSUPP;
	non_ht_phy = phy_type_set & W4_PHY_TYPE_ERP ?
		W4_PHY_TYPE_ERP_INDEX : W4_PHY_TYPE_HR_DSSS_INDEX;
	supported_set = non_ht_phy == W4_PHY_TYPE_ERP_INDEX ?
		W4_RATE_SET_ERP : W4_RATE_SET_HR_DSSS;
	profile->desired_rate_set = operational & supported_set;
	if (!profile->desired_rate_set ||
	    (basic & profile->desired_rate_set) != basic)
		return -EOPNOTSUPP;
	profile->capability = bss->capability;
	profile->basic_rate_set = basic;
	profile->desired_phy_type_set = phy_type_set;
	profile->non_ht_basic_phy_type = non_ht_phy;
	profile->hw_default_rate_code = w4_default_rate_code(basic, non_ht_phy);
	return 0;
}

static void w4_fill_sta_record(struct mt6797_wifi_w4 *w4, u8 *cmd,
			       u8 sequence, u8 sta_state, u16 aid)
{
	put_unaligned_le16(8 + 124, cmd);
	put_unaligned_le16(0x8000, cmd + 2);
	cmd[4] = W4_CMD_ID_UPDATE_STA_RECORD;
	cmd[5] = W4_CMD_PKT_TYPE;
	cmd[6] = 1;
	cmd[7] = sequence;
	cmd[8] = w4->sta_index;
	cmd[9] = BIT(0) | BIT(6);
	ether_addr_copy(cmd + 10, w4->candidate_bssid);
	put_unaligned_le16(aid, cmd + 16);
	if (sta_state == W4_STA_STATE_3)
		put_unaligned_le16(10, cmd + 18);
	cmd[20] = 0;
	cmd[21] = w4->desired_phy_type_set;
	put_unaligned_le16(w4->desired_rate_set, cmd + 22);
	put_unaligned_le16(w4->basic_rate_set, cmd + 24);
	cmd[28] = sta_state;
	cmd[62] = w4->wlan_index;
	cmd[63] = W4_STA_BMC_WLAN_INDEX_AIS_AP;
	put_unaligned_le16(w4->hw_default_rate_code, cmd + 82);
}

static int w4_send_open_sta(struct mt6797_wifi_w4 *w4)
{
	u8 cmd[8 + 124] = { };
	int ret;

	ret = w4_activate_ais(w4, W4_CMD_SEQUENCE_ACTIVATE);
	if (ret)
		return ret;
	w4_fill_sta_record(w4, cmd, 3, W4_STA_STATE_1, 0);
	return w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
}
/* The management descriptors follow nic_tx.c's 802.11-normal, long format. */
static int w4_send_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len)
{
	const unsigned int txd_len = W4_TX_DESC_LEN;
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
	frame[2] = (txd_len + IEEE80211_HDRLEN) >> 1;
	frame[3] = BIT(7) | BIT(3); /* TC4: MCU port, queue one */
	frame[4] = w4->wlan_index;
	frame[5] = BIT(7) | (2 << 5) | (IEEE80211_HDRLEN >> 1);
	if (get_unaligned_le16(mgmt) & IEEE80211_FCTL_PROTECTED)
		frame[6] = BIT(7); /* TX_DESC_PROTECTED_FRAME */
	frame[8] = ((mgmt[0] >> 4) & 0xf) | ((mgmt[0] & 0x0c) << 2);
	frame[10] = 0;
	frame[11] = BIT(7); /* nicTxSetPktLowestFixedRate() */
	put_unaligned_le16((30U << 11) | BIT(0), frame + 12);
	put_unaligned_le16(w4->hw_default_rate_code << 2, frame + 26);
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

static int w4_compose_auth(struct mt6797_wifi_w4 *w4,
			   const struct cfg80211_auth_request *req,
			   u8 *frame, size_t frame_size)
{
	u16 algorithm;
	size_t body_len, len;

	if (req->auth_type == NL80211_AUTHTYPE_OPEN_SYSTEM)
		algorithm = WLAN_AUTH_OPEN;
	else if (req->auth_type == NL80211_AUTHTYPE_SAE)
		algorithm = WLAN_AUTH_SAE;
	else
		return -EOPNOTSUPP;
	if (req->auth_data_len && req->auth_data_len < 4)
		return -EINVAL;
	body_len = req->auth_data_len ?: 4;
	if (body_len + req->ie_len >
	    W4_RX_MAX - W4_TX_DESC_LEN - IEEE80211_HDRLEN - 2)
		return -EMSGSIZE;
	len = IEEE80211_HDRLEN + 2 + body_len + req->ie_len;
	if (len > frame_size)
		return -EMSGSIZE;
	memset(frame, 0, len);
	w4_mgmt_header(w4, frame, IEEE80211_STYPE_AUTH);
	put_unaligned_le16(algorithm, frame + IEEE80211_HDRLEN);
	if (req->auth_data_len)
		memcpy(frame + IEEE80211_HDRLEN + 2, req->auth_data,
		       req->auth_data_len);
	else
		put_unaligned_le16(1, frame + IEEE80211_HDRLEN + 2);
	if (req->ie_len)
		memcpy(frame + IEEE80211_HDRLEN + 2 + body_len, req->ie,
		       req->ie_len);
	return len;
}

static int w4_send_auth(struct mt6797_wifi_w4 *w4,
			const struct cfg80211_auth_request *req)
{
	u8 *frame;
	int len, ret;

	frame = kmalloc(W4_RX_MAX - W4_TX_DESC_LEN, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	len = w4_compose_auth(w4, req, frame,
			      W4_RX_MAX - W4_TX_DESC_LEN);
	ret = len < 0 ? len : w4_send_mgmt(w4, frame, len);
	kfree(frame);
	return ret;
}

static int w4_send_assoc(struct mt6797_wifi_w4 *w4,
			 const struct cfg80211_assoc_request *req)
{
	u8 rates[ARRAY_SIZE(w4_rate_codes)];
	u8 *frame, *ie;
	u16 capability = WLAN_CAPABILITY_ESS;
	unsigned int n_rates, supported_rates;
	size_t len, ie_len, rate_ie_len;
	int ret;

	n_rates = w4_rate_set_to_codes(w4->desired_rate_set,
				       w4->basic_rate_set, rates);
	supported_rates = min(n_rates, 8U);
	rate_ie_len = 2 + supported_rates;
	if (n_rates > supported_rates)
		rate_ie_len += 2 + n_rates - supported_rates;
	ie_len = 2 + w4->join_ssid_len + rate_ie_len + req->ie_len;
	if (ie_len > W4_ASSOC_IE_MAX)
		return -EMSGSIZE;
	len = IEEE80211_HDRLEN + 4 + ie_len;
	frame = kzalloc(len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	ie = frame + IEEE80211_HDRLEN + 4;
	if (w4->candidate_capability & WLAN_CAPABILITY_SHORT_PREAMBLE)
		capability |= WLAN_CAPABILITY_SHORT_PREAMBLE;
	if (w4->non_ht_basic_phy_type == W4_PHY_TYPE_ERP_INDEX)
		capability |= WLAN_CAPABILITY_SHORT_SLOT_TIME;
	if (req->crypto.n_ciphers_pairwise)
		capability |= WLAN_CAPABILITY_PRIVACY;
	w4_mgmt_header(w4, frame, IEEE80211_STYPE_ASSOC_REQ);
	put_unaligned_le16(capability, frame + IEEE80211_HDRLEN);
	put_unaligned_le16(10, frame + IEEE80211_HDRLEN + 2);
	*ie++ = WLAN_EID_SSID;
	*ie++ = w4->join_ssid_len;
	memcpy(ie, w4->join_ssid, w4->join_ssid_len);
	ie += w4->join_ssid_len;
	*ie++ = WLAN_EID_SUPP_RATES;
	*ie++ = supported_rates;
	memcpy(ie, rates, supported_rates);
	ie += supported_rates;
	if (n_rates > supported_rates) {
		*ie++ = WLAN_EID_EXT_SUPP_RATES;
		*ie++ = n_rates - supported_rates;
		memcpy(ie, rates + supported_rates, n_rates - supported_rates);
		ie += n_rates - supported_rates;
	}
	if (req->ie_len) {
		memcpy(ie, req->ie, req->ie_len);
		ie += req->ie_len;
	}
	w4->assoc_req_ie_len = ie - (frame + IEEE80211_HDRLEN + 4);
	memcpy(w4->assoc_req_ies, frame + IEEE80211_HDRLEN + 4,
	       w4->assoc_req_ie_len);
	ret = w4_send_mgmt(w4, frame, len);
	kfree(frame);
	return ret;
}

static int w4_send_sta_state(struct mt6797_wifi_w4 *w4, u8 sta_state, u16 aid)
{
	u8 cmd[8 + 124] = { };

	w4_fill_sta_record(w4, cmd, 4, sta_state, aid);
	return w4_cmd53(w4, true, W4_WTDR1, cmd, sizeof(cmd));
}

static int w4_send_bss_info(struct mt6797_wifi_w4 *w4, bool connected)
{
	u8 cmd[MT6797_WIFI_GEN3_BSS_INFO_CMD_LEN];
	int ret;

	ret = mt6797_wifi_gen3_build_bss_info_cmd(cmd, sizeof(cmd),
						  ++w4->command_sequence,
						  connected,
						  w4->protected_network,
						  w4->desired_rate_set,
						  w4->basic_rate_set,
						  w4->non_ht_basic_phy_type,
						  w4->desired_phy_type_set,
						  w4->join_ssid,
						  w4->join_ssid_len,
						  w4->candidate_bssid,
						  w4->candidate_channel);
	if (ret < 0)
		return ret;
	return w4_cmd53(w4, true, W4_WTDR1, cmd, ret);
}

static void w4_join_timeout(struct work_struct *work)
{
	struct mt6797_wifi_w4 *w4 = container_of(to_delayed_work(work),
		struct mt6797_wifi_w4, join_timeout);
	struct cfg80211_assoc_failure failure = { .timeout = true };

	wiphy_lock(w4->wiphy);
	mutex_lock(&w4->lock);
	if (w4->join_state != W4_JOIN_WAIT_CHANNEL &&
	    w4->join_state != W4_JOIN_AUTHENTICATING &&
	    w4->join_state != W4_JOIN_ASSOCIATING)
		goto out;
	if (w4->join_state == W4_JOIN_WAIT_CHANNEL ||
	    w4->join_state == W4_JOIN_AUTHENTICATING) {
		cfg80211_auth_timeout(w4->ndev, w4->candidate_bssid);
	} else {
		failure.bss[0] = w4->assoc_bss;
		w4->assoc_bss = NULL;
		cfg80211_assoc_failure(w4->ndev, &failure);
	}
	w4_release_channel(w4);
	w4->join_state = W4_JOIN_IDLE;
	w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
	w4->pending_auth_len = 0;
	w4->protected_network = false;
	w4->port_authorized = false;
	w4->control_port_no_encrypt = false;
	w4->use_mfp = false;
out:
	mutex_unlock(&w4->lock);
	wiphy_unlock(w4->wiphy);
}

/* Host MLME receive completion, invoked with the wiphy and driver locks held. */
static void w4_rx_mgmt(struct mt6797_wifi_w4 *w4, const u8 *mgmt, size_t len)
{
	u16 fc, status;

	if (len < IEEE80211_HDRLEN ||
	    !ether_addr_equal(mgmt + 10, w4->candidate_bssid) ||
	    (!ether_addr_equal(mgmt + 4, w4->ndev->dev_addr) &&
	     !is_broadcast_ether_addr(mgmt + 4)))
		return;
	fc = get_unaligned_le16(mgmt);
	if (w4->join_state == W4_JOIN_AUTHENTICATING &&
	    len >= IEEE80211_HDRLEN + 6 &&
	    (fc & 0x00fc) == IEEE80211_STYPE_AUTH) {
		u16 expected = w4->auth_type == NL80211_AUTHTYPE_SAE ?
			WLAN_AUTH_SAE : WLAN_AUTH_OPEN;

		if (get_unaligned_le16(mgmt + IEEE80211_HDRLEN) != expected)
			return;
		status = get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 4);
		cancel_delayed_work(&w4->join_timeout);
		cfg80211_rx_mlme_mgmt(w4->ndev, mgmt, len);
		if (status) {
			w4_release_channel(w4);
			w4->join_state = W4_JOIN_IDLE;
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
			w4->protected_network = false;
			w4->port_authorized = false;
			w4->control_port_no_encrypt = false;
		} else {
			mod_delayed_work(system_wq, &w4->join_timeout,
					 msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
		}
		return;
	}
	if (w4->join_state == W4_JOIN_ASSOCIATING &&
	    len >= IEEE80211_HDRLEN + 6 &&
	    (fc & 0x00fc) == IEEE80211_STYPE_ASSOC_RESP) {
		struct cfg80211_rx_assoc_resp_data data = {
			.buf = mgmt,
			.len = len,
			.req_ies = w4->assoc_req_ies,
			.req_ies_len = w4->assoc_req_ie_len,
		};
		u16 aid = get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 4) & 0x3fff;

		status = get_unaligned_le16(mgmt + IEEE80211_HDRLEN + 2);
		cancel_delayed_work(&w4->join_timeout);
		if (!status &&
		    (w4_send_bss_info(w4, true) ||
		     w4_send_sta_state(w4, W4_STA_STATE_3, aid))) {
			struct cfg80211_assoc_failure failure = {
				.bss = { w4->assoc_bss },
			};

			w4_send_bss_info(w4, false);
			w4->assoc_bss = NULL;
			w4->join_state = W4_JOIN_IDLE;
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
			w4->protected_network = false;
			w4->port_authorized = false;
			w4->control_port_no_encrypt = false;
			w4->use_mfp = false;
			w4_release_channel(w4);
			netif_carrier_off(w4->ndev);
			cfg80211_assoc_failure(w4->ndev, &failure);
			return;
		}
		w4_release_channel(w4);
		w4->join_state = status ? W4_JOIN_IDLE : W4_JOIN_CONNECTED;
		if (status) {
			w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
			w4->protected_network = false;
			w4->port_authorized = false;
			w4->control_port_no_encrypt = false;
			w4->use_mfp = false;
		}
		if (status)
			netif_carrier_off(w4->ndev);
		else
			netif_carrier_on(w4->ndev);
		data.links[0].bss = w4->assoc_bss;
		ether_addr_copy(data.links[0].addr, w4->candidate_bssid);
		w4->assoc_bss = NULL;
		cfg80211_rx_assoc_resp(w4->ndev, &data);
		return;
	}
	if (w4->join_state == W4_JOIN_ASSOCIATING &&
	    len >= IEEE80211_HDRLEN + 2 &&
	    ((fc & 0x00fc) == IEEE80211_STYPE_DEAUTH ||
	     (fc & 0x00fc) == IEEE80211_STYPE_DISASSOC)) {
		struct cfg80211_assoc_failure failure = {
			.bss = { w4->assoc_bss },
		};

		cancel_delayed_work(&w4->join_timeout);
		w4->assoc_bss = NULL;
		w4->join_state = W4_JOIN_IDLE;
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		w4->pairwise_key_installed = false;
		w4->group_key_bitmap = 0;
		w4->mgmt_key_bitmap = 0;
		w4->protected_network = false;
		w4->port_authorized = false;
		w4->control_port_no_encrypt = false;
		w4->use_mfp = false;
		w4_release_channel(w4);
		netif_carrier_off(w4->ndev);
		cfg80211_rx_mlme_mgmt(w4->ndev, mgmt, len);
		cfg80211_assoc_failure(w4->ndev, &failure);
		return;
	}
	if ((w4->join_state == W4_JOIN_CONNECTED ||
	     w4->join_state == W4_JOIN_AUTHENTICATING) &&
	    len >= IEEE80211_HDRLEN + 2 &&
	    ((fc & 0x00fc) == IEEE80211_STYPE_DEAUTH ||
	     (fc & 0x00fc) == IEEE80211_STYPE_DISASSOC)) {
		bool connected = w4->join_state == W4_JOIN_CONNECTED;

		if (w4->join_state == W4_JOIN_CONNECTED && w4->use_mfp &&
		    !(fc & IEEE80211_FCTL_PROTECTED)) {
			cfg80211_rx_unprot_mlme_mgmt(w4->ndev, mgmt, len);
			return;
		}
		cancel_delayed_work(&w4->join_timeout);
		if (connected) {
			w4_purge_keys(w4);
			if (w4_send_bss_info(w4, false))
				dev_warn(w4->dev,
					 "failed to clear disconnected BSS state\n");
		}
		w4->join_state = W4_JOIN_IDLE;
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		w4->pairwise_key_installed = false;
		w4->group_key_bitmap = 0;
		w4->mgmt_key_bitmap = 0;
		w4->protected_network = false;
		w4->port_authorized = false;
		w4->control_port_no_encrypt = false;
		w4->use_mfp = false;
		w4_release_channel(w4);
		netif_carrier_off(w4->ndev);
		cfg80211_rx_mlme_mgmt(w4->ndev, mgmt, len);
	}
}
static int w4_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;
	mutex_lock(&w4->lock);
	if (!w4->opened) {
		mutex_unlock(&w4->lock);
		return -ENETDOWN;
	}
	if (w4->scan_request || w4->join_state != W4_JOIN_IDLE) {
		mutex_unlock(&w4->lock);
		return -EBUSY;
	}
	w4_diag_hif(w4, "scan-pre");
	ret = w4_activate_ais(w4, W4_CMD_SEQUENCE_ACTIVATE);
	if (!ret)
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

static int w4_auth(struct wiphy *wiphy, struct net_device *ndev,
		   struct cfg80211_auth_request *req)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	struct w4_bss_profile profile = { };
	const u8 *ssid;
	u8 join_ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_len;
	int ret;

	if (ndev != w4->ndev || !req->bss || req->link_id >= 0 || req->key ||
	    (req->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	     req->auth_type != NL80211_AUTHTYPE_SAE))
		return -EOPNOTSUPP;
	if (!req->bss->channel || req->bss->channel->band != NL80211_BAND_2GHZ)
		return -EINVAL;
	ret = w4_bss_profile_from_cfg80211(req->bss, &profile);
	if (ret)
		return ret;
	rcu_read_lock();
	ssid = ieee80211_bss_get_ie(req->bss, WLAN_EID_SSID);
	if (!ssid || ssid[1] > IEEE80211_MAX_SSID_LEN) {
		rcu_read_unlock();
		return -EINVAL;
	}
	ssid_len = ssid[1];
	memcpy(join_ssid, ssid + 2, ssid_len);
	rcu_read_unlock();

	mutex_lock(&w4->lock);
	if (!w4->opened) {
		ret = -ENETDOWN;
		goto out;
	}
	if (w4->join_state == W4_JOIN_AUTHENTICATING) {
		if (!ether_addr_equal(req->bss->bssid, w4->candidate_bssid) ||
		    req->auth_type != w4->auth_type) {
			ret = -EBUSY;
			goto out;
		}
		ret = w4_send_auth(w4, req);
		if (!ret)
			mod_delayed_work(system_wq, &w4->join_timeout,
					 msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
		goto out;
	}
	if (w4->join_state != W4_JOIN_IDLE || w4->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	ether_addr_copy(w4->candidate_bssid, req->bss->bssid);
	w4->candidate_channel = req->bss->channel->hw_value;
	w4->candidate_valid = true;
	w4->candidate_capability = profile.capability;
	w4->basic_rate_set = profile.basic_rate_set;
	w4->desired_rate_set = profile.desired_rate_set;
	w4->desired_phy_type_set = profile.desired_phy_type_set;
	w4->non_ht_basic_phy_type = profile.non_ht_basic_phy_type;
	w4->hw_default_rate_code = profile.hw_default_rate_code;
	/* AIS activation reserves entry zero for BMC. secPrivacySeekForEntry()
	 * therefore assigns entry one to the AP. Entry 31 remains the vendor
	 * no-peer/default entry; it must not carry AP TX.
	 */
	w4->sta_index = W4_STA_INDEX_AIS_AP;
	w4->wlan_index = W4_WLAN_INDEX_AIS_AP;
	w4->join_ssid_len = ssid_len;
	memcpy(w4->join_ssid, join_ssid, ssid_len);
	w4->auth_type = req->auth_type;
	w4->assoc_req_ie_len = 0;
	w4->use_mfp = false;
	w4->port_authorized = false;
	w4->control_port_no_encrypt = false;
	w4->pairwise_key_installed = false;
	w4->group_key_bitmap = 0;
	w4->mgmt_key_bitmap = 0;
	ret = w4_compose_auth(w4, req, w4->pending_auth,
			      sizeof(w4->pending_auth));
	if (ret < 0) {
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		goto out;
	}
	w4->pending_auth_len = ret;
	ret = w4_send_channel_request(w4);
	if (ret) {
		w4->pending_auth_len = 0;
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		goto out;
	}
	w4->join_state = W4_JOIN_WAIT_CHANNEL;
	mod_delayed_work(system_wq, &w4->join_timeout,
		msecs_to_jiffies(W4_CHANNEL_TIMEOUT_MS));
	mod_delayed_work(system_wq, &w4->scan_poll,
			 msecs_to_jiffies(W4_SCAN_POLL_INTERVAL_MS));
	ret = 0;
out:
	mutex_unlock(&w4->lock);
	return ret;
}

static bool w4_crypto_valid(const struct cfg80211_assoc_request *req,
			    enum nl80211_auth_type auth_type)
{
	if (req->crypto.control_port_over_nl80211 ||
	    (req->crypto.control_port_ethertype &&
	     req->crypto.control_port_ethertype != htons(ETH_P_PAE)) ||
	    req->crypto.psk || req->crypto.sae_pwd)
		return false;
	if (!req->crypto.n_akm_suites && !req->crypto.n_ciphers_pairwise)
		return !req->crypto.wpa_versions && !req->crypto.cipher_group &&
			auth_type ==
			NL80211_AUTHTYPE_OPEN_SYSTEM;
	if (req->crypto.wpa_versions != NL80211_WPA_VERSION_2 ||
	    req->crypto.n_akm_suites != 1 ||
	    req->crypto.n_ciphers_pairwise != 1 ||
	    req->crypto.ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP ||
	    req->crypto.cipher_group != WLAN_CIPHER_SUITE_CCMP)
		return false;
	if (req->crypto.akm_suites[0] == WLAN_AKM_SUITE_SAE)
		return auth_type == NL80211_AUTHTYPE_SAE && req->use_mfp;
	return req->crypto.akm_suites[0] == WLAN_AKM_SUITE_PSK &&
		auth_type == NL80211_AUTHTYPE_OPEN_SYSTEM;
}

static int w4_assoc(struct wiphy *wiphy, struct net_device *ndev,
		    struct cfg80211_assoc_request *req)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;

	if (ndev != w4->ndev || !req->bss || req->link_id >= 0 ||
	    req->prev_bssid || !w4_crypto_valid(req, w4->auth_type))
		return -EOPNOTSUPP;

	mutex_lock(&w4->lock);
	if (!w4->opened) {
		ret = -ENETDOWN;
		goto out;
	}
	if (w4->join_state != W4_JOIN_AUTHENTICATING || w4->assoc_bss) {
		ret = -EBUSY;
		goto out;
	}
	if (!ether_addr_equal(req->bss->bssid, w4->candidate_bssid)) {
		ret = -EINVAL;
		goto out;
	}
	ret = w4_send_sta_state(w4, W4_STA_STATE_2, 0);
	if (!ret)
		ret = w4_send_assoc(w4, req);
	if (ret)
		goto out;
	w4->assoc_bss = req->bss;
	w4->use_mfp = req->use_mfp;
	w4->protected_network = req->crypto.n_ciphers_pairwise;
	w4->port_authorized = !req->crypto.control_port;
	w4->control_port_no_encrypt = req->crypto.control_port_no_encrypt;
	w4->join_state = W4_JOIN_ASSOCIATING;
	mod_delayed_work(system_wq, &w4->join_timeout,
			 msecs_to_jiffies(W4_JOIN_TIMEOUT_MS));
out:
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_leave(struct mt6797_wifi_w4 *w4, const u8 *peer, u16 subtype,
		    u16 reason, const u8 *ie, size_t ie_len,
		    bool local_state_change)
{
	struct cfg80211_assoc_failure failure = { };
	u8 *frame;
	size_t len = IEEE80211_HDRLEN + 2 + ie_len;
	bool connected = w4->join_state == W4_JOIN_CONNECTED;
	int ret = 0;

	if (!ether_addr_equal(peer, w4->candidate_bssid) ||
	    len > W4_RX_MAX - W4_TX_DESC_LEN)
		return -EINVAL;
	frame = kzalloc(len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	w4_mgmt_header(w4, frame, subtype |
		(w4->use_mfp && w4->pairwise_key_installed ?
		 IEEE80211_FCTL_PROTECTED : 0));
	put_unaligned_le16(reason, frame + IEEE80211_HDRLEN);
	if (ie_len)
		memcpy(frame + IEEE80211_HDRLEN + 2, ie, ie_len);
	if (!local_state_change)
		ret = w4_send_mgmt(w4, frame, len);
	if (!ret)
		cfg80211_tx_mlme_mgmt(w4->ndev, frame, len, false);
	if (!ret) {
		int cleanup_ret = 0;

		if (w4->assoc_bss) {
			failure.bss[0] = w4->assoc_bss;
			w4->assoc_bss = NULL;
			cfg80211_assoc_failure(w4->ndev, &failure);
		}
		cancel_delayed_work(&w4->join_timeout);
		if (connected) {
			w4_purge_keys(w4);
			cleanup_ret = w4_send_bss_info(w4, false);
		}
		w4->join_state = W4_JOIN_IDLE;
		w4->wlan_index = W4_WLAN_INDEX_DEFAULT;
		w4->pairwise_key_installed = false;
		w4->group_key_bitmap = 0;
		w4->mgmt_key_bitmap = 0;
		w4->protected_network = false;
		w4->port_authorized = false;
		w4->control_port_no_encrypt = false;
		w4->use_mfp = false;
		w4_release_channel(w4);
		netif_carrier_off(w4->ndev);
		if (cleanup_ret)
			dev_warn(w4->dev,
				 "failed to clear firmware BSS state: %d\n",
				 cleanup_ret);
	}
	kfree(frame);
	return ret;
}

static int w4_deauth(struct wiphy *wiphy, struct net_device *ndev,
		     struct cfg80211_deauth_request *req)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;

	if (ndev != w4->ndev || !req->bssid)
		return -EINVAL;
	mutex_lock(&w4->lock);
	ret = w4_leave(w4, req->bssid, IEEE80211_STYPE_DEAUTH,
		       req->reason_code, req->ie, req->ie_len,
		       req->local_state_change);
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_disassoc(struct wiphy *wiphy, struct net_device *ndev,
		       struct cfg80211_disassoc_request *req)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;

	if (ndev != w4->ndev || !req->ap_addr)
		return -EINVAL;
	mutex_lock(&w4->lock);
	ret = w4_leave(w4, req->ap_addr, IEEE80211_STYPE_DISASSOC,
		       req->reason_code, req->ie, req->ie_len,
		       req->local_state_change);
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		      int link_id, u8 key_index, bool pairwise,
		      const u8 *mac_addr, struct key_params *params)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	enum mt6797_wifi_gen3_cipher cipher;
	u8 peer[ETH_ALEN];
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	u8 wlan_index;
	int ret;

	if (wdev != w4->wdev || link_id >= 0 || !params)
		return -EINVAL;
	if (params->mode != NL80211_KEY_RX_TX)
		return -EOPNOTSUPP;
	if (params->cipher == WLAN_CIPHER_SUITE_CCMP)
		cipher = MT6797_WIFI_GEN3_CIPHER_CCMP;
	else if (params->cipher == WLAN_CIPHER_SUITE_AES_CMAC)
		cipher = MT6797_WIFI_GEN3_CIPHER_BIP;
	else
		return -EOPNOTSUPP;
	if (!params->key || params->key_len != 16 || params->seq_len > 16 ||
	    (params->seq_len && !params->seq))
		return -EINVAL;
	if ((cipher == MT6797_WIFI_GEN3_CIPHER_BIP &&
	     (pairwise || key_index < 4 || key_index > 5)) ||
	    (cipher == MT6797_WIFI_GEN3_CIPHER_CCMP &&
	     ((pairwise && key_index != 0) || (!pairwise && key_index > 3))))
		return -EINVAL;
	if (pairwise) {
		if (!mac_addr || !ether_addr_equal(mac_addr, w4->candidate_bssid))
			return -EINVAL;
		ether_addr_copy(peer, mac_addr);
		wlan_index = W4_WLAN_INDEX_AIS_AP;
	} else {
		if (mac_addr && !is_broadcast_ether_addr(mac_addr) &&
		    !ether_addr_equal(mac_addr, w4->candidate_bssid))
			return -EINVAL;
		ether_addr_copy(peer, w4->candidate_bssid);
		wlan_index = W4_STA_BMC_WLAN_INDEX_AIS_AP;
	}

	mutex_lock(&w4->lock);
	if (!w4->opened || w4->join_state != W4_JOIN_CONNECTED) {
		ret = -ENOTCONN;
		goto out;
	}
	if (!w4->protected_network ||
	    (cipher == MT6797_WIFI_GEN3_CIPHER_BIP && !w4->use_mfp)) {
		ret = -EINVAL;
		goto out;
	}
	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd),
					     ++w4->command_sequence, true,
					     pairwise, peer, key_index,
					     wlan_index, cipher, params->key,
					     params->key_len, params->seq,
					     params->seq_len);
	if (ret < 0)
		goto out;
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, ret);
	memzero_explicit(cmd, sizeof(cmd));
	if (!ret && pairwise)
		w4->pairwise_key_installed = true;
	else if (!ret && cipher == MT6797_WIFI_GEN3_CIPHER_BIP)
		w4->mgmt_key_bitmap |= BIT(key_index);
	else if (!ret)
		w4->group_key_bitmap |= BIT(key_index);
out:
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		      int link_id, u8 key_index, bool pairwise,
		      const u8 *mac_addr)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	u8 peer[ETH_ALEN];
	u8 cmd[MT6797_WIFI_GEN3_KEY_CMD_LEN];
	u8 wlan_index;
	int ret;

	if (wdev != w4->wdev || link_id >= 0 ||
	    (pairwise && key_index != 0) || (!pairwise && key_index > 5))
		return -EINVAL;
	if (pairwise) {
		if (!mac_addr || !ether_addr_equal(mac_addr, w4->candidate_bssid))
			return -EINVAL;
		ether_addr_copy(peer, mac_addr);
		wlan_index = W4_WLAN_INDEX_AIS_AP;
	} else {
		if (mac_addr && !is_broadcast_ether_addr(mac_addr) &&
		    !ether_addr_equal(mac_addr, w4->candidate_bssid))
			return -EINVAL;
		ether_addr_copy(peer, w4->candidate_bssid);
		wlan_index = W4_STA_BMC_WLAN_INDEX_AIS_AP;
	}

	mutex_lock(&w4->lock);
	if (!w4->opened) {
		if (pairwise) {
			w4->pairwise_key_installed = false;
		} else {
			w4->group_key_bitmap &= ~BIT(key_index);
			w4->mgmt_key_bitmap &= ~BIT(key_index);
		}
		ret = 0;
		goto out;
	}
	if ((pairwise && !w4->pairwise_key_installed) ||
	    (!pairwise && !(w4->group_key_bitmap & BIT(key_index)) &&
	     !(w4->mgmt_key_bitmap & BIT(key_index)))) {
		ret = 0;
		goto out;
	}
	ret = mt6797_wifi_gen3_build_key_cmd(cmd, sizeof(cmd),
					     ++w4->command_sequence, false,
					     pairwise, peer, key_index,
					     wlan_index,
					     MT6797_WIFI_GEN3_CIPHER_NONE,
					     NULL, 0, NULL, 0);
	if (ret < 0)
		goto out;
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, ret);
	if (!ret && pairwise)
		w4->pairwise_key_installed = false;
	else if (!ret) {
		w4->group_key_bitmap &= ~BIT(key_index);
		w4->mgmt_key_bitmap &= ~BIT(key_index);
	}
out:
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
			      int link_id, u8 key_index, bool unicast,
			      bool multicast)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	u8 cmd[MT6797_WIFI_GEN3_DEFAULT_KEY_CMD_LEN];
	int ret;

	if (ndev != w4->ndev || link_id >= 0 || key_index > 3 ||
	    (!unicast && !multicast))
		return -EINVAL;
	if (unicast && !multicast)
		return 0;
	mutex_lock(&w4->lock);
	if (!w4->opened || w4->join_state != W4_JOIN_CONNECTED) {
		ret = -ENOTCONN;
		goto out;
	}
	if (!w4->protected_network) {
		ret = -EINVAL;
		goto out;
	}
	if (!(w4->group_key_bitmap & BIT(key_index))) {
		ret = -ENOENT;
		goto out;
	}
	ret = mt6797_wifi_gen3_build_default_key_cmd(cmd, sizeof(cmd),
						     ++w4->command_sequence,
						     key_index, unicast,
						     multicast);
	if (ret < 0)
		goto out;
	ret = w4_cmd53(w4, true, W4_WTDR1, cmd, ret);
out:
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_set_default_mgmt_key(struct wiphy *wiphy,
				   struct wireless_dev *wdev, int link_id,
				   u8 key_index)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret;

	if (wdev != w4->wdev || link_id >= 0 || key_index < 4 || key_index > 5)
		return -EINVAL;

	mutex_lock(&w4->lock);
	if (!w4->opened || w4->join_state != W4_JOIN_CONNECTED)
		ret = -ENOTCONN;
	else if (!w4->use_mfp || !(w4->mgmt_key_bitmap & BIT(key_index)))
		ret = -ENOENT;
	else
		/* GEN3 selects the receive BIP slot from the IGTK key ID itself. */
		ret = 0;
	mutex_unlock(&w4->lock);
	return ret;
}

static int w4_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const u8 *mac, struct station_parameters *params)
{
	struct mt6797_wifi_w4 *w4 = w4_wiphy(wiphy);
	int ret = 0;

	if (wdev != w4->wdev || !mac || !params ||
	    !ether_addr_equal(mac, w4->candidate_bssid))
		return -EINVAL;
	if (!(params->sta_flags_mask & BIT(NL80211_STA_FLAG_AUTHORIZED)) ||
	    (params->sta_flags_mask & ~BIT(NL80211_STA_FLAG_AUTHORIZED)))
		return -EOPNOTSUPP;
	mutex_lock(&w4->lock);
	if (!w4->opened || w4->join_state != W4_JOIN_CONNECTED)
		ret = -ENOTCONN;
	else
		w4->port_authorized =
			params->sta_flags_set & BIT(NL80211_STA_FLAG_AUTHORIZED);
	mutex_unlock(&w4->lock);
	return ret;
}

static const struct cfg80211_ops w4_cfg80211_ops = {
	.scan = w4_scan, .auth = w4_auth, .assoc = w4_assoc,
	.deauth = w4_deauth, .disassoc = w4_disassoc,
	.add_key = w4_add_key, .del_key = w4_del_key,
	.set_default_key = w4_set_default_key,
	.set_default_mgmt_key = w4_set_default_mgmt_key,
	.change_station = w4_change_station,
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
	w4->wiphy->features |= NL80211_FEATURE_SAE;
	w4->wiphy->cipher_suites = w4_cipher_suites;
	w4->wiphy->n_cipher_suites = ARRAY_SIZE(w4_cipher_suites);
	/* iw's ordinary wildcard probe is one SSID entry; zero makes nl80211
	 * reject it before ->scan() is reached.  CMD_SCAN_REQ implements one. */
	w4->wiphy->max_scan_ssids = 1;
	w4->wiphy->max_scan_ie_len = W4_SCAN_MAX_IE_LEN;
	w4->wiphy->bands[NL80211_BAND_2GHZ] = &w4_band; set_wiphy_dev(w4->wiphy, &pdev->dev);
	ret = wiphy_register(w4->wiphy); if (ret) goto free_wiphy;
	w4->ndev = alloc_netdev(sizeof(struct mt6797_wifi_w4 *), "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!w4->ndev) { ret = -ENOMEM; goto unregister_wiphy; }
	netif_carrier_off(w4->ndev);
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
