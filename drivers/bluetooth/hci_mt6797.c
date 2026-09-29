// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standard Bluetooth HCI endpoint for the MT6797 CONSYS full-STP owner.
 *
 * The parent CONSYS aggregate owns power, the ROM patch download, BTIF and the
 * multiplexed full-STP session. This endpoint only asks the aggregate for the
 * Bluetooth function (WMT FUNC_CTRL on STP channel 0) and exchanges H:4
 * packets with the controller over that channel.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include <linux/soc/mediatek/mt6797-consys-bt.h>
#include <linux/soc/mediatek/mtk-stp.h>

#include "btmtk.h"
#include "hci_uart.h"

/*
 * Vendor MT6797 BlueZ endpoint defaults, used when no per-device BT.cfg
 * exists: drivers/misc/mediatek/connectivity/drv_bt/include/bt_conf.h:36-45
 * sDefaultCfg. drv_bt/linux/hci_stp.c:935-945 treats the same address read
 * back from the controller as an unprogrammed eFUSE value. Address bytes are
 * in HCI (little-endian) order.
 */
static const bdaddr_t mt6797_hci_default_bdaddr = {
	{ 0x01, 0x79, 0x02, 0x46, 0x00, 0x00 }
};

/* 0xfc79: vendor bt_set_radio (hci_stp.c:142) with sDefaultCfg.radio. */
#define MT6797_HCI_OP_SET_RADIO		0xfc79
static const u8 mt6797_hci_radio[] = { 0x07, 0x80, 0x00, 0x06, 0x05, 0x07 };

/* 0xfc93: vendor bt_set_tx_pwr_offset (hci_stp.c:146) with sDefaultCfg. */
#define MT6797_HCI_OP_SET_TX_PWR_OFFSET	0xfc93
static const u8 mt6797_hci_tx_pwr_offset[] = { 0xff, 0xff, 0xff };

struct mt6797_hci {
	struct device *dev;
	struct hci_dev *hdev;
	/* H:4 reassembly state for h4_recv_buf(); only hu.hdev is used. */
	struct hci_uart hu;
	struct sk_buff *rx_skb;
	const struct mt6797_consys_bt_lease *lease;
};

static const struct h4_recv_pkt mt6797_hci_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};

/*
 * Runs in the BTIF receive thread for each in-sequence STP channel-0 payload.
 * Like the vendor BlueZ path (hci_stp.c:1100-1180), treat the channel as an
 * H:4 byte stream rather than assuming one HCI packet per STP frame.
 *
 * Always consume the payload: a non-zero return makes STP withhold its ACK
 * so the controller would retransmit the same bytes indefinitely. The vendor
 * core ACKs BlueZ payloads regardless of the host result
 * (common_main/core/stp_core.c:1289-1306).
 */
static int mt6797_hci_receive(void *context, const u8 *data, size_t length)
{
	struct mt6797_hci *bt = context;
	struct hci_dev *hdev = bt->hdev;

	if (!length)
		return 0;

	bt->rx_skb = h4_recv_buf(&bt->hu, bt->rx_skb, data, length,
				 mt6797_hci_recv_pkts,
				 ARRAY_SIZE(mt6797_hci_recv_pkts));
	if (IS_ERR(bt->rx_skb)) {
		bt_dev_err_ratelimited(hdev, "dropping malformed H:4 data (%ld)",
				       PTR_ERR(bt->rx_skb));
		hdev->stat.err_rx++;
		bt->rx_skb = NULL;
		return 0;
	}
	hdev->stat.byte_rx += length;
	return 0;
}

static int mt6797_hci_open(struct hci_dev *hdev)
{
	struct mt6797_hci *bt = hci_get_drvdata(hdev);
	const struct mt6797_consys_bt_lease *lease;

	if (bt->lease)
		return -EBUSY;

	bt->hu.padding = 0;
	lease = mt6797_consys_bt_acquire(bt->dev, mt6797_hci_receive, bt);
	if (IS_ERR(lease))
		return PTR_ERR(lease);
	bt->lease = lease;
	return 0;
}

static int mt6797_hci_close(struct hci_dev *hdev)
{
	struct mt6797_hci *bt = hci_get_drvdata(hdev);
	const struct mt6797_consys_bt_lease *lease = bt->lease;
	int ret;

	if (!lease)
		return 0;
	bt->lease = NULL;
	/* Release unbinds the receiver under the STP lock before returning. */
	ret = mt6797_consys_bt_release(lease);
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	return ret;
}

static int mt6797_hci_flush(struct hci_dev *hdev)
{
	return 0;
}

/* Record which ROM/patch level answered; setup runs on every open. */
static int mt6797_hci_log_version(struct hci_dev *hdev)
{
	struct hci_rp_read_local_version *ver;
	struct sk_buff *skb;
	int ret = 0;

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_LOCAL_VERSION, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "reading local version failed (%ld)",
			   PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	if (skb->len != sizeof(*ver)) {
		ret = -EIO;
		goto out;
	}
	ver = (struct hci_rp_read_local_version *)skb->data;
	if (ver->status) {
		ret = -bt_to_errno(ver->status);
		goto out;
	}
	bt_dev_info(hdev,
		    "HCI ver %u rev 0x%04x, LMP ver %u subver 0x%04x, manufacturer %u",
		    ver->hci_ver, le16_to_cpu(ver->hci_rev), ver->lmp_ver,
		    le16_to_cpu(ver->lmp_subver), le16_to_cpu(ver->manufacturer));
out:
	kfree_skb(skb);
	return ret;
}

