// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free MT6797 CLDMA descriptor tests. */

#include <kunit/test.h>
#include <linux/errno.h>

#include "mtk_mt6797_cldma.h"

static void mtk_mt6797_cldma_builds_cyclic_tx_ring(struct kunit *test)
{
	struct mtk_mt6797_cldma_tgpd gpd[2];
	struct mtk_mt6797_cldma_tgpd *gpd_ptrs[] = { &gpd[0], &gpd[1] };
	const dma_addr_t dma[] = { 0x12340000, 0xabcdf000 };

	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_tx_ring_init(gpd_ptrs, dma,
						      ARRAY_SIZE(gpd)),
			0);
	KUNIT_EXPECT_EQ(test, gpd[0].gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].next_gpd_ptr), 0xabcdf000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].debug_id), (u16)0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[1].next_gpd_ptr), 0x12340000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[1].debug_id), (u16)0);
}

static void mtk_mt6797_cldma_prepares_36bit_tx_payload(struct kunit *test)
{
	struct mtk_mt6797_cldma_tgpd gpd;
	struct mtk_mt6797_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t ring_dma = 0x12340000;

	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_tx_ring_init(gpd_ptrs, &ring_dma, 1), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_tgpd_prepare_tx(&gpd,
							 0xa76543210ULL, 16),
			0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd.data_buff_bd_ptr), 0x76543210U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.debug_id), (u16)0x000a);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.data_buff_len), (u16)16);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x80);
}

static void mtk_mt6797_cldma_builds_cyclic_rx_ring(struct kunit *test)
{
	struct mtk_mt6797_cldma_rgpd gpd[2];
	struct mtk_mt6797_cldma_rgpd *gpd_ptrs[] = { &gpd[0], &gpd[1] };
	const dma_addr_t gpd_dma[] = { 0x12340000, 0xabcdf000 };
	const dma_addr_t data_dma[] = { 0xa11112222ULL, 0xb33334444ULL };
	const u16 data_len[] = { 4096, 16384 };

	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_rx_ring_init(gpd_ptrs, gpd_dma,
						      data_dma, data_len,
						      ARRAY_SIZE(gpd)),
			0);
	KUNIT_EXPECT_EQ(test, gpd[0].gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].data_buff_bd_ptr), 0x11112222U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].next_gpd_ptr), 0xabcdf000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].debug_id), (u16)0x000a);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].data_allow_len), (u16)4096);
}

static void mtk_mt6797_cldma_refuses_bad_ring_roots(struct kunit *test)
{
	struct mtk_mt6797_cldma_tgpd gpd;
	struct mtk_mt6797_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t unaligned = 0x12340001;
	const dma_addr_t above_32bit = 0x100000000ULL;

	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_cldma_tx_ring_init(gpd_ptrs, &unaligned, 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_cldma_tx_ring_init(gpd_ptrs, &above_32bit, 1),
			-ERANGE);
}

static void mtk_mt6797_cldma_refuses_bad_payloads(struct kunit *test)
{
	struct mtk_mt6797_cldma_tgpd gpd = {};
	const dma_addr_t above_36bit = 0x1000000000ULL;

	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_cldma_tgpd_prepare_tx(&gpd, 0x1234, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			mtk_mt6797_cldma_tgpd_prepare_tx(&gpd, above_36bit, 1),
			-ERANGE);
}

static void mtk_mt6797_cldma_transfers_tx_ownership(struct kunit *test)
{
	struct mtk_mt6797_cldma_tgpd gpd;
	struct mtk_mt6797_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t ring_dma = 0x12340000;
	u16 len;

	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_tx_ring_init(gpd_ptrs, &ring_dma, 1), 0);
	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_tgpd_prepare_tx(&gpd,
							 0xa76543210ULL, 64),
			0);
	KUNIT_ASSERT_EQ(test, mtk_mt6797_cldma_tgpd_publish(&gpd), 0);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x81);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_cldma_tgpd_reclaim(&gpd, &len),
			-EAGAIN);

	gpd.gpd_flags &= ~MTK_MT6797_CLDMA_GPD_HWO;
	KUNIT_ASSERT_EQ(test, mtk_mt6797_cldma_tgpd_reclaim(&gpd, &len), 0);
	KUNIT_EXPECT_EQ(test, len, (u16)64);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd.data_buff_bd_ptr), 0U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.debug_id), (u16)0);
}

static void mtk_mt6797_cldma_validates_rx_completion(struct kunit *test)
{
	struct mtk_mt6797_cldma_rgpd gpd = { .gpd_flags = 0x80 };
	u16 len;

	KUNIT_ASSERT_EQ(test,
			mtk_mt6797_cldma_rgpd_publish(&gpd,
						      0xa76543210ULL, 3584),
			0);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x81);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_cldma_rgpd_consume(&gpd, &len),
			-EAGAIN);
	gpd.gpd_flags &= ~MTK_MT6797_CLDMA_GPD_HWO;
	gpd.data_buff_len = cpu_to_le16(3585);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_cldma_rgpd_consume(&gpd, &len),
			-EBADMSG);
	gpd.data_buff_len = cpu_to_le16(128);
	KUNIT_EXPECT_EQ(test, mtk_mt6797_cldma_rgpd_consume(&gpd, &len), 0);
	KUNIT_EXPECT_EQ(test, len, (u16)128);
}

static void mtk_mt6797_cldma_decodes_unmasked_irqs(struct kunit *test)
{
	struct mtk_mt6797_cldma_irq_pending pending;
	u32 tx_status = BIT(0) | BIT(2) | BIT(16);
	u32 rx_status = BIT(3) | BIT(4) | BIT(20);

	mtk_mt6797_cldma_irq_decode(tx_status, BIT(2), rx_status, BIT(3),
				    &pending);
	KUNIT_EXPECT_EQ(test, pending.tx_done, (u8)BIT(0));
	KUNIT_EXPECT_EQ(test, pending.rx_done, (u8)BIT(4));
	KUNIT_EXPECT_EQ(test, pending.tx_error, (u8)BIT(0));
	KUNIT_EXPECT_EQ(test, pending.rx_error, (u8)BIT(4));
}

static struct kunit_case mtk_mt6797_cldma_test_cases[] = {
	KUNIT_CASE(mtk_mt6797_cldma_builds_cyclic_tx_ring),
	KUNIT_CASE(mtk_mt6797_cldma_prepares_36bit_tx_payload),
	KUNIT_CASE(mtk_mt6797_cldma_builds_cyclic_rx_ring),
	KUNIT_CASE(mtk_mt6797_cldma_refuses_bad_ring_roots),
	KUNIT_CASE(mtk_mt6797_cldma_refuses_bad_payloads),
	KUNIT_CASE(mtk_mt6797_cldma_transfers_tx_ownership),
	KUNIT_CASE(mtk_mt6797_cldma_validates_rx_completion),
	KUNIT_CASE(mtk_mt6797_cldma_decodes_unmasked_irqs),
	{}
};

static struct kunit_suite mtk_mt6797_cldma_test_suite = {
	.name = "mtk-mt6797-cldma",
	.test_cases = mtk_mt6797_cldma_test_cases,
};

kunit_test_suite(mtk_mt6797_cldma_test_suite);

MODULE_LICENSE("GPL");
