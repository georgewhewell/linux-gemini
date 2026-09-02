// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 HCI over controller-owned full-STP channel zero. */
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#if IS_ENABLED(CONFIG_BT_MTK6797_STP_KUNIT_TEST)
#include <kunit/test.h>
#endif
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

#define MT6797_STP_HCI_TX_RETRY_MS	20
#define MT6797_STP_HCI_VCN33_UV		3300000
#define MT6797_STP_HCI_START_RETRY_MS	200
#define MT6797_STP_HCI_START_RETRIES	300

struct mt6797_h4_rx {
	struct sk_buff *skb;
	unsigned int header_length;
	unsigned int expected_length;
};

struct mt6797_stp_hci {
	struct device *controller;
	struct regulator *paldo;
	struct hci_dev *hdev;
	/* Serialize power/function state with TX submission and draining. */
	struct mutex lock;
	/* Serialize fragmented H4 input independently of WMT/TX waits. */
	struct mutex rx_lock;
	struct mt6797_h4_rx rx;
	struct sk_buff_head txq;
	struct delayed_work tx_work;
	struct mt6797_consys_runtime_controller_bt_lease controller_lease;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	bool bound, paldo_on, function_on, accepting_tx;
};

static bool mt6797_stp_hci_tx_type_valid(u8 type)
{
	return type == HCI_COMMAND_PKT || type == HCI_ACLDATA_PKT ||
	       type == HCI_SCODATA_PKT;
}

static void mt6797_h4_reset(struct mt6797_h4_rx *rx)
{
	kfree_skb(rx->skb);
	rx->skb = NULL;
	rx->header_length = 0;
	rx->expected_length = 0;
}

static int mt6797_h4_start(struct mt6797_h4_rx *rx, u8 type, gfp_t priority)
{
	unsigned int header_length;
	unsigned int maximum_length;

	switch (type) {
	case HCI_EVENT_PKT:
		header_length = HCI_EVENT_HDR_SIZE;
		maximum_length = HCI_MAX_EVENT_SIZE;
		break;
	case HCI_ACLDATA_PKT:
		header_length = HCI_ACL_HDR_SIZE;
		maximum_length = HCI_MAX_FRAME_SIZE;
		break;
	case HCI_SCODATA_PKT:
		header_length = HCI_SCO_HDR_SIZE;
		maximum_length = HCI_MAX_SCO_SIZE;
		break;
	default:
		return -EILSEQ;
	}

	rx->skb = bt_skb_alloc(maximum_length, priority);
	if (!rx->skb)
		return -ENOMEM;
	hci_skb_pkt_type(rx->skb) = type;
	rx->header_length = header_length;
	rx->expected_length = header_length;
	return 0;
}

static unsigned int mt6797_h4_data_length(const struct mt6797_h4_rx *rx)
{
	switch (hci_skb_pkt_type(rx->skb)) {
	case HCI_EVENT_PKT:
		return rx->skb->data[1];
	case HCI_ACLDATA_PKT:
		return get_unaligned_le16(rx->skb->data + 2);
	case HCI_SCODATA_PKT:
		return rx->skb->data[2];
	default:
		return UINT_MAX;
	}
}

typedef int (*mt6797_h4_deliver_t)(void *context, struct sk_buff *skb);

static int mt6797_h4_recv(struct mt6797_h4_rx *rx, const u8 *payload,
			  size_t length, mt6797_h4_deliver_t deliver,
			  void *context, gfp_t priority)
{
	unsigned int data_length;
	size_t count;
	int ret;

	if ((!payload && length) || !deliver)
		return -EINVAL;

	while (length) {
		if (!rx->skb) {
			ret = mt6797_h4_start(rx, *payload++, priority);
			length--;
			if (ret)
				return ret;
		}

		count = min_t(size_t, rx->expected_length - rx->skb->len,
			      length);
		skb_put_data(rx->skb, payload, count);
		payload += count;
		length -= count;
		if (rx->skb->len != rx->expected_length)
			continue;

		if (rx->skb->len == rx->header_length) {
			data_length = mt6797_h4_data_length(rx);
			if (data_length == UINT_MAX ||
			    data_length > skb_tailroom(rx->skb)) {
				mt6797_h4_reset(rx);
				return -EMSGSIZE;
			}
			rx->expected_length += data_length;
			if (data_length)
				continue;
		}

		ret = deliver(context, rx->skb);
		rx->skb = NULL;
		rx->header_length = 0;
		rx->expected_length = 0;
		if (ret)
			return ret;
	}

	return 0;
}

