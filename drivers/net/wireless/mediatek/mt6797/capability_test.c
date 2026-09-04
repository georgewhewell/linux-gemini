// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "capability.h"
#include "protocol.h"

enum mt6797_wifi_capability_test_event {
	TEST_PRECHECK,
	TEST_ARM,
	TEST_LINUX_ENABLE,
	TEST_DEVICE_ENABLE,
	TEST_HIF_UNMASK,
	TEST_SEND,
	TEST_WAIT,
	TEST_DISARM,
	TEST_HIF_MASK,
	TEST_DEVICE_DISABLE,
	TEST_LINUX_DISABLE,
	TEST_FENCE,
};

struct mt6797_wifi_capability_test_context {
	struct kunit *test;
	enum mt6797_wifi_capability_test_event event[16];
	enum mt6797_wifi_capability_test_event fail_at;
	unsigned int event_count;
	int precheck_result;
};

static int
mt6797_wifi_capability_test_record(struct mt6797_wifi_capability_test_context *context,
				   enum mt6797_wifi_capability_test_event event)
{
	KUNIT_ASSERT_LT(context->test, context->event_count,
			(unsigned int)ARRAY_SIZE(context->event));
	context->event[context->event_count++] = event;
	return context->fail_at == event ? -EIO : 0;
}

static int mt6797_wifi_capability_test_precheck(void *data)
{
	struct mt6797_wifi_capability_test_context *context = data;

	mt6797_wifi_capability_test_record(context, TEST_PRECHECK);
	return context->precheck_result;
}

#define MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(_name, _event) \
	static int mt6797_wifi_capability_test_##_name(void *data) \
	{ \
		return mt6797_wifi_capability_test_record(data, _event); \
	}

#define MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(_name, _event) \
	static void mt6797_wifi_capability_test_##_name(void *data) \
	{ \
		mt6797_wifi_capability_test_record(data, _event); \
	}

MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(arm, TEST_ARM)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(linux_enable, TEST_LINUX_ENABLE)
MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(device_enable, TEST_DEVICE_ENABLE)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(hif_unmask, TEST_HIF_UNMASK)
MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(send, TEST_SEND)
MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(wait, TEST_WAIT)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(disarm, TEST_DISARM)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(hif_mask, TEST_HIF_MASK)
MT6797_WIFI_CAPABILITY_TEST_INT_CALLBACK(device_disable, TEST_DEVICE_DISABLE)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(linux_disable, TEST_LINUX_DISABLE)
MT6797_WIFI_CAPABILITY_TEST_VOID_CALLBACK(fence, TEST_FENCE)

static void
mt6797_wifi_capability_test_init_ops(struct mt6797_wifi_capability_test_context *context,
				     struct mt6797_wifi_capability_ops *ops)
{
	*ops = (struct mt6797_wifi_capability_ops) {
		.precheck = mt6797_wifi_capability_test_precheck,
		.arm = mt6797_wifi_capability_test_arm,
		.enable_linux_irq = mt6797_wifi_capability_test_linux_enable,
		.enable_device_irq = mt6797_wifi_capability_test_device_enable,
		.unmask_hif_irq = mt6797_wifi_capability_test_hif_unmask,
		.send = mt6797_wifi_capability_test_send,
		.wait = mt6797_wifi_capability_test_wait,
		.disarm = mt6797_wifi_capability_test_disarm,
		.mask_hif_irq = mt6797_wifi_capability_test_hif_mask,
		.disable_device_irq = mt6797_wifi_capability_test_device_disable,
		.disable_linux_irq = mt6797_wifi_capability_test_linux_disable,
		.fence = mt6797_wifi_capability_test_fence,
		.context = context,
	};
}

static void
mt6797_wifi_capability_test_expect(struct kunit *test,
				   struct mt6797_wifi_capability_test_context *context,
				   const enum mt6797_wifi_capability_test_event *expected,
				   size_t count)
{
	unsigned int index;

	KUNIT_ASSERT_EQ(test, context->event_count, (unsigned int)count);
	for (index = 0; index < count; index++)
		KUNIT_EXPECT_EQ(test, context->event[index], expected[index]);
}

