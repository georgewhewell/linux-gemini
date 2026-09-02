// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mtk_ccci_common.h"

static void mtk_ccci_header_roundtrip(struct kunit *test)
{
	struct mtk_ccci_header header;
	struct mtk_ccci_frame frame;
	int ret;

	mtk_ccci_header_init(&header, MTK_CCCI_UART2_TX, 0x1234, 42);
	ret = mtk_ccci_header_decode(&header, sizeof(header), &frame);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, frame.data0, 0U);
	KUNIT_EXPECT_EQ(test, frame.data1, 42U);
	KUNIT_EXPECT_EQ(test, frame.channel, (u16)MTK_CCCI_UART2_TX);
	KUNIT_EXPECT_EQ(test, frame.sequence, (u16)0x1234);
	KUNIT_EXPECT_TRUE(test, frame.assert_bit);
	KUNIT_EXPECT_EQ(test, frame.reserved, 0U);
	ret = mtk_ccci_header_decode(&header, sizeof(header) - 1, &frame);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void mtk_ccci_net_map(struct kunit *test)
{
	u16 tx, rx;
	u8 queue;

	KUNIT_ASSERT_EQ(test, mtk_ccci_net_channels(0, &tx, &rx, &queue), 0);
	KUNIT_EXPECT_EQ(test, tx, (u16)22);
	KUNIT_EXPECT_EQ(test, rx, (u16)20);
	KUNIT_EXPECT_EQ(test, queue, (u8)3);
	KUNIT_ASSERT_EQ(test, mtk_ccci_net_channels(3, &tx, &rx, &queue), 0);
	KUNIT_EXPECT_EQ(test, tx, (u16)71);
	KUNIT_EXPECT_EQ(test, rx, (u16)69);
	KUNIT_EXPECT_EQ(test, queue, (u8)5);
	KUNIT_EXPECT_EQ(test, mtk_ccci_net_channels(8, NULL, NULL, NULL),
			-EINVAL);
	KUNIT_ASSERT_EQ(test, mtk_ccci_net_channels(17, &tx, &rx, &queue), 0);
	KUNIT_EXPECT_EQ(test, tx, (u16)145);
	KUNIT_EXPECT_EQ(test, rx, (u16)143);
	KUNIT_EXPECT_EQ(test, mtk_ccci_net_rx_link(143), 17);
	KUNIT_EXPECT_EQ(test, mtk_ccci_net_rx_link(144), -ENOENT);
}

static void mtk_ccci_make_query(u8 *query)
{
	unsigned int i;

	memset(query, 0, MTK_CCCI_FEATURE_QUERY_SIZE);
	put_unaligned_le32(MTK_CCCI_MD_FEATURE_PATTERN, query);
	for (i = 0; i < MTK_CCCI_RT_FEATURE_COUNT; i++)
		query[sizeof(u32) + i] = MTK_CCCI_FEATURE_NOT_EXIST;
	put_unaligned_le32(MTK_CCCI_MD_FEATURE_PATTERN,
			   query + MTK_CCCI_FEATURE_QUERY_SIZE - sizeof(u32));
}

static void mtk_ccci_runtime_v2_minimal(struct kunit *test)
{
	struct mtk_ccci_runtime_config config = {
		.smem_md_addr = 0x40000000,
		.smem_size = 0x100000,
		.random_seed = 0x12345678,
		.ccmni_mtu = MTK_CCCI_NET_RX_SIZE,
	};
	u8 query[MTK_CCCI_FEATURE_QUERY_SIZE];
	u8 *runtime;
	u8 response[100];
	size_t runtime_len, response_len;
	int ret;

	runtime = kunit_kzalloc(test, MTK_CCCI_RUNTIME_AP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	mtk_ccci_make_query(query);
	query[4 + MTK_CCCI_RT_BOOT_INFO] = MTK_CCCI_FEATURE_MUST_SUPPORT;
	query[4 + MTK_CCCI_RT_EXCEPTION_SHM] = MTK_CCCI_FEATURE_MUST_SUPPORT;
	query[4 + MTK_CCCI_RT_CCMNI_MTU] = MTK_CCCI_FEATURE_MUST_SUPPORT;
	ret = mtk_ccci_runtime_v2_build(query, sizeof(query), &config, runtime,
					MTK_CCCI_RUNTIME_AP_SIZE, &runtime_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, runtime_len, (size_t)0);
	KUNIT_EXPECT_LE(test, runtime_len, (size_t)MTK_CCCI_RUNTIME_AP_SIZE);
	KUNIT_EXPECT_EQ(test, runtime[0], (u8)MTK_CCCI_RT_BOOT_INFO);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(runtime + 4), 16U);
	ret = mtk_ccci_runtime_v2_response(&config, response, sizeof(response),
					   &response_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, response_len, sizeof(response));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(response),
			MTK_CCCI_AP_FEATURE_PATTERN);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(response + 68), 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(response + 72), 0x40010000U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(response + 96),
			MTK_CCCI_AP_FEATURE_PATTERN);
}

static void mtk_ccci_runtime_rejects_unsupported_must(struct kunit *test)
{
	struct mtk_ccci_runtime_config config = {
		.smem_md_addr = 0x40000000,
		.smem_size = 0x100000,
		.ccmni_mtu = MTK_CCCI_NET_RX_SIZE,
	};
	u8 query[MTK_CCCI_FEATURE_QUERY_SIZE];
	u8 *runtime;
	size_t runtime_len;
	int ret;

	runtime = kunit_kzalloc(test, MTK_CCCI_RUNTIME_AP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	mtk_ccci_make_query(query);
	query[4 + MTK_CCCI_RT_FAST_HEADER] = MTK_CCCI_FEATURE_MUST_SUPPORT;
	ret = mtk_ccci_runtime_v2_build(query, sizeof(query), &config, runtime,
					MTK_CCCI_RUNTIME_AP_SIZE, &runtime_len);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	query[0] ^= 1;
	ret = mtk_ccci_runtime_v2_build(query, sizeof(query), &config, runtime,
					MTK_CCCI_RUNTIME_AP_SIZE, &runtime_len);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static struct kunit_case mtk_ccci_common_cases[] = {
	KUNIT_CASE(mtk_ccci_header_roundtrip),
	KUNIT_CASE(mtk_ccci_net_map),
	KUNIT_CASE(mtk_ccci_runtime_v2_minimal),
	KUNIT_CASE(mtk_ccci_runtime_rejects_unsupported_must),
	{}
};

static struct kunit_suite mtk_ccci_common_suite = {
	.name = "mtk-ccci-common",
	.test_cases = mtk_ccci_common_cases,
};

kunit_test_suite(mtk_ccci_common_suite);

MODULE_LICENSE("GPL");