static int mt6797_hci_setup(struct hci_dev *hdev)
{
	struct hci_rp_read_bd_addr *rp;
	struct sk_buff *skb;
	int ret;

	ret = mt6797_hci_log_version(hdev);
	if (ret)
		return ret;

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "reading BD address failed (%ld)",
			   PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	if (skb->len != sizeof(*rp)) {
		kfree_skb(skb);
		return -EIO;
	}
	rp = (struct hci_rp_read_bd_addr *)skb->data;
	if (rp->status) {
		ret = -bt_to_errno(rp->status);
		kfree_skb(skb);
		return ret;
	}
	if (!bacmp(&rp->bdaddr, BDADDR_ANY) ||
	    !bacmp(&rp->bdaddr, &mt6797_hci_default_bdaddr)) {
		/*
		 * The vendor stack takes the address from NVRAM (APRDEB
		 * BT_Addr on nvdata), a file the kernel must not parse. Start
		 * unconfigured so DT local-bd-address or userspace
		 * (btmgmt public-addr) supplies a stable address.
		 */
		bt_dev_info(hdev, "unprogrammed BD address %pMR; waiting for a configured address",
			    &rp->bdaddr);
		hci_set_quirk(hdev, HCI_QUIRK_INVALID_BDADDR);
		hci_set_quirk(hdev, HCI_QUIRK_USE_BDADDR_PROPERTY);
	}
	kfree_skb(skb);

	/*
	 * The vendor BlueZ init table (hci_stp.c:233-243) then applies the
	 * radio and TX power offset settings. Its sleep command (0xfc7a) is
	 * deliberately not sent: its host wake-up scheme relies on the vendor
	 * STP power-saving handshake (psm_core.c), which this STP owner does not
	 * implement. The core issues the table's final HCI_Reset.
	 */
	ret = __hci_cmd_sync_status(hdev, MT6797_HCI_OP_SET_RADIO,
				    sizeof(mt6797_hci_radio), mt6797_hci_radio,
				    HCI_INIT_TIMEOUT);
	if (ret) {
		bt_dev_err(hdev, "applying radio settings failed (%d)", ret);
		return ret;
	}
	ret = __hci_cmd_sync_status(hdev, MT6797_HCI_OP_SET_TX_PWR_OFFSET,
				    sizeof(mt6797_hci_tx_pwr_offset),
				    mt6797_hci_tx_pwr_offset, HCI_INIT_TIMEOUT);
	if (ret)
		bt_dev_err(hdev, "applying TX power offset failed (%d)", ret);
	return ret;
}

/*
 * Called from the HCI command/TX work items, which may sleep. The frame is
 * copied into an STP slot synchronously; on failure the core frees the skb.
 */
static int mt6797_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mt6797_hci *bt = hci_get_drvdata(hdev);
	unsigned int length = skb->len;
	int ret;

	if (!bt->lease)
		return -ESHUTDOWN;
	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
	case HCI_ACLDATA_PKT:
	case HCI_SCODATA_PKT:
		break;
	default:
		return -EILSEQ;
	}
	if (length + 1 > MTK_STP_MAX_PAYLOAD)
		return -EMSGSIZE;

	/* Vendor hci_stp.c:601 sends the H:4 type byte inside the STP payload. */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	ret = mt6797_consys_bt_transmit(bt->lease, skb->data, skb->len);
	if (ret) {
		hdev->stat.err_tx++;
		return ret;
	}

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}
	hdev->stat.byte_tx += length;
	kfree_skb(skb);
	return 0;
}

static int mt6797_hci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_hci *bt;
	struct hci_dev *hdev;
	int ret;

	bt = devm_kzalloc(dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;
	bt->dev = dev;
	bt->hdev = hdev;
	bt->hu.hdev = hdev;
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, dev);
	hdev->open = mt6797_hci_open;
	hdev->close = mt6797_hci_close;
	hdev->flush = mt6797_hci_flush;
	hdev->setup = mt6797_hci_setup;
	hdev->send = mt6797_hci_send_frame;
	hdev->set_bdaddr = btmtk_set_bdaddr;
	/* close() turns the WMT Bluetooth function off, losing setup state. */
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	/*
	 * The firmware (LMP subver 0x0307) reports Data Length Extension,
	 * LL Privacy and Extended Scanner Filter Policies, but answers
	 * LE Read Maximum Data Length with Unknown Command and rejects any
	 * LE event mask bit above bit 4 (status 0x20). The vendor kernel's
	 * BlueZ used it as a Bluetooth 4.0 LE controller.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LE_FEATURES);
	/* Listed in its supported commands, yet fails with status 0x02. */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_MWS_TRANSPORT_CONFIG);
	platform_set_drvdata(pdev, bt);
	ret = hci_register_dev(hdev);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		hci_free_dev(hdev);
		return ret;
	}
	return 0;
}

static void mt6797_hci_remove(struct platform_device *pdev)
{
	struct mt6797_hci *bt = platform_get_drvdata(pdev);

	/* Unregistering closes an open device through mt6797_hci_close(). */
	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
}

static const struct of_device_id mt6797_hci_of_match[] = {
	{ .compatible = "mediatek,mt6797-bluetooth" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_hci_of_match);

static struct platform_driver mt6797_hci_driver = {
	.probe = mt6797_hci_probe,
	.remove = mt6797_hci_remove,
	.driver = {
		.name = "mt6797-bluetooth",
		.of_match_table = mt6797_hci_of_match,
	},
};
module_platform_driver(mt6797_hci_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CONSYS Bluetooth HCI transport");
MODULE_LICENSE("GPL");