static int mt6797_stp_hci_deliver(void *context, struct sk_buff *skb)
{
	struct mt6797_stp_hci *data = context;

	/* Keep parser alignment before HCI core marks the device running. */
	if (!test_bit(HCI_RUNNING, &data->hdev->flags)) {
		kfree_skb(skb);
		return 0;
	}
	skb->dev = (void *)data->hdev;
	return hci_recv_frame(data->hdev, skb);
}

static int mt6797_stp_hci_receive(void *context, const u8 *payload,
				  size_t length)
{
	struct mt6797_stp_hci *data = context;
	int ret;

	if (!data)
		return -EINVAL;
	data->hdev->stat.byte_rx += length;
	mutex_lock(&data->rx_lock);
	ret = mt6797_h4_recv(&data->rx, payload, length, mt6797_stp_hci_deliver,
			     data, GFP_KERNEL);
	if (ret)
		mt6797_h4_reset(&data->rx);
	mutex_unlock(&data->rx_lock);
	if (ret)
		data->hdev->stat.err_rx++;
	return ret;
}

static void mt6797_stp_hci_tx_ready(void *context)
{
	struct mt6797_stp_hci *data = context;

	if (READ_ONCE(data->accepting_tx))
		mod_delayed_work(system_wq, &data->tx_work, 0);
}

static void mt6797_stp_hci_release_locked(struct mt6797_stp_hci *data,
					  bool *terminal)
{
	int ret;

	WRITE_ONCE(data->accepting_tx, false);

	if (data->bound) {
		ret = mt6797_consys_stp_session_unbind_bt(
			data->stp_lease, mt6797_stp_hci_receive,
			mt6797_stp_hci_tx_ready, data);
		if (ret && ret != -ESHUTDOWN)
			*terminal = true;
		data->bound = false;
	}

	if (data->function_on) {
		ret = mt6797_consys_runtime_controller_bt_function_set(
			&data->controller_lease, false);
		if (ret)
			*terminal = true;
		else
			data->function_on = false;
	}
	if (data->paldo_on) {
		ret = regulator_disable(data->paldo);
		if (ret)
			*terminal = true;
		else
			data->paldo_on = false;
	}
	if (data->controller_lease.controller)
		mt6797_consys_runtime_controller_bt_session_put(
			&data->controller_lease);
	data->stp_lease = NULL;
}

static int mt6797_stp_hci_open(struct hci_dev *hdev)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	unsigned int attempt;
	bool terminal = false;
	int ret;

	cancel_delayed_work_sync(&data->tx_work);
	skb_queue_purge(&data->txq);
	mutex_lock(&data->rx_lock);
	mt6797_h4_reset(&data->rx);
	mutex_unlock(&data->rx_lock);
	for (attempt = 0; ; attempt++) {
		mutex_lock(&data->lock);
		if (data->stp_lease) {
			ret = -EBUSY;
			goto unlock;
		}
		ret = mt6797_consys_runtime_controller_bt_session_get(
			data->controller, &data->controller_lease,
			&data->stp_lease);
		if (ret != -EAGAIN)
			break;
		mutex_unlock(&data->lock);
		if (attempt == MT6797_STP_HCI_START_RETRIES)
			return -ETIMEDOUT;
		if (msleep_interruptible(MT6797_STP_HCI_START_RETRY_MS))
			return -EINTR;
	}
	if (ret)
		goto unlock;
	ret = mt6797_consys_stp_session_bind_bt(data->stp_lease,
						mt6797_stp_hci_receive,
						mt6797_stp_hci_tx_ready, data);
	if (ret)
		goto release;
	data->bound = true;
	ret = regulator_enable(data->paldo);
	if (ret)
		goto release;
	data->paldo_on = true;
	ret = mt6797_consys_runtime_controller_bt_function_set(
		&data->controller_lease, true);
	if (ret) {
		/* A failed WMT transaction leaves BT function state unknowable. */
		terminal = true;
		goto release;
	}
	data->function_on = true;
	WRITE_ONCE(data->accepting_tx, true);
	mutex_unlock(&data->lock);
	return 0;
release:
	mt6797_stp_hci_release_locked(data, &terminal);
