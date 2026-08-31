// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendor hci_stp.c:1059-1268 retains H4 state across arbitrary STP delivery
 * and accepts Event, ACL and SCO. This proof collector follows that framing,
 * but applies strict caps and accepts only its armed Command Complete reply.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BT_H4_COLLECTOR_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-bt-h4-collector.h>

#define HCI_ACLDATA_PKT	0x02
#define HCI_SCODATA_PKT	0x03
#define HCI_EVENT_PKT		0x04
#define HCI_EV_CMD_COMPLETE	0x0e

static void mt6797_bt_h4_reset_parser(struct mt6797_bt_h4_collector *collector)
{
	collector->rx_state = MT6797_BT_H4_RX_TYPE;
	collector->packet_length = 0;
	collector->header_length = 0;
	collector->header_received = 0;
	collector->packet_expected = 0;
	collector->packet_type = 0;
}

void mt6797_bt_h4_collector_init(struct mt6797_bt_h4_collector *collector)
{
	if (!collector)
		return;
	memset(collector, 0, sizeof(*collector));
	spin_lock_init(&collector->lock);
	init_completion(&collector->command_complete);
	mt6797_bt_h4_reset_parser(collector);
}
EXPORT_SYMBOL_GPL(mt6797_bt_h4_collector_init);

static int mt6797_bt_h4_start_packet(struct mt6797_bt_h4_collector *collector,
				      u8 type)
{
	collector->packet_type = type;
	collector->packet[0] = type;
	collector->packet_length = 1;
	collector->header_received = 0;
	collector->rx_state = MT6797_BT_H4_RX_HEADER;
	switch (type) {
	case HCI_EVENT_PKT:
		collector->header_length = 2;
		break;
	case HCI_ACLDATA_PKT:
		collector->header_length = 4;
		break;
	case HCI_SCODATA_PKT:
		collector->header_length = 3;
		break;
	default:
		mt6797_bt_h4_reset_parser(collector);
		return -EPROTO;
	}
	return 0;
}

static int mt6797_bt_h4_finish_packet(struct mt6797_bt_h4_collector *collector)
{
	u16 opcode;
	u8 status;

	if (collector->packet_type != HCI_EVENT_PKT ||
	    collector->packet[1] != HCI_EV_CMD_COMPLETE) {
		collector->async_packets++;
		collector->async_bytes += collector->packet_length;
		mt6797_bt_h4_reset_parser(collector);
		return 0;
	}
	/* A Command Complete without num-packets/opcode/status is not async. */
	if (collector->packet[2] < 4)
		return -EPROTO;
	opcode = collector->packet[4] | ((u16)collector->packet[5] << 8);
	status = collector->packet[6];
	if (!collector->command_pending || opcode != collector->expected_opcode) {
		collector->async_packets++;
		collector->async_bytes += collector->packet_length;
		mt6797_bt_h4_reset_parser(collector);
		return 0;
	}
	memcpy(collector->reply, collector->packet, collector->packet_length);
	collector->reply_length = collector->packet_length;
	collector->command_result = status ? -EIO : 0;
	collector->command_pending = false;
	mt6797_bt_h4_reset_parser(collector);
	complete_all(&collector->command_complete);
	return 0;
}

static int mt6797_bt_h4_header_complete(struct mt6797_bt_h4_collector *collector)
{
	size_t body_length;

	switch (collector->packet_type) {
	case HCI_EVENT_PKT:
		body_length = collector->packet[2];
		if (body_length > MT6797_BT_H4_EVENT_MAX_PAYLOAD)
			return -EMSGSIZE;
		break;
	case HCI_ACLDATA_PKT:
		body_length = collector->packet[3] | ((size_t)collector->packet[4] << 8);
		if (body_length > MT6797_BT_H4_ACL_MAX_PAYLOAD)
			return -EMSGSIZE;
		break;
	case HCI_SCODATA_PKT:
		body_length = collector->packet[3];
		if (body_length > MT6797_BT_H4_SCO_MAX_PAYLOAD)
			return -EMSGSIZE;
		break;
	default:
		return -EPROTO;
	}
	collector->packet_expected = 1 + collector->header_length + body_length;
	if (collector->packet_expected > sizeof(collector->packet))
		return -EMSGSIZE;
	if (collector->packet_length == collector->packet_expected)
		return mt6797_bt_h4_finish_packet(collector);
	collector->rx_state = MT6797_BT_H4_RX_BODY;
	return 0;
}

