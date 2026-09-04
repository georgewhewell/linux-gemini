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

static struct kunit_case mt6797_md1_protocol_test_cases[] = {
	KUNIT_CASE(mt6797_ccci_round_trip_test),
	KUNIT_CASE(mt6797_ccci_rejects_invalid_sequence_test),
	KUNIT_CASE(mt6797_cldma_address_test),
	KUNIT_CASE(mt6797_cldma_rejects_invalid_address_test),
	KUNIT_CASE(mt6797_cldma_rejects_invalid_pointer_test),
	{}
};

static struct kunit_suite mt6797_md1_protocol_test_suite = {
	.name = "mt6797-md1-protocol",
	.test_cases = mt6797_md1_protocol_test_cases,
};

kunit_test_suite(mt6797_md1_protocol_test_suite);
