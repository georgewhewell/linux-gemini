// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 MD1 CLDMA resource and GPD construction.
 *
 * Vendor evidence: arch/arm64/boot/dts/mt6797.dtsi:1782-1815 supplies the
 * ordered regions, GIC sources and capability; eccci/mt6797/cldma_platform.c
 * :101-145 consumes that order; modem_cldma.h:258-285 and modem_cldma.c
 * :124-176, :912-968 supply the 16-byte GPD and cyclic pointer format.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-md1-cldma.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/string.h>

#define MT6797_MD1_CLDMA_CAPABILITY 6U
#define MT6797_MD1_CLDMA_MD_SMEM_SIZE 0x00100000U

/* LK live FDT ranges already independently checked by the hand-off probe. */
#define MT6797_MD1_CLDMA_LK_MD_BASE 0xb4000000ULL
#define MT6797_MD1_CLDMA_LK_MD_SIZE 0x08000000U
#define MT6797_MD1_CLDMA_LK_SMEM_BASE 0x88000000ULL
#define MT6797_MD1_CLDMA_LK_SMEM_SIZE 0x00600000U

static const struct mt6797_md1_cldma_region mt6797_md1_cldma_regions[] = {
	[MT6797_MD1_CLDMA_AP_AO] = { 0x10014000, 0x1e00 },
	[MT6797_MD1_CLDMA_MD_AO] = { 0x10015000, 0x1e00 },
	[MT6797_MD1_CLDMA_AP_PDN] = { 0x10219000, 0x1e00 },
	[MT6797_MD1_CLDMA_MD_PDN] = { 0x1021a000, 0x1e00 },
	[MT6797_MD1_CLDMA_AP_CCIF] = { 0x10209000, 0x1000 },
	[MT6797_MD1_CLDMA_MD_CCIF] = { 0x1020a000, 0x1000 },
};

static const struct mt6797_md1_cldma_irq mt6797_md1_cldma_irqs[] = {
	[MT6797_MD1_CLDMA_IRQ] = { 265, MT6797_MD1_CLDMA_IRQ_LEVEL_HIGH },
	[MT6797_MD1_AP_CCIF_IRQ] = { 147, MT6797_MD1_CLDMA_IRQ_LEVEL_LOW },
	[MT6797_MD1_MD_WDT_IRQ] = { 266, MT6797_MD1_CLDMA_IRQ_EDGE_FALLING },
};

static_assert(sizeof(struct mt6797_md1_cldma_tgpd) ==
	      MT6797_MD1_CLDMA_GPD_BYTES);
static_assert(sizeof(struct mt6797_md1_cldma_rgpd) ==
	      MT6797_MD1_CLDMA_GPD_BYTES);

static bool mt6797_md1_cldma_addr_valid(dma_addr_t addr)
{
	return (u64)addr <= MT6797_MD1_CLDMA_ADDR_MAX;
}

static int mt6797_md1_cldma_set_pointer(__le32 *low, __le16 *debug,
					dma_addr_t addr, bool next)
{
	u16 value;

	if (!mt6797_md1_cldma_addr_valid(addr))
		return -ERANGE;
	value = le16_to_cpu(*debug);
	if (next)
		value = (value & 0xff0f) | (((u64)addr >> 28) & 0x00f0);
	else
		value = (value & 0xfff0) | (((u64)addr >> 32) & 0x000f);
	*low = cpu_to_le32(addr);
	*debug = cpu_to_le16(value);
	return 0;
}

static int mt6797_md1_cldma_gpd_dma_valid(dma_addr_t addr)
{
	if (!IS_ALIGNED(addr, MT6797_MD1_CLDMA_GPD_ALIGN))
		return -EINVAL;
	return mt6797_md1_cldma_addr_valid(addr) ? 0 : -ERANGE;
}