int mt6797_bt_h4_collector_receive(struct mt6797_bt_h4_collector *collector,
				    const u8 *data, size_t length)
{
	unsigned long flags;
	size_t index;
	int ret = 0;

	if (!collector || (!data && length))
		return -EINVAL;
	spin_lock_irqsave(&collector->lock, flags);
	if (collector->fenced) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	for (index = 0; index < length; index++) {
		if (collector->rx_state == MT6797_BT_H4_RX_TYPE) {
			ret = mt6797_bt_h4_start_packet(collector, data[index]);
			if (ret)
				goto malformed;
			continue;
		}
		if (collector->packet_length >= sizeof(collector->packet)) {
			ret = -EMSGSIZE;
			goto malformed;
		}
		collector->packet[collector->packet_length++] = data[index];
		if (collector->rx_state == MT6797_BT_H4_RX_HEADER) {
			collector->header_received++;
			if (collector->header_received == collector->header_length) {
				ret = mt6797_bt_h4_header_complete(collector);
				if (ret)
					goto malformed;
			}
		} else if (collector->packet_length == collector->packet_expected) {
			ret = mt6797_bt_h4_finish_packet(collector);
			if (ret)
				goto malformed;
		}
	}
	goto out_unlock;
malformed:
	collector->malformed_packets++;
	mt6797_bt_h4_reset_parser(collector);
	/* Framing loss or a malformed command reply ends this one-shot exchange. */
	collector->fenced = true;
	if (collector->command_pending) {
		collector->command_pending = false;
		collector->command_result = ret;
		complete_all(&collector->command_complete);
	}
out_unlock:
	spin_unlock_irqrestore(&collector->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bt_h4_collector_receive);

int mt6797_bt_h4_collector_begin(struct mt6797_bt_h4_collector *collector,
				  u16 opcode)
{
	unsigned long flags;
	int ret = 0;

	if (!collector)
		return -EINVAL;
	spin_lock_irqsave(&collector->lock, flags);
	if (collector->fenced)
		ret = -ESHUTDOWN;
	else if (collector->command_pending)
		ret = -EBUSY;
	else {
		reinit_completion(&collector->command_complete);
		collector->expected_opcode = opcode;
		collector->command_result = -ETIMEDOUT;
		collector->reply_length = 0;
		collector->command_pending = true;
	}
	spin_unlock_irqrestore(&collector->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bt_h4_collector_begin);

int mt6797_bt_h4_collector_wait(struct mt6797_bt_h4_collector *collector,
				 unsigned long timeout, u8 *reply,
				 size_t reply_capacity, size_t *reply_length)
{
	unsigned long flags;
	int ret;

	if (!collector || !reply || !reply_length || !timeout)
		return -EINVAL;
	if (!wait_for_completion_timeout(&collector->command_complete, timeout)) {
		spin_lock_irqsave(&collector->lock, flags);
		if (collector->command_pending) {
			collector->command_pending = false;
			collector->command_result = -ETIMEDOUT;
			spin_unlock_irqrestore(&collector->lock, flags);
			return -ETIMEDOUT;
		}
		/* RX completed just as the timeout expired; consume that reply below. */
	} else {
		spin_lock_irqsave(&collector->lock, flags);
	}
	if (collector->fenced) {
		spin_unlock_irqrestore(&collector->lock, flags);
		return -ESHUTDOWN;
	}
	ret = collector->command_result;
	if (collector->reply_length > reply_capacity) {
		spin_unlock_irqrestore(&collector->lock, flags);
		return ret ?: -EMSGSIZE;
	}
	if (collector->reply_length) {
		memcpy(reply, collector->reply, collector->reply_length);
		*reply_length = collector->reply_length;
	}
	spin_unlock_irqrestore(&collector->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_bt_h4_collector_wait);

void mt6797_bt_h4_collector_fence(struct mt6797_bt_h4_collector *collector)
{
	unsigned long flags;

	if (!collector)
		return;
	spin_lock_irqsave(&collector->lock, flags);
	collector->fenced = true;
	mt6797_bt_h4_reset_parser(collector);
	collector->command_pending = false;
	collector->command_result = -ESHUTDOWN;
	complete_all(&collector->command_complete);
	spin_unlock_irqrestore(&collector->lock, flags);
}
EXPORT_SYMBOL_GPL(mt6797_bt_h4_collector_fence);

#if IS_ENABLED(CONFIG_MTK_MT6797_BT_H4_COLLECTOR_KUNIT_TEST)
static const u8 hci_reset_complete[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
static const u8 hci_version_complete[] = {
	0x04, 0x0e, 0x0c, 0x01, 0x01, 0x10, 0x00, 0x0b,
	0x34, 0x12, 0x0b, 0x46, 0x00, 0x78, 0x56,
};

static void mt6797_bt_h4_test_split_and_coalesced(struct kunit *test)
{
	struct mt6797_bt_h4_collector collector;
	u8 reply[sizeof(hci_version_complete)];
	u8 coalesced[sizeof(hci_reset_complete) + sizeof(hci_version_complete)];
	size_t reply_length = 0;

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), 0);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, hci_reset_complete, 2), 0);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, hci_reset_complete + 2, sizeof(hci_reset_complete) - 2), 0);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply, sizeof(reply), &reply_length), 0);
	KUNIT_EXPECT_EQ(test, reply_length, sizeof(hci_reset_complete));
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x1001), 0);
	memcpy(coalesced, hci_reset_complete, sizeof(hci_reset_complete));
	memcpy(coalesced + sizeof(hci_reset_complete), hci_version_complete,
	       sizeof(hci_version_complete));
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, coalesced,
			 sizeof(coalesced)), 0);
	KUNIT_EXPECT_EQ(test, collector.async_packets, 1ULL);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply, sizeof(reply), &reply_length), 0);
	KUNIT_EXPECT_EQ(test, reply_length, sizeof(hci_version_complete));
}

