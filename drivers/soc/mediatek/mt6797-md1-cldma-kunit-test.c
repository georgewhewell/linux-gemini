// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free MT6797 MD1 CLDMA resource and GPD contract tests. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-md1-cldma.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>

static struct mt6797_md1_cldma_resources mt6797_md1_cldma_resources = {
	.capability = 6,
	.md_smem_size = 0x00100000,
	.region = {
		{ 0x10014000, 0x1e00 }, { 0x10015000, 0x1e00 },
		{ 0x10219000, 0x1e00 }, { 0x1021a000, 0x1e00 },
		{ 0x10209000, 0x1000 }, { 0x1020a000, 0x1000 },
	},
	.irq = {
		{ 265, MT6797_MD1_CLDMA_IRQ_LEVEL_HIGH },
		{ 147, MT6797_MD1_CLDMA_IRQ_LEVEL_LOW },
		{ 266, MT6797_MD1_CLDMA_IRQ_EDGE_FALLING },
	},
};

static struct mt6797_md1_mpu_remap_layout mt6797_md1_cldma_layout = {
	.md_base = 0xb4000000ULL,
	.md_size = 0x08000000,
	.ap_md_smem_base = 0x88000000ULL,
	.ap_md_smem_size = 0x00200000,
	.lk_reported_load_complete = true,
};

static void mt6797_md1_cldma_accepts_vendor_topology(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_cldma_resources_validate(
				&mt6797_md1_cldma_resources),
			0);
}

static void mt6797_md1_cldma_refuses_non_vendor_topology(struct kunit *test)
{
	struct mt6797_md1_cldma_resources resources =
		mt6797_md1_cldma_resources;

	resources.region[MT6797_MD1_CLDMA_AP_PDN].size--;
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_resources_validate(&resources),
			-ERANGE);
	resources = mt6797_md1_cldma_resources;
	resources.irq[MT6797_MD1_AP_CCIF_IRQ].type =
		MT6797_MD1_CLDMA_IRQ_LEVEL_HIGH;
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_resources_validate(&resources),
			-ERANGE);
}

static void
mt6797_md1_cldma_accepts_only_attested_lk_carveout(struct kunit *test)
{
	struct mt6797_md1_mpu_remap_layout layout = mt6797_md1_cldma_layout;

	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_lk_carveout_validate(&layout),
			0);
	layout.ap_md_smem_size = 0x00600000; /* Whole LK mblock, not MD1. */
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_lk_carveout_validate(&layout),
			-ERANGE);
	layout = mt6797_md1_cldma_layout;
	layout.ap_md_smem_size--;
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_lk_carveout_validate(&layout),
			-ERANGE);
	layout = mt6797_md1_cldma_layout;
	layout.lk_reported_load_complete = false;
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_lk_carveout_validate(&layout),
			-EPERM);
}

static void mt6797_md1_cldma_builds_cyclic_tx_gpd_ring(struct kunit *test)
{
	struct mt6797_md1_cldma_tgpd gpd[2];
	struct mt6797_md1_cldma_tgpd *gpd_ptrs[] = { &gpd[0], &gpd[1] };
	const dma_addr_t dma[2] = { 0x812340000ULL, 0x9abcdf000ULL };

	KUNIT_ASSERT_EQ(test,
			mt6797_md1_cldma_tx_ring_init(gpd_ptrs, dma,
						      ARRAY_SIZE(gpd)),
			0);
	KUNIT_EXPECT_EQ(test, gpd[0].gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].next_gpd_ptr), 0xabcdf000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].debug_id), (u16)0x0090);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[1].next_gpd_ptr), 0x12340000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[1].debug_id), (u16)0x0080);
}

static void mt6797_md1_cldma_prepares_tx_payload_without_hwo(struct kunit *test)
{
	struct mt6797_md1_cldma_tgpd gpd;
	struct mt6797_md1_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t ring_dma = 0x812340000ULL;

	KUNIT_ASSERT_EQ(
		test, mt6797_md1_cldma_tx_ring_init(gpd_ptrs, &ring_dma, 1), 0);
	KUNIT_ASSERT_EQ(
		test,
		mt6797_md1_cldma_tgpd_prepare_tx(&gpd, 0xa76543210ULL, 16), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd.data_buff_bd_ptr), 0x76543210U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.debug_id), (u16)0x008a);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.data_buff_len), (u16)16);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x80);
}

static void mt6797_md1_cldma_builds_cyclic_rx_gpd_ring(struct kunit *test)
{
	struct mt6797_md1_cldma_rgpd gpd[2];
	struct mt6797_md1_cldma_rgpd *gpd_ptrs[] = { &gpd[0], &gpd[1] };
	const dma_addr_t gpd_dma[2] = { 0x812340000ULL, 0x9abcdf000ULL };
	const dma_addr_t data_dma[2] = { 0xa11112222ULL, 0xb33334444ULL };
	const u16 data_len[2] = { 4096, 16384 };

	KUNIT_ASSERT_EQ(test,
			mt6797_md1_cldma_rx_ring_init(gpd_ptrs, gpd_dma,
						      data_dma, data_len,
						      ARRAY_SIZE(gpd)),
			0);
	KUNIT_EXPECT_EQ(test, gpd[0].gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].data_buff_bd_ptr),
			0x11112222U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd[0].next_gpd_ptr), 0xabcdf000U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].debug_id), (u16)0x009a);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd[0].data_allow_len), (u16)4096);
}

