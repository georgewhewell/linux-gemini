// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "protocol.h"

static void mt6797_ccci_round_trip_test(struct kunit *test)
{
	const struct mt6797_ccci_header_fields input = {
		.data = { 0x12345678, 0x90abcdef },
		.channel = 0x2468,
		.sequence = 0x4567,
		.assert = true,
		.reserved = 0x55aa00ff,
	};
	struct mt6797_ccci_header_fields output = {};
	struct mt6797_ccci_header header = {};

	KUNIT_ASSERT_EQ(test, mt6797_ccci_header_encode(&header, &input), 0);
	KUNIT_EXPECT_EQ(test, header.data[0], cpu_to_le32(0x12345678));
	KUNIT_EXPECT_EQ(test, header.data[1], cpu_to_le32(0x90abcdef));
	KUNIT_EXPECT_EQ(test, header.channel, cpu_to_le16(0x2468));
	KUNIT_EXPECT_EQ(test, header.sequence_assert, cpu_to_le16(0xc567));
	KUNIT_EXPECT_EQ(test, header.reserved, cpu_to_le32(0x55aa00ff));

	KUNIT_ASSERT_EQ(test, mt6797_ccci_header_decode(&header, &output), 0);
	KUNIT_EXPECT_EQ(test, output.data[0], input.data[0]);
	KUNIT_EXPECT_EQ(test, output.data[1], input.data[1]);
	KUNIT_EXPECT_EQ(test, output.channel, input.channel);
	KUNIT_EXPECT_EQ(test, output.sequence, input.sequence);
	KUNIT_EXPECT_EQ(test, output.assert, input.assert);
	KUNIT_EXPECT_EQ(test, output.reserved, input.reserved);
}

static void mt6797_ccci_rejects_invalid_sequence_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields fields = {
		.sequence = MT6797_CCCI_SEQUENCE_MAX + 1,
	};
	struct mt6797_ccci_header header = {};

	KUNIT_EXPECT_EQ(test, mt6797_ccci_header_encode(&header, &fields),
			-ERANGE);
}

static void mt6797_cldma_address_test(struct kunit *test)
{
	const u64 data_address = 0x0a12345678;
	const u64 next_address = 0x0bcdef0123;
	__le16 debug_id = cpu_to_le16(0xa55a);
	__le32 data_low = 0;
	__le32 next_low = 0;
	u64 decoded;
	int ret;

	ret = mt6797_cldma_address_encode(data_address,
					  MT6797_CLDMA_POINTER_DATA,
					  &data_low, &debug_id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, data_low, cpu_to_le32(0x12345678));
	KUNIT_EXPECT_EQ(test, debug_id, cpu_to_le16(0xa55a));

	ret = mt6797_cldma_address_encode(next_address,
					  MT6797_CLDMA_POINTER_NEXT,
					  &next_low, &debug_id);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, next_low, cpu_to_le32(0xcdef0123));
	KUNIT_EXPECT_EQ(test, debug_id, cpu_to_le16(0xa5ba));

	ret = mt6797_cldma_address_decode(data_low, debug_id,
					  MT6797_CLDMA_POINTER_DATA,
					  &decoded);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, decoded, data_address);
	ret = mt6797_cldma_address_decode(next_low, debug_id,
					  MT6797_CLDMA_POINTER_NEXT,
					  &decoded);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, decoded, next_address);
}

static void mt6797_cldma_rejects_invalid_address_test(struct kunit *test)
{
	__le16 debug_id = cpu_to_le16(0x55aa);
	__le32 low = cpu_to_le32(0x12345678);
	int ret;

	ret = mt6797_cldma_address_encode(BIT_ULL(36),
					  MT6797_CLDMA_POINTER_DATA,
					  &low, &debug_id);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	KUNIT_EXPECT_EQ(test, low, cpu_to_le32(0x12345678));
	KUNIT_EXPECT_EQ(test, debug_id, cpu_to_le16(0x55aa));
}

static void mt6797_cldma_rejects_invalid_pointer_test(struct kunit *test)
{
	__le16 debug_id = 0;
	__le32 low = 0;
	u64 address;
	int ret;

	ret = mt6797_cldma_address_encode(0, -1, &low, &debug_id);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = mt6797_cldma_address_decode(low, debug_id, -1, &address);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct mt6797_ccci_header_fields mt6797_ccci_control(u32 message,
							    u32 reserved)
{
	return (struct mt6797_ccci_header_fields) {
		.channel = MT6797_CCCI_CONTROL_RX,
		.data = { 0, message },
		.reserved = reserved,
	};
}

static struct mt6797_ccci_header_fields mt6797_ccci_hs1(void)
{
	return mt6797_ccci_control(MT6797_CCCI_MD_INIT_START_BOOT,
				   MT6797_CCCI_MD_INIT_CHECK_ID);
}

static void mt6797_ccci_boot_happy_path_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields hs1 = mt6797_ccci_hs1();
	struct mt6797_ccci_header_fields hs2 =
		mt6797_ccci_control(MT6797_CCCI_MD_INIT_START_BOOT, 0x369cd);
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;

	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_WAIT_HS1);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &hs1),
			MT6797_CCCI_BOOT_EVENT_HS1);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_SEND_RUNTIME);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_runtime_sent(&state), 0);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_WAIT_HS2);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &hs2),
			MT6797_CCCI_BOOT_EVENT_READY);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_READY);
}