unlock:
	mutex_unlock(&data->lock);
	if (terminal)
		mt6797_consys_runtime_controller_stop(data->controller);
	return ret;
}

static int mt6797_stp_hci_close(struct hci_dev *hdev)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	bool terminal = false;

	mutex_lock(&data->lock);
	WRITE_ONCE(data->accepting_tx, false);
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->tx_work);
	skb_queue_purge(&data->txq);
	mutex_lock(&data->lock);
	mt6797_stp_hci_release_locked(data, &terminal);
	mutex_unlock(&data->lock);
	mutex_lock(&data->rx_lock);
	mt6797_h4_reset(&data->rx);
	mutex_unlock(&data->rx_lock);
	if (terminal)
		mt6797_consys_runtime_controller_stop(data->controller);
	return terminal ? -EIO : 0;
}

static int mt6797_stp_hci_flush(struct hci_dev *hdev)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);

	cancel_delayed_work_sync(&data->tx_work);
	skb_queue_purge(&data->txq);
	return 0;
}

static int mt6797_stp_hci_shutdown(struct hci_dev *hdev)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&data->lock);
	WRITE_ONCE(data->accepting_tx, false);
	mutex_unlock(&data->lock);
	cancel_delayed_work_sync(&data->tx_work);
	skb_queue_purge(&data->txq);

	mutex_lock(&data->lock);
	if (data->function_on) {
		ret = mt6797_consys_runtime_controller_bt_function_set(
			&data->controller_lease, false);
		if (!ret)
			data->function_on = false;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static void mt6797_stp_hci_count_tx(struct hci_dev *hdev,
				    const struct sk_buff *skb)
{
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
}

static void mt6797_stp_hci_tx_work(struct work_struct *work)
{
	struct mt6797_stp_hci *data = container_of(
		to_delayed_work(work), struct mt6797_stp_hci, tx_work);
	struct hci_dev *hdev = data->hdev;
	struct sk_buff *skb;
	bool failed = false;
	int ret;

	mutex_lock(&data->lock);
	while (READ_ONCE(data->accepting_tx) && data->function_on &&
	       data->stp_lease) {
		skb = skb_dequeue(&data->txq);
		if (!skb)
			break;
		ret = mt6797_consys_stp_session_transmit_bt(
			data->stp_lease, mt6797_stp_hci_receive, data,
			skb->data, skb->len);
		if (ret == -EAGAIN) {
			skb_queue_head(&data->txq, skb);
			mod_delayed_work(
				system_wq, &data->tx_work,
				msecs_to_jiffies(MT6797_STP_HCI_TX_RETRY_MS));
			break;
		}
		if (ret) {
			hdev->stat.err_tx++;
			kfree_skb(skb);
			WRITE_ONCE(data->accepting_tx, false);
			failed = true;
			break;
		}
		hdev->stat.byte_tx += skb->len;
		mt6797_stp_hci_count_tx(hdev, skb);
		kfree_skb(skb);
	}
	mutex_unlock(&data->lock);

	if (failed) {
		skb_queue_purge(&data->txq);
		if (test_bit(HCI_RUNNING, &hdev->flags))
			hci_reset_dev(hdev);
	}
}

static int mt6797_stp_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	u8 type = hci_skb_pkt_type(skb);

	if (!mt6797_stp_hci_tx_type_valid(type))
		return -EILSEQ;
	if (skb_cow_head(skb, 1))
		return -ENOMEM;
	*(u8 *)skb_push(skb, 1) = type;
	mutex_lock(&data->lock);
	if (!READ_ONCE(data->accepting_tx) || !data->function_on ||
	    !data->stp_lease) {
		mutex_unlock(&data->lock);
		return -ESHUTDOWN;
	}
	skb_queue_tail(&data->txq, skb);
	mutex_unlock(&data->lock);
	mod_delayed_work(system_wq, &data->tx_work, 0);
	return 0;
}