static void mt6797_md1_cldma_refuses_unusable_dma_addresses(struct kunit *test)
{
	struct mt6797_md1_cldma_tgpd gpd;
	struct mt6797_md1_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t unaligned = 0x812340001ULL;
	const dma_addr_t too_wide = 0x1000000000ULL;

	KUNIT_EXPECT_EQ(test,
			mt6797_md1_cldma_tx_ring_init(gpd_ptrs, &unaligned, 1),
			-ERANGE);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_cldma_tx_ring_init(gpd_ptrs, &too_wide, 1),
			-ERANGE);
	KUNIT_EXPECT_EQ(test,
			mt6797_md1_cldma_tgpd_prepare_tx(&gpd, too_wide, 1),
			-ERANGE);
}

static void mt6797_md1_cldma_transfers_tx_ownership(struct kunit *test)
{
	struct mt6797_md1_cldma_tgpd gpd;
	struct mt6797_md1_cldma_tgpd *gpd_ptrs[] = { &gpd };
	const dma_addr_t ring_dma = 0x812340000ULL;
	u16 len;

	KUNIT_ASSERT_EQ(test,
			mt6797_md1_cldma_tx_ring_init(gpd_ptrs, &ring_dma, 1), 0);
	KUNIT_ASSERT_EQ(test,
			mt6797_md1_cldma_tgpd_prepare_tx(&gpd, 0xa76543210ULL, 64), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_cldma_tgpd_publish(&gpd), 0);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x81);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_tgpd_reclaim(&gpd, &len), -EAGAIN);

	/* Emulate CLDMA returning ownership after completing the transfer. */
	gpd.gpd_flags &= ~MT6797_MD1_CLDMA_GPD_HWO;
	KUNIT_ASSERT_EQ(test, mt6797_md1_cldma_tgpd_reclaim(&gpd, &len), 0);
	KUNIT_EXPECT_EQ(test, len, (u16)64);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x80);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(gpd.data_buff_bd_ptr), 0U);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(gpd.debug_id), (u16)0x0080);
}

static void mt6797_md1_cldma_validates_rx_completion(struct kunit *test)
{
	struct mt6797_md1_cldma_rgpd gpd = { .gpd_flags = 0x80 };
	u16 len;

	KUNIT_ASSERT_EQ(test,
			mt6797_md1_cldma_rgpd_publish(&gpd, 0xa76543210ULL, 3584), 0);
	KUNIT_EXPECT_EQ(test, gpd.gpd_flags, (u8)0x81);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_rgpd_consume(&gpd, &len), -EAGAIN);
	gpd.gpd_flags &= ~MT6797_MD1_CLDMA_GPD_HWO;
	gpd.data_buff_len = cpu_to_le16(3585);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_rgpd_consume(&gpd, &len), -EBADMSG);
	gpd.data_buff_len = cpu_to_le16(128);
	KUNIT_EXPECT_EQ(test, mt6797_md1_cldma_rgpd_consume(&gpd, &len), 0);
	KUNIT_EXPECT_EQ(test, len, (u16)128);
}

static void mt6797_md1_cldma_decodes_unmasked_irqs(struct kunit *test)
{
	struct mt6797_md1_cldma_irq_pending pending;
	u32 tx_status = BIT(0) | BIT(2) | BIT(16);
	u32 rx_status = BIT(3) | BIT(4) | BIT(20);

	mt6797_md1_cldma_irq_decode(tx_status, BIT(2), rx_status, BIT(3), &pending);
	KUNIT_EXPECT_EQ(test, pending.tx_done, (u8)BIT(0));
	KUNIT_EXPECT_EQ(test, pending.rx_done, (u8)BIT(4));
	KUNIT_EXPECT_EQ(test, pending.tx_error, (u8)BIT(0));
	KUNIT_EXPECT_EQ(test, pending.rx_error, (u8)BIT(4));
}

static struct kunit_case mt6797_md1_cldma_test_cases[] = {
	KUNIT_CASE(mt6797_md1_cldma_accepts_vendor_topology),
	KUNIT_CASE(mt6797_md1_cldma_refuses_non_vendor_topology),
	KUNIT_CASE(mt6797_md1_cldma_accepts_only_attested_lk_carveout),
	KUNIT_CASE(mt6797_md1_cldma_builds_cyclic_tx_gpd_ring),
	KUNIT_CASE(mt6797_md1_cldma_prepares_tx_payload_without_hwo),
	KUNIT_CASE(mt6797_md1_cldma_builds_cyclic_rx_gpd_ring),
	KUNIT_CASE(mt6797_md1_cldma_refuses_unusable_dma_addresses),
	KUNIT_CASE(mt6797_md1_cldma_transfers_tx_ownership),
	KUNIT_CASE(mt6797_md1_cldma_validates_rx_completion),
	KUNIT_CASE(mt6797_md1_cldma_decodes_unmasked_irqs),
	{}
};

static struct kunit_suite mt6797_md1_cldma_test_suite = {
	.name = "mt6797-md1-cldma",
	.test_cases = mt6797_md1_cldma_test_cases,
};
kunit_test_suite(mt6797_md1_cldma_test_suite);

MODULE_LICENSE("GPL");