static void mt6797_ccci_boot_rejects_out_of_order_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields hs2 =
		mt6797_ccci_control(MT6797_CCCI_MD_INIT_START_BOOT, 0x369cd);
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;

	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &hs2),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);

	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_runtime_sent(&state), -EPROTO);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);
}

static void mt6797_ccci_boot_ignores_other_channels_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields message = mt6797_ccci_hs1();
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;

	message.channel = 1;
	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &message),
			-ENOMSG);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_WAIT_HS1);
}

static void mt6797_ccci_boot_fences_exception_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields exception =
		mt6797_ccci_control(MT6797_CCCI_MD_EXCEPTION, 0);
	struct mt6797_ccci_header_fields version_error =
		mt6797_ccci_control(MT6797_CCCI_DRIVER_VERSION_ERROR, 0);
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;

	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &exception),
			-EIO);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);

	mt6797_ccci_boot_begin(&state);
	KUNIT_EXPECT_EQ(test, mt6797_ccci_boot_receive(&state, &version_error),
			-EPROTONOSUPPORT);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);
}

static void mt6797_ccci_boot_control_sequence_test(struct kunit *test)
{
	struct mt6797_ccci_rx_sequence sequence = {};
	struct mt6797_ccci_header_fields hs1 = mt6797_ccci_hs1();
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;
	int ret;

	hs1.assert = true;
	mt6797_ccci_boot_begin(&state);
	ret = mt6797_ccci_boot_rx(&state, &sequence, &hs1);
	KUNIT_EXPECT_EQ(test, ret, MT6797_CCCI_BOOT_EVENT_HS1);
	KUNIT_EXPECT_TRUE(test, sequence.valid);
	KUNIT_EXPECT_EQ(test, sequence.last, (u16)0);

	mt6797_ccci_boot_begin(&state);
	sequence = (struct mt6797_ccci_rx_sequence) {
		.last = 0,
		.valid = true,
	};
	hs1.sequence = 2;
	ret = mt6797_ccci_boot_rx(&state, &sequence, &hs1);
	KUNIT_EXPECT_EQ(test, ret, -EILSEQ);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);
}

static void mt6797_ccci_boot_control_rejects_channel_test(struct kunit *test)
{
	struct mt6797_ccci_rx_sequence sequence = {};
	struct mt6797_ccci_header_fields hs1 = mt6797_ccci_hs1();
	enum mt6797_ccci_boot_state state = MT6797_CCCI_BOOT_OFFLINE;
	int ret;

	hs1.channel = MT6797_CCCI_UART2_RX;
	mt6797_ccci_boot_begin(&state);
	ret = mt6797_ccci_boot_rx(&state, &sequence, &hs1);
	KUNIT_EXPECT_EQ(test, ret, -ENOMSG);
	KUNIT_EXPECT_EQ(test, state, MT6797_CCCI_BOOT_FAILED);
	KUNIT_EXPECT_FALSE(test, sequence.valid);
}

static int mt6797_ccci_uart2_test_gate(enum mt6797_ccci_boot_state state,
				       bool rx_owned, bool tx_owned)
{
	return mt6797_ccci_uart2_gate(state, MT6797_CCCI_UART2_RX,
					MT6797_CCCI_UART2_TX,
					MT6797_CCCI_UART2_QUEUE,
					MT6797_CCCI_UART2_QUEUE,
					rx_owned, tx_owned);
}

static void mt6797_ccci_uart2_port_gate_test(struct kunit *test)
{
	int ret;

	ret = mt6797_ccci_uart2_test_gate(MT6797_CCCI_BOOT_WAIT_HS2,
					  true, true);
	KUNIT_EXPECT_EQ(test, ret, -EHOSTDOWN);
	ret = mt6797_ccci_uart2_test_gate(MT6797_CCCI_BOOT_READY,
					  false, true);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
	ret = mt6797_ccci_uart2_test_gate(MT6797_CCCI_BOOT_READY,
					  true, false);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
	ret = mt6797_ccci_uart2_test_gate(MT6797_CCCI_BOOT_READY, true, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = mt6797_ccci_uart2_gate(MT6797_CCCI_BOOT_READY,
				     MT6797_CCCI_CONTROL_RX,
				     MT6797_CCCI_CONTROL_TX, 0, 0, true, true);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_md1_protocol_test_cases[] = {
	KUNIT_CASE(mt6797_ccci_round_trip_test),
	KUNIT_CASE(mt6797_ccci_rejects_invalid_sequence_test),
	KUNIT_CASE(mt6797_cldma_address_test),
	KUNIT_CASE(mt6797_cldma_rejects_invalid_address_test),
	KUNIT_CASE(mt6797_cldma_rejects_invalid_pointer_test),
	KUNIT_CASE(mt6797_ccci_boot_happy_path_test),
	KUNIT_CASE(mt6797_ccci_boot_rejects_out_of_order_test),
	KUNIT_CASE(mt6797_ccci_boot_ignores_other_channels_test),
	KUNIT_CASE(mt6797_ccci_boot_fences_exception_test),
	KUNIT_CASE(mt6797_ccci_boot_control_sequence_test),
	KUNIT_CASE(mt6797_ccci_boot_control_rejects_channel_test),
	KUNIT_CASE(mt6797_ccci_uart2_port_gate_test),
	{}
};

static struct kunit_suite mt6797_md1_protocol_test_suite = {
	.name = "mt6797-md1-protocol",
	.test_cases = mt6797_md1_protocol_test_cases,
};

kunit_test_suite(mt6797_md1_protocol_test_suite);