int mt6797_md1_cldma_resources_validate(
	const struct mt6797_md1_cldma_resources *resources)
{
	unsigned int i;

	if (!resources)
		return -EINVAL;
	if (resources->md_id ||
	    resources->capability != MT6797_MD1_CLDMA_CAPABILITY ||
	    resources->md_smem_size != MT6797_MD1_CLDMA_MD_SMEM_SIZE)
		return -EINVAL;
	for (i = 0; i < MT6797_MD1_CLDMA_REGION_COUNT; i++)
		if (resources->region[i].base !=
			    mt6797_md1_cldma_regions[i].base ||
		    resources->region[i].size !=
			    mt6797_md1_cldma_regions[i].size)
			return -ERANGE;
	for (i = 0; i < MT6797_MD1_CLDMA_IRQ_COUNT; i++)
		if (resources->irq[i].hwirq != mt6797_md1_cldma_irqs[i].hwirq ||
		    resources->irq[i].type != mt6797_md1_cldma_irqs[i].type)
			return -ERANGE;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_resources_validate);

int mt6797_md1_cldma_lk_carveout_validate(
	const struct mt6797_md1_mpu_remap_layout *layout)
{
	if (!layout)
		return -EINVAL;
	if (!layout->lk_reported_load_complete)
		return -EPERM;
	if (layout->md_base != MT6797_MD1_CLDMA_LK_MD_BASE ||
	    layout->md_size != MT6797_MD1_CLDMA_LK_MD_SIZE ||
	    layout->ap_md_smem_base != MT6797_MD1_CLDMA_LK_SMEM_BASE ||
	    layout->ap_md_smem_size != MT6797_MD1_CLDMA_LK_SMEM_SIZE)
		return -ERANGE;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_lk_carveout_validate);

int mt6797_md1_cldma_tgpd_prepare_tx(struct mt6797_md1_cldma_tgpd *gpd,
				     dma_addr_t data_dma, u16 data_len)
{
	int ret;

	if (!gpd)
		return -EINVAL;
	ret = mt6797_md1_cldma_set_pointer(&gpd->data_buff_bd_ptr,
					   &gpd->debug_id, data_dma, false);
	if (ret)
		return ret;
	gpd->data_buff_len = cpu_to_le16(data_len);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_tgpd_prepare_tx);

