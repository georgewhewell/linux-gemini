// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MT6797 MD1 LK hand-off and CLDMA queue-0 invariants.
 *
 * These helpers deliberately have no MMIO, SMC, IRQ, clock, regulator,
 * reset, firmware-load or DMA-allocation operation.  LK's FDT rewrite is a
 * memory-ownership hand-off, not permission to restart MD1 or alter its MPU.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "mt6797-md1-handoff.h"

#define MT6797_MD1_EMI_BASE		0x40000000ULL
#define MT6797_MD1_MPU_GRANULE		SZ_64K
#define MT6797_MD1_FIRMWARE_MIN_SIZE	0x0a000000ULL
#define MT6797_MD1_SHARED_MIN_SIZE	SZ_1M

static const struct mt6797_md1_resource mt6797_md1_resources[] = {
	{ 0x10014000, 0x1e00 }, /* AP CLDMA AO */
	{ 0x10001000, 0x1000 }, /* infracfg */
	{ 0x10015000, 0x1e00 }, /* MD CLDMA AO */
	{ 0x10219000, 0x1e00 }, /* AP CLDMA PDN */
	{ 0x1021a000, 0x1e00 }, /* MD CLDMA PDN */
	{ 0x10209000, 0x1000 }, /* AP CCIF */
	{ 0x1020a000, 0x1000 }, /* MD CCIF */
	{ 0x1000c000, 0x1000 }, /* apmixedsys */
	{ 0x20150000, 0x0118 }, /* MD clock switch */
	{ 0x20130000, 0x1004 }, /* MD DCM */
	{ 0x20060000, 0x00d0 }, /* MD peripheral misc */
	{ 0x260f0000, 0x0200 }, /* MD L1 A0 */
	{ 0x20140000, 0x00a0 }, /* MD top PLL */
	{ 0x20120000, 0x00d0 }, /* MD system clock */
	{ 0x200f0100, 0x0300 }, /* MD RGU */
	{ 0x26010000, 0x0040 }, /* L1 RGU */
	{ 0x20000024, 0x0004 }, /* boot-vector enable */
	{ 0x10201300, 0x0008 }, /* boot status */
};

static_assert(ARRAY_SIZE(mt6797_md1_resources) ==
	      MT6797_MD1_RESOURCE_COUNT);
static_assert(sizeof(struct mt6797_md1_tgpd) == MT6797_MD1_GPD_SIZE);
static_assert(sizeof(struct mt6797_md1_rgpd) == MT6797_MD1_GPD_SIZE);

int mt6797_md1_resources_validate(const struct mt6797_md1_resource *resources,
				  size_t count)
{
	unsigned int i;

	if (!resources || count != ARRAY_SIZE(mt6797_md1_resources))
		return -EINVAL;

	for (i = 0; i < count; i++)
		if (resources[i].start != mt6797_md1_resources[i].start ||
		    resources[i].size != mt6797_md1_resources[i].size)
			return -ERANGE;

	return 0;
}

static int mt6797_md1_range_end(u64 base, u64 size, u64 *end)
{
	if (!size || check_add_overflow(base, size - 1, end))
		return -EOVERFLOW;
	if (base < MT6797_MD1_EMI_BASE || *end > U32_MAX)
		return -ERANGE;
	if (!IS_ALIGNED(base, MT6797_MD1_MPU_GRANULE) ||
	    !IS_ALIGNED(size, MT6797_MD1_MPU_GRANULE))
		return -EINVAL;

	return 0;
}

int mt6797_md1_layout_validate(const struct mt6797_md1_layout *layout)
{
	u64 bank0_window_end;
	u64 bank4_base;
	u64 bank4_window_end;
	u64 firmware_end;
	u64 shared_end;
	int ret;

	if (!layout)
		return -EINVAL;

	ret = mt6797_md1_range_end(layout->firmware_base,
				   layout->firmware_size, &firmware_end);
	if (ret)
		return ret;
	ret = mt6797_md1_range_end(layout->shared_base, layout->shared_size,
				   &shared_end);
	if (ret)
		return ret;
	if (layout->shared_size < MT6797_MD1_SHARED_MIN_SIZE)
		return -ENOSPC;
	if (layout->firmware_size < MT6797_MD1_FIRMWARE_MIN_SIZE)
		return -ENOSPC;
	if (layout->firmware_base <= shared_end &&
	    layout->shared_base <= firmware_end)
		return -EINVAL;
	if (!IS_ALIGNED(layout->firmware_base, MT6797_MD1_REMAP_SLOT_SIZE))
		return -EINVAL;
	if (check_add_overflow(layout->firmware_base,
			       MT6797_MD1_REMAP_WINDOW_SIZE - 1,
			       &bank0_window_end) || bank0_window_end > U32_MAX)
		return -ERANGE;

	bank4_base = round_down(layout->shared_base,
				MT6797_MD1_REMAP_SLOT_SIZE);
	if (check_add_overflow(bank4_base, MT6797_MD1_REMAP_WINDOW_SIZE - 1,
			       &bank4_window_end) || bank4_window_end > U32_MAX ||
	    shared_end > bank4_window_end)
		return -ERANGE;

	return 0;
}