static void mt6797_bt_h4_test_async_and_status(struct kunit *test)
{
	struct mt6797_bt_h4_collector collector;
	u8 reply[sizeof(hci_version_complete)];
	u8 async[] = { 0x04, 0xff, 0x01, 0x55 };
	u8 failed[sizeof(hci_reset_complete)];
	size_t reply_length = 0;

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), 0);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, async, sizeof(async)), 0);
	KUNIT_EXPECT_EQ(test, collector.async_packets, 1ULL);
	memcpy(failed, hci_reset_complete, sizeof(failed));
	failed[6] = 1;
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, failed, sizeof(failed)), 0);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply, sizeof(reply), &reply_length), -EIO);
	KUNIT_EXPECT_EQ(test, reply_length, sizeof(failed));
	KUNIT_EXPECT_EQ(test, reply[6], 1);
}

static void mt6797_bt_h4_test_malformed_and_large_acl(struct kunit *test)
{
	struct mt6797_bt_h4_collector collector;
	u8 acl[1 + 4 + 300] = { 0x02, 0x01, 0x00, 0x2c, 0x01 };
	u8 oversize[] = { 0x02, 0x01, 0x00, 0x01, 0x08 };

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_receive(&collector, oversize, sizeof(oversize)), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, collector.malformed_packets, 1ULL);
	KUNIT_EXPECT_TRUE(test, collector.fenced);
	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, acl, 256), 0);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, acl + 256, sizeof(acl) - 256), 0);
	KUNIT_EXPECT_EQ(test, collector.async_packets, 1ULL);
	KUNIT_EXPECT_EQ(test, collector.async_bytes, (u64)sizeof(acl));
}

static void mt6797_bt_h4_test_timeout_and_fence(struct kunit *test)
{
	struct mt6797_bt_h4_collector collector;
	struct mt6797_bt_h4_collector *fenced;
	u8 reply[sizeof(hci_reset_complete)];
	size_t reply_length = 0;

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), 0);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply, sizeof(reply), &reply_length), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, collector.command_pending);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector, hci_reset_complete, sizeof(hci_reset_complete)), 0);
	KUNIT_EXPECT_EQ(test, collector.async_packets, 1ULL);
	mt6797_bt_h4_collector_fence(&collector);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_receive(&collector, hci_reset_complete, sizeof(hci_reset_complete)), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), -ESHUTDOWN);
	fenced = kunit_kzalloc(test, sizeof(*fenced), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fenced);
	mt6797_bt_h4_collector_init(fenced);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(fenced, 0x0c03), 0);
	mt6797_bt_h4_collector_fence(fenced);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(fenced, 1, reply,
			 sizeof(reply), &reply_length), -ESHUTDOWN);
}

static void mt6797_bt_h4_test_malformed_complete_and_completed_fence(struct kunit *test)
{
	struct mt6797_bt_h4_collector collector;
	u8 reply[sizeof(hci_reset_complete)];
	u8 short_complete[] = { 0x04, 0x0e, 0x03, 0x01, 0x03, 0x0c };
	size_t reply_length = 0;

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), 0);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_receive(&collector, short_complete,
			 sizeof(short_complete)), -EPROTO);
	KUNIT_EXPECT_TRUE(test, collector.fenced);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply,
			 sizeof(reply), &reply_length), -ESHUTDOWN);

	mt6797_bt_h4_collector_init(&collector);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_begin(&collector, 0x0c03), 0);
	KUNIT_ASSERT_EQ(test, mt6797_bt_h4_collector_receive(&collector,
			hci_reset_complete, sizeof(hci_reset_complete)), 0);
	mt6797_bt_h4_collector_fence(&collector);
	KUNIT_EXPECT_EQ(test, mt6797_bt_h4_collector_wait(&collector, 1, reply,
			 sizeof(reply), &reply_length), -ESHUTDOWN);
}

static struct kunit_case mt6797_bt_h4_collector_cases[] = {
	KUNIT_CASE(mt6797_bt_h4_test_split_and_coalesced),
	KUNIT_CASE(mt6797_bt_h4_test_async_and_status),
	KUNIT_CASE(mt6797_bt_h4_test_malformed_and_large_acl),
	KUNIT_CASE(mt6797_bt_h4_test_timeout_and_fence),
	KUNIT_CASE(mt6797_bt_h4_test_malformed_complete_and_completed_fence), {}
};
static struct kunit_suite mt6797_bt_h4_collector_suite = {
	.name = "mt6797-bt-h4-collector", .test_cases = mt6797_bt_h4_collector_cases,
};
kunit_test_suite(mt6797_bt_h4_collector_suite);
#endif

MODULE_DESCRIPTION("Bounded MT6797 Bluetooth H4 command collector");
MODULE_LICENSE("GPL");