int mt6797_md1_cldma_tx_ring_init(struct mt6797_md1_cldma_tgpd *const *gpd,
				  const dma_addr_t *gpd_dma, size_t count)
{
	size_t i;

	if (!gpd || !gpd_dma || !count)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (!gpd[i] || mt6797_md1_cldma_gpd_dma_valid(gpd_dma[i]))
			return -ERANGE;
	for (i = 0; i < count; i++) {
		memset(gpd[i], 0, sizeof(*gpd[i]));
		gpd[i]->gpd_flags = MT6797_MD1_CLDMA_TX_GPD_IOC;
		mt6797_md1_cldma_set_pointer(&gpd[i]->next_gpd_ptr,
					     &gpd[i]->debug_id,
					     gpd_dma[(i + 1) % count], true);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_tx_ring_init);

int mt6797_md1_cldma_rx_ring_init(struct mt6797_md1_cldma_rgpd *const *gpd,
				  const dma_addr_t *gpd_dma,
				  const dma_addr_t *data_dma,
				  const u16 *data_allow_len, size_t count)
{
	size_t i;
	int ret;

	if (!gpd || !gpd_dma || !data_dma || !data_allow_len || !count)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (!gpd[i] || mt6797_md1_cldma_gpd_dma_valid(gpd_dma[i]) ||
		    !mt6797_md1_cldma_addr_valid(data_dma[i]) ||
		    !data_allow_len[i])
			return -ERANGE;
	for (i = 0; i < count; i++) {
		memset(gpd[i], 0, sizeof(*gpd[i]));
		/* HWO stays clear until a later post-vector transport owner exists. */
		gpd[i]->gpd_flags = MT6797_MD1_CLDMA_RX_GPD_IOC;
		gpd[i]->data_allow_len = cpu_to_le16(data_allow_len[i]);
		ret = mt6797_md1_cldma_set_pointer(&gpd[i]->data_buff_bd_ptr,
						   &gpd[i]->debug_id,
						   data_dma[i], false);
		if (ret)
			return ret;
		ret = mt6797_md1_cldma_set_pointer(&gpd[i]->next_gpd_ptr,
						   &gpd[i]->debug_id,
						   gpd_dma[(i + 1) % count],
						   true);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_rx_ring_init);

int mt6797_md1_cldma_tgpd_publish(struct mt6797_md1_cldma_tgpd *gpd)
{
	if (!gpd || (gpd->gpd_flags & MT6797_MD1_CLDMA_GPD_HWO) ||
	    !le16_to_cpu(gpd->data_buff_len))
		return -EINVAL;

	/* All descriptor and payload writes must precede ownership transfer. */
	dma_wmb();
	WRITE_ONCE(gpd->gpd_flags,
		   MT6797_MD1_CLDMA_TX_GPD_IOC | MT6797_MD1_CLDMA_GPD_HWO);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_tgpd_publish);

int mt6797_md1_cldma_tgpd_reclaim(struct mt6797_md1_cldma_tgpd *gpd,
				  u16 *data_len)
{
	u16 debug;

	if (!gpd || !data_len)
		return -EINVAL;
	if (READ_ONCE(gpd->gpd_flags) & MT6797_MD1_CLDMA_GPD_HWO)
		return -EAGAIN;
	dma_rmb();
	*data_len = le16_to_cpu(gpd->data_buff_len);
	if (!*data_len)
		return -EBADMSG;

	gpd->data_buff_len = 0;
	gpd->data_buff_bd_ptr = 0;
	debug = le16_to_cpu(gpd->debug_id) & 0xfff0;
	gpd->debug_id = cpu_to_le16(debug);
	WRITE_ONCE(gpd->gpd_flags, MT6797_MD1_CLDMA_TX_GPD_IOC);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_tgpd_reclaim);

int mt6797_md1_cldma_rgpd_publish(struct mt6797_md1_cldma_rgpd *gpd,
				  dma_addr_t data_dma, u16 data_allow_len)
{
	int ret;

	if (!gpd || !data_allow_len ||
	    (gpd->gpd_flags & MT6797_MD1_CLDMA_GPD_HWO))
		return -EINVAL;
	ret = mt6797_md1_cldma_set_pointer(&gpd->data_buff_bd_ptr,
					   &gpd->debug_id, data_dma, false);
	if (ret)
		return ret;
	gpd->data_allow_len = cpu_to_le16(data_allow_len);
	gpd->data_buff_len = 0;

	/* The replacement buffer and descriptor must be visible before HWO. */
	dma_wmb();
	WRITE_ONCE(gpd->gpd_flags,
		   MT6797_MD1_CLDMA_RX_GPD_IOC | MT6797_MD1_CLDMA_GPD_HWO);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_rgpd_publish);

int mt6797_md1_cldma_rgpd_consume(struct mt6797_md1_cldma_rgpd *gpd,
				  u16 *data_len)
{
	u16 allow;

	if (!gpd || !data_len)
		return -EINVAL;
	if (READ_ONCE(gpd->gpd_flags) & MT6797_MD1_CLDMA_GPD_HWO)
		return -EAGAIN;
	dma_rmb();
	allow = le16_to_cpu(gpd->data_allow_len);
	*data_len = le16_to_cpu(gpd->data_buff_len);
	if (!*data_len || *data_len > allow)
		return -EBADMSG;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_rgpd_consume);

void mt6797_md1_cldma_irq_decode(u32 tx_status, u32 tx_mask,
				 u32 rx_status, u32 rx_mask,
				 struct mt6797_md1_cldma_irq_pending *pending)
{
	u32 tx = tx_status & ~tx_mask;
	u32 rx = rx_status & ~rx_mask;

	if (!pending)
		return;
	pending->tx_done = tx & MT6797_MD1_CLDMA_ACTIVE_QUEUES;
	pending->rx_done = rx & MT6797_MD1_CLDMA_ACTIVE_QUEUES;
	pending->tx_error = (tx >> 16) & MT6797_MD1_CLDMA_ACTIVE_QUEUES;
	pending->rx_error = (rx >> 16) & MT6797_MD1_CLDMA_ACTIVE_QUEUES;
}
EXPORT_SYMBOL_GPL(mt6797_md1_cldma_irq_decode);