static void mt6797_wifi_capability_codec_test(struct kunit *test)
{
	static const u8 mac_address[] = { 0x02, 0x79, 0x67, 0, 0, 1 };
	struct mt6797_wifi_capability capability;
	u8 packet[MT6797_WIFI_CAPABILITY_LENGTH];
	int ret;

	ret = mt6797_wifi_capability_command_build(packet, sizeof(packet), 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet), 124U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 2), 0x8000U);
	KUNIT_EXPECT_EQ(test, packet[4], 0x80U);
	KUNIT_EXPECT_EQ(test, packet[5], 0xa0U);
	KUNIT_EXPECT_EQ(test, packet[6], 0U);
	KUNIT_EXPECT_EQ(test, packet[7], 1U);

	memset(packet, 0, sizeof(packet));
	put_unaligned_le16(sizeof(packet), packet);
	put_unaligned_le16(0xe000, packet + 2);
	packet[4] = 1;
	packet[5] = 1;
	put_unaligned_le16(0x6631, packet + 8);
	put_unaligned_le16(0x1234, packet + 10);
	put_unaligned_le16(0x5678, packet + 12);
	memcpy(packet + 16, mac_address, sizeof(mac_address));
	put_unaligned_le32(0x11223344, packet + 44);
	put_unaligned_le32(0x55667788, packet + 48);
	ret = mt6797_wifi_capability_event_parse(packet, sizeof(packet), 1,
						 &capability);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, capability.product_id, 0x6631U);
	KUNIT_EXPECT_EQ(test, capability.firmware_version, 0x1234U);
	KUNIT_EXPECT_EQ(test, capability.driver_version, 0x5678U);
	KUNIT_EXPECT_MEMEQ(test, capability.mac_address, mac_address,
			   sizeof(mac_address));
	KUNIT_EXPECT_EQ(test, capability.feature_flag0, 0x11223344U);
	KUNIT_EXPECT_EQ(test, capability.feature_flag1, 0x55667788U);
	packet[4] = 2;
	KUNIT_EXPECT_EQ(test,
			mt6797_wifi_capability_event_parse(packet, sizeof(packet), 1,
							   &capability), -EPROTO);
	packet[4] = 1;
	put_unaligned_le16(MT6797_WIFI_EVENT_HEADER_LENGTH, packet);
	ret = mt6797_wifi_capability_event_parse(packet, sizeof(packet), 1,
						 &capability);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_wifi_capability_success_test(struct kunit *test)
{
	static const enum mt6797_wifi_capability_test_event expected[] = {
		TEST_PRECHECK, TEST_ARM, TEST_HIF_MASK, TEST_DEVICE_ENABLE,
		TEST_LINUX_ENABLE, TEST_HIF_UNMASK, TEST_SEND, TEST_WAIT, TEST_DISARM,
		TEST_HIF_MASK,
		TEST_LINUX_DISABLE, TEST_DEVICE_DISABLE,
	};
	struct mt6797_wifi_capability_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
	};
	struct mt6797_wifi_capability_ops ops;
	int ret;

	mt6797_wifi_capability_test_init_ops(&context, &ops);
	ret = mt6797_wifi_capability_transaction(&ops);
	KUNIT_EXPECT_EQ(test, ret, 0);
	mt6797_wifi_capability_test_expect(test, &context, expected,
					   ARRAY_SIZE(expected));
}

static void mt6797_wifi_capability_send_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_capability_test_event expected[] = {
		TEST_PRECHECK, TEST_ARM, TEST_HIF_MASK, TEST_DEVICE_ENABLE,
		TEST_LINUX_ENABLE, TEST_HIF_UNMASK, TEST_SEND, TEST_DISARM,
		TEST_HIF_MASK,
		TEST_LINUX_DISABLE, TEST_DEVICE_DISABLE, TEST_FENCE,
	};
	struct mt6797_wifi_capability_test_context context = {
		.test = test,
		.fail_at = TEST_SEND,
	};
	struct mt6797_wifi_capability_ops ops;
	int ret;

	mt6797_wifi_capability_test_init_ops(&context, &ops);
	ret = mt6797_wifi_capability_transaction(&ops);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	mt6797_wifi_capability_test_expect(test, &context, expected,
					   ARRAY_SIZE(expected));
}

static void mt6797_wifi_capability_precheck_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_capability_test_event expected[] = {
		TEST_PRECHECK, TEST_FENCE,
	};
	struct mt6797_wifi_capability_test_context context = {
		.test = test,
		.fail_at = TEST_FENCE,
		.precheck_result = -EUCLEAN,
	};
	struct mt6797_wifi_capability_ops ops;
	int ret;

	mt6797_wifi_capability_test_init_ops(&context, &ops);
	ret = mt6797_wifi_capability_transaction(&ops);
	KUNIT_EXPECT_EQ(test, ret, -EUCLEAN);
	mt6797_wifi_capability_test_expect(test, &context, expected,
					   ARRAY_SIZE(expected));
}

static void mt6797_wifi_capability_enable_failure_test(struct kunit *test)
{
	static const enum mt6797_wifi_capability_test_event expected[] = {
		TEST_PRECHECK, TEST_ARM, TEST_HIF_MASK, TEST_DEVICE_ENABLE,
		TEST_DISARM, TEST_HIF_MASK, TEST_DEVICE_DISABLE,
	};
	struct mt6797_wifi_capability_test_context context = {
		.test = test,
		.fail_at = TEST_DEVICE_ENABLE,
	};
	struct mt6797_wifi_capability_ops ops;
	int ret;

	mt6797_wifi_capability_test_init_ops(&context, &ops);
	ret = mt6797_wifi_capability_transaction(&ops);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	mt6797_wifi_capability_test_expect(test, &context, expected,
					   ARRAY_SIZE(expected));
}

static void mt6797_wifi_capability_cleanup_failure_test(struct kunit *test)
{
	struct mt6797_wifi_capability_test_context context = {
		.test = test,
		.fail_at = TEST_DEVICE_DISABLE,
	};
	struct mt6797_wifi_capability_ops ops;
	int ret;

	mt6797_wifi_capability_test_init_ops(&context, &ops);
	ret = mt6797_wifi_capability_transaction(&ops);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_ASSERT_GE(test, context.event_count, 3U);
	KUNIT_EXPECT_EQ(test, context.event[context.event_count - 2],
			TEST_DEVICE_DISABLE);
	KUNIT_EXPECT_EQ(test, context.event[context.event_count - 1], TEST_FENCE);
}

static struct kunit_case mt6797_wifi_capability_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_capability_codec_test),
	KUNIT_CASE(mt6797_wifi_capability_success_test),
	KUNIT_CASE(mt6797_wifi_capability_send_failure_test),
	KUNIT_CASE(mt6797_wifi_capability_precheck_failure_test),
	KUNIT_CASE(mt6797_wifi_capability_enable_failure_test),
	KUNIT_CASE(mt6797_wifi_capability_cleanup_failure_test),
	{ }
};

static struct kunit_suite mt6797_wifi_capability_test_suite = {
	.name = "mt6797-wifi-capability",
	.test_cases = mt6797_wifi_capability_test_cases,
};

kunit_test_suite(mt6797_wifi_capability_test_suite);

MODULE_LICENSE("GPL");
