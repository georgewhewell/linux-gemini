// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 HCI over the exclusive controller-owned full-STP channel zero. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#if IS_ENABLED(CONFIG_BT_MTK6797_STP_KUNIT_TEST)
#include <kunit/test.h>
#endif
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <linux/soc/mediatek/mt6797-consys-runtime-controller.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>

static bool enable_experimental_stp_hci;
module_param_named(enable_experimental_stp_hci, enable_experimental_stp_hci,
			   bool, 0400);
MODULE_PARM_DESC(enable_experimental_stp_hci,
	"allow the gated MT6797 full-STP Bluetooth HCI endpoint (default: false)");

struct mt6797_stp_hci {
	struct device *controller;
	struct regulator *paldo;
	struct hci_dev *hdev;
	struct mutex lock;
	struct mt6797_consys_runtime_controller_bt_lease controller_lease;
	const struct mt6797_consys_stp_session_lease *stp_lease;
	bool bound, paldo_on, function_on;
};

static bool mt6797_stp_hci_tx_type_valid(u8 type)
{
	return type == HCI_COMMAND_PKT || type == HCI_ACLDATA_PKT ||
		type == HCI_SCODATA_PKT;
}

static int mt6797_stp_hci_receive(void *context, const u8 *payload, size_t length)
{
	struct mt6797_stp_hci *data = context;
	struct sk_buff *skb;
	u8 type;

	if (!data || !payload || length < 2)
		return -EPROTO;
	/* HCI has not reached RUNNING until open returns: consume early events. */
	if (!test_bit(HCI_RUNNING, &data->hdev->flags))
		return 0;
	type = payload[0];
	if (type != HCI_EVENT_PKT && type != HCI_ACLDATA_PKT &&
	    type != HCI_SCODATA_PKT)
		return -EPROTO;
	skb = bt_skb_alloc(length - 1, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, payload + 1, length - 1);
	skb->dev = (void *)data->hdev;
	hci_skb_pkt_type(skb) = type;
	return hci_recv_frame(data->hdev, skb);
}

static void mt6797_stp_hci_release_locked(struct mt6797_stp_hci *data,
					   bool *terminal)
{
	int ret;

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
	if (data->bound) {
		ret = mt6797_consys_stp_session_unbind_bt(data->stp_lease,
			mt6797_stp_hci_receive, data);
		if (ret && ret != -ESHUTDOWN)
			*terminal = true;
		data->bound = false;
	}
	if (data->controller_lease.controller)
		mt6797_consys_runtime_controller_bt_session_put(&data->controller_lease);
	data->stp_lease = NULL;
}

static int mt6797_stp_hci_open(struct hci_dev *hdev)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	bool terminal = false;
	int ret;

	mutex_lock(&data->lock);
	if (data->stp_lease) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = mt6797_consys_runtime_controller_bt_session_get(data->controller,
		&data->controller_lease, &data->stp_lease);
	if (ret)
		goto unlock;
	ret = mt6797_consys_stp_session_bind_bt(data->stp_lease,
		mt6797_stp_hci_receive, data);
	if (ret)
		goto release;
	data->bound = true;
	ret = regulator_enable(data->paldo);
	if (ret)
		goto release;
	data->paldo_on = true;
	ret = mt6797_consys_runtime_controller_bt_function_set(
		&data->controller_lease, true);
	if (ret)
		goto release;
	data->function_on = true;
	mutex_unlock(&data->lock);
	return 0;
release:
	terminal = true;
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
	mt6797_stp_hci_release_locked(data, &terminal);
	mutex_unlock(&data->lock);
	if (terminal)
		mt6797_consys_runtime_controller_stop(data->controller);
	return terminal ? -EIO : 0;
}

static int mt6797_stp_hci_flush(struct hci_dev *hdev)
{
	return 0;
}

static int mt6797_stp_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mt6797_stp_hci *data = hci_get_drvdata(hdev);
	u8 type = hci_skb_pkt_type(skb);
	int ret;

	if (!mt6797_stp_hci_tx_type_valid(type)) {
		ret = -EILSEQ;
		goto free;
	}
	if (skb_cow_head(skb, 1)) {
		ret = -ENOMEM;
		goto free;
	}
	*(u8 *)skb_push(skb, 1) = type;
	mutex_lock(&data->lock);
	ret = data->function_on && data->stp_lease ?
		mt6797_consys_stp_session_transmit(data->stp_lease, MTK_STP_BT,
			skb->data, skb->len) : -ESHUTDOWN;
	mutex_unlock(&data->lock);
	if (ret)
		hdev->stat.err_tx++;
	else
		hdev->stat.byte_tx += skb->len;
free:
	kfree_skb(skb);
	return ret;
}

static int mt6797_stp_hci_probe(struct platform_device *pdev)
{
	struct mt6797_stp_hci *data;
	struct device_node *node;
	struct platform_device *controller;
	struct hci_dev *hdev;
	int ret;

	if (!READ_ONCE(enable_experimental_stp_hci) ||
	    !device_property_read_bool(&pdev->dev, "mediatek,experimental-stp-hci"))
		return -EPERM;
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,controller", 0);
	if (!node)
		return -EINVAL;
	controller = of_find_device_by_node(node);
	of_node_put(node);
	if (!controller)
		return -EPROBE_DEFER;
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		put_device(&controller->dev);
		return -ENOMEM;
	}
	data->controller = &controller->dev;
	data->paldo = devm_regulator_get(&pdev->dev, "vcn33-bt");
	if (IS_ERR(data->paldo)) {
		ret = PTR_ERR(data->paldo);
		goto put_controller;
	}
	mutex_init(&data->lock);
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
	mt6797_stp_hci_close(data->hdev);
	hci_free_dev(data->hdev);
	put_device(data->controller);
}

static const struct of_device_id mt6797_stp_hci_of_match[] = {
	{ .compatible = "mediatek,mt6797-stp-hci" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_stp_hci_of_match);
static struct platform_driver mt6797_stp_hci_driver = {
	.probe = mt6797_stp_hci_probe,
	.remove = mt6797_stp_hci_remove,
	.driver = { .name = "mt6797-stp-hci", .of_match_table = mt6797_stp_hci_of_match },
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
static struct kunit_case mt6797_stp_hci_test_cases[] = {
	KUNIT_CASE(mt6797_stp_hci_test_packet_types), {}
};
static struct kunit_suite mt6797_stp_hci_test_suite = {
	.name = "mt6797-stp-hci", .test_cases = mt6797_stp_hci_test_cases,
};
kunit_test_suite(mt6797_stp_hci_test_suite);
#endif