static int mt6797_stp_hci_probe(struct platform_device *pdev)
{
	struct mt6797_stp_hci *data;
	struct device_node *node;
	struct platform_device *controller;
	struct hci_dev *hdev;
	int ret;

	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing mediatek,controller\n");
	controller = of_find_device_by_node(node);
	of_node_put(node);
	if (!controller)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "CONSYS controller is not registered\n");
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		put_device(&controller->dev);
		return -ENOMEM;
	}
	data->controller = &controller->dev;
	if (!device_link_add(&pdev->dev, data->controller,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = dev_err_probe(&pdev->dev, -ENOMEM,
				    "failed to link CONSYS controller\n");
		goto put_controller;
	}
	data->paldo = devm_regulator_get(&pdev->dev, "vcn33-bt");
	if (IS_ERR(data->paldo)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(data->paldo),
				    "failed to get Bluetooth 3.3 V supply\n");
		goto put_controller;
	}
	ret = regulator_set_voltage(data->paldo, MT6797_STP_HCI_VCN33_UV,
				    MT6797_STP_HCI_VCN33_UV);
	if (ret) {
		ret = dev_err_probe(
			&pdev->dev, ret,
			"failed to select Bluetooth supply voltage\n");
		goto put_controller;
	}
	mutex_init(&data->lock);
	mutex_init(&data->rx_lock);
	skb_queue_head_init(&data->txq);
	INIT_DELAYED_WORK(&data->tx_work, mt6797_stp_hci_tx_work);
	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto put_controller;
	}
	data->hdev = hdev;
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, data);
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	hdev->open = mt6797_stp_hci_open;
	hdev->close = mt6797_stp_hci_close;
	hdev->flush = mt6797_stp_hci_flush;
	hdev->send = mt6797_stp_hci_send_frame;
	hdev->shutdown = mt6797_stp_hci_shutdown;
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LE_EVENT_MASK);
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LE_DATA_LEN);
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_MWS_TRANSPORT_CONFIG);
	ret = hci_register_dev(hdev);
	if (ret) {
		hci_free_dev(hdev);
		goto put_controller;
	}
	platform_set_drvdata(pdev, data);
	return 0;
put_controller:
	put_device(&controller->dev);
	return ret;
}

static void mt6797_stp_hci_remove(struct platform_device *pdev)
{
	struct mt6797_stp_hci *data = platform_get_drvdata(pdev);

	if (!data)
		return;
	hci_unregister_dev(data->hdev);
	cancel_delayed_work_sync(&data->tx_work);
	skb_queue_purge(&data->txq);
	mt6797_h4_reset(&data->rx);
	hci_free_dev(data->hdev);
	put_device(data->controller);
}

static const struct of_device_id mt6797_stp_hci_of_match[] = {
	{ .compatible = "mediatek,mt6797-bluetooth" },
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_stp_hci_of_match);
static struct platform_driver mt6797_stp_hci_driver = {
	.probe = mt6797_stp_hci_probe,
	.remove = mt6797_stp_hci_remove,
	.driver = {
		.name = "mt6797-stp-hci",
		.of_match_table = mt6797_stp_hci_of_match,
	},
};
module_platform_driver(mt6797_stp_hci_driver);
MODULE_DESCRIPTION("MT6797 Bluetooth HCI endpoint over full STP");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_BT_MTK6797_STP_KUNIT_TEST)
static void mt6797_stp_hci_test_packet_types(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mt6797_stp_hci_tx_type_valid(HCI_COMMAND_PKT));
	KUNIT_EXPECT_TRUE(test, mt6797_stp_hci_tx_type_valid(HCI_ACLDATA_PKT));
	KUNIT_EXPECT_TRUE(test, mt6797_stp_hci_tx_type_valid(HCI_SCODATA_PKT));
	KUNIT_EXPECT_FALSE(test, mt6797_stp_hci_tx_type_valid(HCI_EVENT_PKT));
}

struct mt6797_stp_hci_test_rx {
	unsigned int count;
	u8 type[2];
	size_t length[2];
	u8 payload[2][16];
};

static int mt6797_stp_hci_test_deliver(void *context, struct sk_buff *skb)
{
	struct mt6797_stp_hci_test_rx *received = context;
	unsigned int index = received->count++;

	if (index >= ARRAY_SIZE(received->type) ||
	    skb->len > sizeof(received->payload[index])) {
		kfree_skb(skb);
		return -ENOSPC;
	}
	received->type[index] = hci_skb_pkt_type(skb);
	received->length[index] = skb->len;
	memcpy(received->payload[index], skb->data, skb->len);
	kfree_skb(skb);
	return 0;
}

