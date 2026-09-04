// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "cldma.h"

static int mt6797_cldma_test_ring(struct mt6797_cldma_rgpd *ring)
{
	return mt6797_cldma_rx_ring_build(ring, 2, 0x1000, 0x2000, 3584);
}

static int
mt6797_cldma_test_read(struct mt6797_cldma_rgpd *gpd, const void *data,
		       size_t *packet_len,
		       struct mt6797_ccci_header_fields *fields)
{
	return mt6797_cldma_rx_read(gpd, data, 3584, 0x1010, 0x2000,
				      packet_len, fields);
}

static void mt6797_cldma_rx_ring_build_test(struct kunit *test)
{
	struct mt6797_cldma_rgpd ring[3];
	u64 address;
	int ret;

	ret = mt6797_cldma_rx_ring_build(ring, ARRAY_SIZE(ring), 0x1000,
					 0xa01200000, 3584);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ring[0].flags,
			(u8)(MT6797_CLDMA_GPD_IOC | MT6797_CLDMA_GPD_HWO));
	KUNIT_EXPECT_EQ(test, ring[0].data_allow_len, cpu_to_le16(3584));
	KUNIT_EXPECT_EQ(test, ring[0].data_len, cpu_to_le16(0));

	ret = mt6797_cldma_address_decode(ring[0].next, ring[0].debug_id,
					  MT6797_CLDMA_POINTER_NEXT, &address);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, address, (u64)0x1010);
	ret = mt6797_cldma_address_decode(ring[2].next, ring[2].debug_id,
					  MT6797_CLDMA_POINTER_NEXT, &address);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, address, (u64)0x1000);
	ret = mt6797_cldma_address_decode(ring[1].data, ring[1].debug_id,
					  MT6797_CLDMA_POINTER_DATA, &address);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, address, (u64)0xa01200e00);
}

static void mt6797_cldma_rx_ring_rejects_unaddressable_test(struct kunit *test)
{
	struct mt6797_cldma_rgpd ring[2];
	int ret;

	ret = mt6797_cldma_rx_ring_build(ring, ARRAY_SIZE(ring),
					 (u64)U32_MAX - 15, 0x2000, 3584);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	ret = mt6797_cldma_rx_ring_build(ring, ARRAY_SIZE(ring),
					 0x1001, 0x2000, 3584);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void mt6797_cldma_rx_descriptor_read_test(struct kunit *test)
{
	struct mt6797_ccci_header header = {};
	struct mt6797_ccci_header_fields input = {
		.data = { 0, MT6797_CCCI_MD_INIT_START_BOOT },
		.channel = MT6797_CCCI_CONTROL_RX,
		.reserved = MT6797_CCCI_MD_INIT_CHECK_ID,
	};
	struct mt6797_ccci_header_fields output = {};
	struct mt6797_cldma_rgpd ring[2];
	size_t packet_len = 0;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_ccci_header_encode(&header, &input), 0);
	KUNIT_ASSERT_EQ(test, mt6797_cldma_test_ring(ring), 0);
	ring[0].flags = MT6797_CLDMA_GPD_IOC;
	ring[0].data_len = cpu_to_le16(sizeof(header));
	ret = mt6797_cldma_test_read(&ring[0], &header, &packet_len, &output);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, packet_len, sizeof(header));
	KUNIT_EXPECT_EQ(test, output.channel, (u16)MT6797_CCCI_CONTROL_RX);
	KUNIT_EXPECT_EQ(test, output.data[1], (u32)MT6797_CCCI_MD_INIT_START_BOOT);
	KUNIT_EXPECT_EQ(test, output.reserved,
			(u32)MT6797_CCCI_MD_INIT_CHECK_ID);
}

static void mt6797_cldma_rx_descriptor_fences_bad_owner_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields fields;
	struct mt6797_cldma_rgpd ring[2];
	u8 data[sizeof(struct mt6797_ccci_header)] = {};
	size_t packet_len;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_cldma_test_ring(ring), 0);
	ring[0].data_len = cpu_to_le16(sizeof(struct mt6797_ccci_header));
	ret = mt6797_cldma_test_read(&ring[0], data, &packet_len, &fields);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	ring[0].flags = MT6797_CLDMA_GPD_IOC | BIT(1);
	ret = mt6797_cldma_test_read(&ring[0], data, &packet_len, &fields);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	ring[0].flags = MT6797_CLDMA_GPD_IOC;
	ring[0].data_len = cpu_to_le16(8);
	ret = mt6797_cldma_test_read(&ring[0], data, &packet_len, &fields);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mt6797_cldma_rx_descriptor_fences_corruption_test(struct kunit *test)
{
	struct mt6797_ccci_header_fields fields;
	struct mt6797_cldma_rgpd ring[2];
	u8 data[sizeof(struct mt6797_ccci_header)] = {};
	size_t packet_len;
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_cldma_test_ring(ring), 0);
	ring[0].flags = MT6797_CLDMA_GPD_IOC;
	ring[0].data_len = cpu_to_le16(sizeof(struct mt6797_ccci_header));
	ring[0].data_allow_len = cpu_to_le16(128);
	ret = mt6797_cldma_test_read(&ring[0], data, &packet_len, &fields);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);

	ring[0].data_allow_len = cpu_to_le16(3584);
	ring[0].next = cpu_to_le32(0x1020);
	ret = mt6797_cldma_test_read(&ring[0], data, &packet_len, &fields);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
}

static void mt6797_cldma_rx_descriptor_rearm_test(struct kunit *test)
{
	struct mt6797_cldma_rgpd ring[2];
	int ret;

	KUNIT_ASSERT_EQ(test, mt6797_cldma_test_ring(ring), 0);
	ring[0].flags = MT6797_CLDMA_GPD_IOC;
	ring[0].data_len = cpu_to_le16(128);
	ret = mt6797_cldma_rx_rearm(&ring[0], 3584);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ring[0].data_len, cpu_to_le16(0));
	KUNIT_EXPECT_EQ(test, ring[0].flags,
			(u8)(MT6797_CLDMA_GPD_IOC | MT6797_CLDMA_GPD_HWO));

	ring[0].flags = MT6797_CLDMA_GPD_IOC | BIT(1);
	KUNIT_EXPECT_EQ(test,
			mt6797_cldma_rx_rearm(&ring[0], 3584),
			-EPROTO);
}

static struct kunit_case mt6797_md1_cldma_test_cases[] = {
	KUNIT_CASE(mt6797_cldma_rx_ring_build_test),
	KUNIT_CASE(mt6797_cldma_rx_ring_rejects_unaddressable_test),
	KUNIT_CASE(mt6797_cldma_rx_descriptor_read_test),
	KUNIT_CASE(mt6797_cldma_rx_descriptor_fences_bad_owner_test),
	KUNIT_CASE(mt6797_cldma_rx_descriptor_fences_corruption_test),
	KUNIT_CASE(mt6797_cldma_rx_descriptor_rearm_test),
	{}
};

static struct kunit_suite mt6797_md1_cldma_test_suite = {
	.name = "mt6797-md1-cldma",
	.test_cases = mt6797_md1_cldma_test_cases,
};

kunit_test_suite(mt6797_md1_cldma_test_suite);
