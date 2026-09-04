// SPDX-License-Identifier: GPL-2.0-only
/* Standard Bluetooth HCI endpoint for the MT6797 CONSYS full-STP owner. */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include <linux/soc/mediatek/mt6797-consys-bt.h>
#include <linux/soc/mediatek/mtk-stp.h>

struct mt6797_hci {
	struct device *dev;
	struct hci_dev *hdev;
	const struct mt6797_consys_bt_lease *lease;
};

static int mt6797_hci_frame_length(u8 type, const u8 *data, size_t length,
				   size_t *frame_length)
{
	size_t expected;

	switch (type) {
	case HCI_EVENT_PKT:
		if (length < HCI_EVENT_HDR_SIZE)
			return -EMSGSIZE;
		expected = HCI_EVENT_HDR_SIZE + data[1];
		break;
	case HCI_ACLDATA_PKT:
		if (length < HCI_ACL_HDR_SIZE)
			return -EMSGSIZE;
		expected = HCI_ACL_HDR_SIZE + get_unaligned_le16(data + 2);
		break;
	case HCI_SCODATA_PKT:
		if (length < HCI_SCO_HDR_SIZE)
			return -EMSGSIZE;
		expected = HCI_SCO_HDR_SIZE + data[2];
		break;
	default:
		return -EPROTO;
	}
	if (expected != length)
		return -EMSGSIZE;
	*frame_length = expected;
	return 0;
}

static int mt6797_hci_receive(void *context, const u8 *frame,
			      size_t frame_length)
{
	struct mt6797_hci *bt = context;
	struct sk_buff *skb;
	size_t hci_length;
	u8 type;
	int ret;

	if (!frame || frame_length < 1)
		return -EMSGSIZE;
	type = frame[0];
	ret = mt6797_hci_frame_length(type, frame + 1, frame_length - 1,
				      &hci_length);
	if (ret) {
		bt->hdev->stat.err_rx++;
		return ret;
	}
	skb = bt_skb_alloc(hci_length, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, frame + 1, hci_length);
	hci_skb_pkt_type(skb) = type;
	bt->hdev->stat.byte_rx += hci_length;
	return hci_recv_frame(bt->hdev, skb);
}

static int mt6797_hci_open(struct hci_dev *hdev)
{
	struct mt6797_hci *bt = hci_get_drvdata(hdev);
	const struct mt6797_consys_bt_lease *lease;

	if (bt->lease)
		return -EBUSY;
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

	if (!lease)
		return 0;
	bt->lease = NULL;
	return mt6797_consys_bt_release(lease);
}

static int mt6797_hci_flush(struct hci_dev *hdev)
{
	return 0;
}

static int mt6797_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mt6797_hci *bt = hci_get_drvdata(hdev);
	u8 *frame;
	size_t frame_length;
	int ret;

	if (!bt->lease) {
		ret = -ESHUTDOWN;
		goto free_skb;
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
	default:
		ret = -EILSEQ;
		goto free_skb;
	}
	if (skb->len >= MTK_STP_MAX_PAYLOAD) {
		ret = -EMSGSIZE;
		goto free_skb;
	}
	frame_length = skb->len + 1;
	frame = kmalloc(frame_length, GFP_KERNEL);
	if (!frame) {
		ret = -ENOMEM;
		goto free_skb;
	}
	frame[0] = hci_skb_pkt_type(skb);
	skb_copy_from_linear_data(skb, frame + 1, skb->len);
	ret = mt6797_consys_bt_transmit(bt->lease, frame, frame_length);
	kfree(frame);
	if (ret)
		hdev->stat.err_tx++;
	else
		hdev->stat.byte_tx += skb->len;

free_skb:
	kfree_skb(skb);
	return ret;
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
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, dev);
	hdev->open = mt6797_hci_open;
	hdev->close = mt6797_hci_close;
	hdev->flush = mt6797_hci_flush;
	hdev->send = mt6797_hci_send_frame;
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

	hci_unregister_dev(bt->hdev);
	if (bt->lease) {
		mt6797_consys_bt_release(bt->lease);
		bt->lease = NULL;
	}
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