static void mt6797_stp_hci_test_fragmented_event(struct kunit *test)
{
	static const u8 event[] = {
		HCI_EVENT_PKT, HCI_EV_CMD_COMPLETE, 4, 1, 0x03, 0x0c, 0,
	};
	struct mt6797_stp_hci_test_rx received;
	struct mt6797_h4_rx rx;
	unsigned int split;
	int ret;

	for (split = 1; split < ARRAY_SIZE(event); split++) {
		received = (struct mt6797_stp_hci_test_rx){};
		rx = (struct mt6797_h4_rx){};
		ret = mt6797_h4_recv(&rx, event, split,
				     mt6797_stp_hci_test_deliver, &received,
				     GFP_KERNEL);
		KUNIT_ASSERT_EQ(test, ret, 0);
		ret = mt6797_h4_recv(&rx, event + split,
				     ARRAY_SIZE(event) - split,
				     mt6797_stp_hci_test_deliver, &received,
				     GFP_KERNEL);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, received.count, 1U);
		KUNIT_EXPECT_EQ(test, received.type[0], (u8)HCI_EVENT_PKT);
		KUNIT_EXPECT_EQ(test, received.length[0],
				ARRAY_SIZE(event) - 1);
		KUNIT_EXPECT_MEMEQ(test, received.payload[0], event + 1,
				   ARRAY_SIZE(event) - 1);
		mt6797_h4_reset(&rx);
	}
}

static void mt6797_stp_hci_test_coalesced_packets(struct kunit *test)
{
	static const u8 packets[] = {
		HCI_EVENT_PKT,
		HCI_EV_HARDWARE_ERROR,
		0,
		HCI_ACLDATA_PKT,
		1,
		0,
		3,
		0,
		0xaa,
		0xbb,
		0xcc,
	};
	struct mt6797_stp_hci_test_rx received = {};
	struct mt6797_h4_rx rx = {};
	int ret;

	ret = mt6797_h4_recv(&rx, packets, ARRAY_SIZE(packets),
			     mt6797_stp_hci_test_deliver, &received,
			     GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, received.count, 2U);
	KUNIT_EXPECT_EQ(test, received.type[0], (u8)HCI_EVENT_PKT);
	KUNIT_EXPECT_EQ(test, received.length[0], (size_t)HCI_EVENT_HDR_SIZE);
	KUNIT_EXPECT_EQ(test, received.type[1], (u8)HCI_ACLDATA_PKT);
	KUNIT_EXPECT_EQ(test, received.length[1], (size_t)7);
	KUNIT_EXPECT_MEMEQ(test, received.payload[1], packets + 4, 7);
	mt6797_h4_reset(&rx);
}

static void mt6797_stp_hci_test_malformed_and_reset(struct kunit *test)
{
	static const u8 invalid[] = { 0xff };
	static const u8 oversized_acl[] = {
		HCI_ACLDATA_PKT, 1, 0, 0xff, 0xff,
	};
	static const u8 partial[] = { HCI_EVENT_PKT, HCI_EV_CMD_COMPLETE };
	struct mt6797_stp_hci_test_rx received = {};
	struct mt6797_h4_rx rx = {};
	int ret;

	ret = mt6797_h4_recv(&rx, invalid, ARRAY_SIZE(invalid),
			     mt6797_stp_hci_test_deliver, &received,
			     GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -EILSEQ);
	KUNIT_EXPECT_PTR_EQ(test, rx.skb, NULL);
	ret = mt6797_h4_recv(&rx, oversized_acl, ARRAY_SIZE(oversized_acl),
			     mt6797_stp_hci_test_deliver, &received,
			     GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	KUNIT_EXPECT_PTR_EQ(test, rx.skb, NULL);
	ret = mt6797_h4_recv(&rx, partial, ARRAY_SIZE(partial),
			     mt6797_stp_hci_test_deliver, &received,
			     GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, rx.skb);
	mt6797_h4_reset(&rx);
	KUNIT_EXPECT_PTR_EQ(test, rx.skb, NULL);
}

static struct kunit_case mt6797_stp_hci_test_cases[] = {
	KUNIT_CASE(mt6797_stp_hci_test_packet_types),
	KUNIT_CASE(mt6797_stp_hci_test_fragmented_event),
	KUNIT_CASE(mt6797_stp_hci_test_coalesced_packets),
	KUNIT_CASE(mt6797_stp_hci_test_malformed_and_reset),
	{}
};

static struct kunit_suite mt6797_stp_hci_test_suite = {
	.name = "mt6797-stp-hci",
	.test_cases = mt6797_stp_hci_test_cases,
};

kunit_test_suite(mt6797_stp_hci_test_suite);
#endif