static void mt6797_md1_remap_pair(u64 base, u32 *map0, u32 *map1)
{
	*map0 = (((base >> 24) | 0x1) & 0xff) |
		 ((((base + MT6797_MD1_REMAP_SLOT_SIZE) >> 16) | BIT(8)) &
		  0xff00) |
		 ((((base + 2 * MT6797_MD1_REMAP_SLOT_SIZE) >> 8) | BIT(16)) &
		  0xff0000) |
		 ((((base + 3 * MT6797_MD1_REMAP_SLOT_SIZE) >> 0) | BIT(24)) &
		  0xff000000);
	*map1 = ((((base + 4 * MT6797_MD1_REMAP_SLOT_SIZE) >> 24) | 0x1) &
		  0xff) |
		 ((((base + 5 * MT6797_MD1_REMAP_SLOT_SIZE) >> 16) | BIT(8)) &
		  0xff00) |
		 ((((base + 6 * MT6797_MD1_REMAP_SLOT_SIZE) >> 8) | BIT(16)) &
		  0xff0000) |
		 ((((base + 7 * MT6797_MD1_REMAP_SLOT_SIZE) >> 0) | BIT(24)) &
		  0xff000000);
}

int mt6797_md1_remap_plan_build(const struct mt6797_md1_layout *layout,
				struct mt6797_md1_remap_plan *plan)
{
	u64 bank4_base;
	int ret;

	if (!plan)
		return -EINVAL;
	ret = mt6797_md1_layout_validate(layout);
	if (ret)
		return ret;

	bank4_base = round_down(layout->shared_base,
				MT6797_MD1_REMAP_SLOT_SIZE);
	mt6797_md1_remap_pair(layout->firmware_base,
			      &plan->physical.bank0_map0,
			      &plan->physical.bank0_map1);
	mt6797_md1_remap_pair(bank4_base, &plan->physical.bank4_map0,
			      &plan->physical.bank4_map1);
	mt6797_md1_remap_pair(layout->firmware_base - MT6797_MD1_EMI_BASE,
			      &plan->emi_relative.bank0_map0,
			      &plan->emi_relative.bank0_map1);
	mt6797_md1_remap_pair(bank4_base - MT6797_MD1_EMI_BASE,
			      &plan->emi_relative.bank4_map0,
			      &plan->emi_relative.bank4_map1);
	plan->bank4_base = bank4_base;

	return 0;
}

static int mt6797_md1_set_pointer(__le32 *low, __le16 *debug,
				  dma_addr_t address, bool next)
{
	u64 value = address;
	u16 high;

	if (value >= BIT_ULL(MT6797_MD1_DMA_ADDRESS_BITS))
		return -ERANGE;

	high = le16_to_cpu(*debug);
	if (next)
		high = (high & 0xff0f) | ((value >> 28) & 0x00f0);
	else
		high = (high & 0xfff0) | ((value >> 32) & 0x000f);
	*low = cpu_to_le32(value);
	*debug = cpu_to_le16(high);

	return 0;
}

int mt6797_md1_q0_init(struct mt6797_md1_q0 *q0, dma_addr_t tx_dma,
		       dma_addr_t rx_dma)
{
	int ret;

	/* Vendor queue-root registers contain only the low 32 address bits. */
	if (!q0)
		return -EINVAL;
	if (tx_dma > U32_MAX || rx_dma > U32_MAX ||
	    !IS_ALIGNED(tx_dma, MT6797_MD1_GPD_SIZE) ||
	    !IS_ALIGNED(rx_dma, MT6797_MD1_GPD_SIZE))
		return -ERANGE;

	memset(q0, 0, sizeof(*q0));
	q0->tx.flags = MT6797_MD1_GPD_IOC;
	q0->rx.flags = MT6797_MD1_GPD_IOC;
	ret = mt6797_md1_set_pointer(&q0->tx.next, &q0->tx.debug_id,
				     tx_dma, true);
	if (ret)
		return ret;
	ret = mt6797_md1_set_pointer(&q0->rx.next, &q0->rx.debug_id,
				     rx_dma, true);
	if (ret)
		return ret;

	/* HWO remains clear: this is geometry, not a live DMA queue. */
	return 0;
}
