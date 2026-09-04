// SPDX-License-Identifier: GPL-2.0-only
/* Single, fail-closed resource owner for the MT6797 MD1 modem. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "cldma.h"
#include "observer.h"
#include "power.h"
#include "protocol.h"

#define MT6797_MD1_DMA_BITS	36
#define MT6797_MD1_SPM_PWR_STATUS	0x180
#define MT6797_MD1_SPM_PWR_STATUS_2ND	0x184
#define MT6797_MD1_POWER_SAMPLES	3
#define MT6797_MD1_INFRA_CG_STA1	0x94
#define MT6797_MD1_INFRA_CCIF_GATES	(BIT(23) | BIT(26))

#define MT6797_MD1_CLDMA_UL_START	0x004
#define MT6797_MD1_CLDMA_UL_STATUS	0x050
#define MT6797_MD1_CLDMA_DL_START	0xa78
#define MT6797_MD1_CLDMA_DL_STATUS	0xab8

#define MT6797_MD1_CCIF_BUSY	0x004
#define MT6797_MD1_CCIF_RCHNUM	0x010

#define MT6797_MD1_CLDMA_SO_CFG		0xa04
#define MT6797_MD1_CLDMA_SO_CHECKSUM	0xa74
#define MT6797_MD1_CLDMA_SO_DEBUG_ID	0xac8
#define MT6797_MD1_CLDMA_L2_RX_MASK_CLEAR	0xc60
#define MT6797_MD1_CLDMA_L2_RX_MASK_SET	0xc68
#define MT6797_MD1_CLDMA_BUS_CFG		0xc90

#define MT6797_MD1_CLDMA_SO_START	0x3bc
#define MT6797_MD1_CLDMA_SO_STOP		0x3c4
#define MT6797_MD1_CLDMA_L2_RX_STATUS	0x450
#define MT6797_MD1_CLDMA_L2_RX_STATUS_1	0x454
#define MT6797_MD1_CLDMA_L3_RX_STATUS	0x470
#define MT6797_MD1_CLDMA_L3_RX_STATUS_1	0x474
#define MT6797_MD1_CLDMA_L3_RX_MASK_CLEAR	0x480
#define MT6797_MD1_CLDMA_L3_RX_MASK_CLEAR_1	0x484
#define MT6797_MD1_CLDMA_L3_RX_MASK_SET	0x488
#define MT6797_MD1_CLDMA_L3_RX_MASK_SET_1	0x48c

#define MT6797_MD1_CLDMA_RX_QUEUE	BIT(MT6797_CLDMA_RX_CONTROL_QUEUE)
#define MT6797_MD1_CLDMA_RX_DONE		MT6797_MD1_CLDMA_RX_QUEUE
#define MT6797_MD1_CLDMA_RX_EMPTY	(MT6797_MD1_CLDMA_RX_QUEUE << 8)
#define MT6797_MD1_CLDMA_RX_ERROR	(MT6797_MD1_CLDMA_RX_QUEUE << 16)
#define MT6797_MD1_CLDMA_RX_ACTIVE	(MT6797_MD1_CLDMA_RX_QUEUE << 24)
#define MT6797_MD1_CLDMA_RX_L2_OWNED	(MT6797_MD1_CLDMA_RX_DONE | \
					 MT6797_MD1_CLDMA_RX_EMPTY | \
					 MT6797_MD1_CLDMA_RX_ERROR | \
					 MT6797_MD1_CLDMA_RX_ACTIVE)
#define MT6797_MD1_CLDMA_RX_L2_ENABLED	(MT6797_MD1_CLDMA_RX_DONE | \
					 MT6797_MD1_CLDMA_RX_ERROR)
#define MT6797_MD1_CLDMA_RX_L3_QUEUE0	(BIT(0) | BIT(8) | BIT(16) | BIT(24))
#define MT6797_MD1_CLDMA_STOP_TIMEOUT_US	10000

enum mt6797_md1_reg {
	MT6797_MD1_AP_AO,
	MT6797_MD1_MD_AO,
	MT6797_MD1_AP_PDN,
	MT6797_MD1_MD_PDN,
	MT6797_MD1_AP_CCIF,
	MT6797_MD1_MD_CCIF,
};

struct mt6797_md1 {
	struct device *dev;
	void __iomem *regs[6];
	struct clk_bulk_data clocks[2];
	struct reset_control_bulk_data resets[2];
	struct regmap *spm;
	struct regmap *infracfg;
	enum mt6797_md1_power_state power_state;
	enum mt6797_md1_inherited_state inherited_state;
	enum mt6797_ccci_boot_state boot_state;
	struct mt6797_md1_snapshot inherited_snapshot;
	u32 power_status;
	u32 power_status_2nd;
	int irqs[3];
	/* Serializes the IRQ thread against teardown and fatal fencing. */
	struct mutex lifecycle_lock;
	struct mt6797_cldma_rgpd *rx_ring;
	dma_addr_t rx_ring_dma;
	u8 *rx_data;
	dma_addr_t rx_data_dma;
	unsigned int rx_consumer;
	bool clocks_enabled;
	bool resets_deasserted;
	bool cldma_irq_enabled;
	bool rx_started;
	bool fatal;
};

static const char * const mt6797_md1_reg_names[] = {
	"ap-ao",
	"md-ao",
	"ap-pdn",
	"md-pdn",
	"ap-ccif",
	"md-ccif",
};

static const char * const mt6797_md1_irq_names[] = {
	"cldma",
	"ccif",
	"wdt",
};

static int mt6797_md1_validate_memory(struct device *dev)
{
	struct device_node *memory;
	int count;
	unsigned int i;

	count = of_count_phandle_with_args(dev->of_node, "memory-region", NULL);
	if (count != 2)
		return dev_err_probe(dev, count < 0 ? count : -EINVAL,
				     "expected two reserved-memory regions\n");
	for (i = 0; i < 2; i++) {
		memory = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!memory)
			return dev_err_probe(dev, -EINVAL,
					     "missing reserved-memory region %u\n", i);
		of_node_put(memory);
	}

	return 0;
}

static int mt6797_md1_observe_power(struct device *dev,
				    struct mt6797_md1 *md1)
{
	enum mt6797_md1_power_state state;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < MT6797_MD1_POWER_SAMPLES; attempt++) {
		ret = regmap_read(md1->spm, MT6797_MD1_SPM_PWR_STATUS,
				  &md1->power_status);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read SPM power status\n");

		ret = regmap_read(md1->spm, MT6797_MD1_SPM_PWR_STATUS_2ND,
				  &md1->power_status_2nd);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read second SPM power status\n");

		state = mt6797_md1_power_state_decode(md1->power_status,
						      md1->power_status_2nd);
		md1->power_state = state;
		if (md1->power_state != MT6797_MD1_POWER_UNSTABLE)
			return 0;

		cpu_relax();
	}

	return dev_err_probe(dev, -EAGAIN,
			     "MD1 SPM power status did not stabilize\n");
}

static void
mt6797_md1_read_inherited_snapshot(struct mt6797_md1 *md1,
				   struct mt6797_md1_snapshot *snapshot)
{
	unsigned int i;

	for (i = 0; i < MT6797_MD1_CLDMA_QUEUES; i++) {
		snapshot->tx_start[i] = readl(md1->regs[MT6797_MD1_AP_PDN] +
					      MT6797_MD1_CLDMA_UL_START + i * 4);
		snapshot->rx_start[i] = readl(md1->regs[MT6797_MD1_AP_AO] +
					      MT6797_MD1_CLDMA_DL_START + i * 4);
	}

	snapshot->tx_active = readl(md1->regs[MT6797_MD1_AP_PDN] +
				    MT6797_MD1_CLDMA_UL_STATUS);
	snapshot->rx_active = readl(md1->regs[MT6797_MD1_AP_AO] +
				    MT6797_MD1_CLDMA_DL_STATUS);
	snapshot->ccif_busy = readl(md1->regs[MT6797_MD1_AP_CCIF] +
				    MT6797_MD1_CCIF_BUSY);
	snapshot->ccif_pending = readl(md1->regs[MT6797_MD1_AP_CCIF] +
				       MT6797_MD1_CCIF_RCHNUM);
}

static int mt6797_md1_observe_inherited_transport(struct device *dev,
						  struct mt6797_md1 *md1)
{
	struct mt6797_md1_snapshot *second = &md1->inherited_snapshot;
	struct mt6797_md1_snapshot first;
	bool stable;
	u32 clock_gates;
	int ret;

	md1->boot_state = MT6797_CCCI_BOOT_OFFLINE;
	if (md1->power_state != MT6797_MD1_POWERED) {
		md1->inherited_state = MT6797_MD1_INHERITED_POWER_OFF;
		return 0;
	}

	ret = regmap_read(md1->infracfg, MT6797_MD1_INFRA_CG_STA1,
			  &clock_gates);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read infrastructure clock gates\n");
	if (clock_gates & MT6797_MD1_INFRA_CCIF_GATES) {
		md1->inherited_state = MT6797_MD1_INHERITED_CLOCK_GATED;
		return 0;
	}

	mt6797_md1_read_inherited_snapshot(md1, &first);
	mt6797_md1_read_inherited_snapshot(md1, second);
	stable = mt6797_md1_snapshot_equal(&first, second);
	if (!stable) {
		md1->inherited_state = MT6797_MD1_INHERITED_UNSTABLE;
		return 0;
	}

	md1->inherited_state =
		mt6797_md1_snapshot_classify(second);
	return 0;
}

static const char *
mt6797_md1_inherited_state_name(enum mt6797_md1_inherited_state state)
{
	switch (state) {
	case MT6797_MD1_INHERITED_POWER_OFF:
		return "power-off";
	case MT6797_MD1_INHERITED_CLOCK_GATED:
		return "clock-gated";
	case MT6797_MD1_INHERITED_QUIESCENT:
		return "quiescent-unconfigured";
	case MT6797_MD1_INHERITED_CCIF_PENDING:
		return "ccif-pending";
	case MT6797_MD1_INHERITED_FOREIGN_RINGS:
		return "foreign-rings";
	case MT6797_MD1_INHERITED_UNSTABLE:
		return "unstable";
	}

	return "invalid";
}

static void mt6797_md1_cldma_mask_ack(struct mt6797_md1 *md1)
{
	u32 status;

	writel(~0U,
	       md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_L2_RX_MASK_SET);
	writel(~0U, md1->regs[MT6797_MD1_AP_PDN] +
	       MT6797_MD1_CLDMA_L3_RX_MASK_SET);
	writel(~0U, md1->regs[MT6797_MD1_AP_PDN] +
	       MT6797_MD1_CLDMA_L3_RX_MASK_SET_1);

	status = readl(md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS);
	if (status)
		writel(status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS);
	status = readl(md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS_1);
	if (status)
		writel(status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS_1);
	status = readl(md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS);
	if (status)
		writel(status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS);
	status = readl(md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS_1);
	if (status)
		writel(status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS_1);
	readl(md1->regs[MT6797_MD1_AP_PDN] +
	      MT6797_MD1_CLDMA_L2_RX_STATUS);
}

static int mt6797_md1_cldma_stop_locked(struct mt6797_md1 *md1)
{
	u32 status;
	int ret;

	if (!md1->resets_deasserted)
		return 0;

	writel(MT6797_MD1_CLDMA_RX_L2_ENABLED,
	       md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_L2_RX_MASK_SET);
	writel(MT6797_MD1_CLDMA_RX_QUEUE,
	       md1->regs[MT6797_MD1_AP_PDN] + MT6797_MD1_CLDMA_SO_STOP);
	readl(md1->regs[MT6797_MD1_AP_PDN] + MT6797_MD1_CLDMA_SO_STOP);
	ret = readl_poll_timeout(md1->regs[MT6797_MD1_AP_AO] +
				 MT6797_MD1_CLDMA_DL_STATUS,
				 status,
				 !(status & MT6797_MD1_CLDMA_RX_QUEUE),
				 10, MT6797_MD1_CLDMA_STOP_TIMEOUT_US);
	mt6797_md1_cldma_mask_ack(md1);
	md1->rx_started = false;

	return ret;
}

static void mt6797_md1_cldma_fence_locked(struct device *dev,
					  struct mt6797_md1 *md1,
					  const char *reason)
{
	int ret;

	md1->fatal = true;
	mt6797_ccci_boot_abort(&md1->boot_state);
	ret = mt6797_md1_cldma_stop_locked(md1);
	if (ret)
		dev_err(dev, "CLDMA fatal fence (%s), queue stop timed out\n",
			reason);
	else
		dev_err(dev, "CLDMA fatal fence: %s\n", reason);
}

static irqreturn_t mt6797_md1_cldma_irq_thread(int irq, void *data)
{
	struct mt6797_md1 *md1 = data;
	struct device *dev = md1->dev;
	struct mt6797_ccci_header_fields fields;
	struct mt6797_cldma_rgpd *gpd;
	const char *reason;
	void *buffer;
	u32 l2_status;
	u32 l2_status_1;
	u32 l3_status;
	u32 l3_status_1;
	unsigned int next;
	int event;

	mutex_lock(&md1->lifecycle_lock);
	if (!md1->rx_started || md1->fatal) {
		mt6797_md1_cldma_mask_ack(md1);
		mutex_unlock(&md1->lifecycle_lock);
		return IRQ_HANDLED;
	}

	l2_status = readl(md1->regs[MT6797_MD1_AP_PDN] +
			  MT6797_MD1_CLDMA_L2_RX_STATUS);
	l2_status_1 = readl(md1->regs[MT6797_MD1_AP_PDN] +
			    MT6797_MD1_CLDMA_L2_RX_STATUS_1);
	l3_status = readl(md1->regs[MT6797_MD1_AP_PDN] +
			  MT6797_MD1_CLDMA_L3_RX_STATUS);
	l3_status_1 = readl(md1->regs[MT6797_MD1_AP_PDN] +
			    MT6797_MD1_CLDMA_L3_RX_STATUS_1);
	writel(MT6797_MD1_CLDMA_RX_L2_ENABLED,
	       md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_L2_RX_MASK_SET);
	if (l2_status)
		writel(l2_status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS);
	if (l2_status_1)
		writel(l2_status_1, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L2_RX_STATUS_1);
	if (l3_status)
		writel(l3_status, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS);
	if (l3_status_1)
		writel(l3_status_1, md1->regs[MT6797_MD1_AP_PDN] +
		       MT6797_MD1_CLDMA_L3_RX_STATUS_1);

	if ((l2_status & ~MT6797_MD1_CLDMA_RX_L2_OWNED) || l2_status_1) {
		reason = "interrupt from an unowned queue";
		goto fence;
	}
	if ((l2_status & MT6797_MD1_CLDMA_RX_ERROR) || l3_status ||
	    l3_status_1) {
		reason = "receive descriptor error";
		goto fence;
	}
	if (!(l2_status & MT6797_MD1_CLDMA_RX_DONE)) {
		reason = "interrupt without queue-0 completion";
		goto fence;
	}

	dma_rmb();
	gpd = &md1->rx_ring[md1->rx_consumer];
	buffer = md1->rx_data +
		 md1->rx_consumer * MT6797_CLDMA_RX_BUFFER_SIZE;
	event = mt6797_cldma_rx_descriptor_read(gpd, buffer,
						MT6797_CLDMA_RX_BUFFER_SIZE, &fields);
	if (event) {
		reason = "invalid queue-0 descriptor";
		goto fence;
	}

	next = (md1->rx_consumer + 1) % MT6797_CLDMA_RX_RING_COUNT;
	if (!(READ_ONCE(md1->rx_ring[next].flags) & MT6797_CLDMA_GPD_HWO)) {
		reason = "multiple packets before HS1 fence";
		goto fence;
	}
	event = mt6797_ccci_boot_receive(&md1->boot_state, &fields);
	if (event != MT6797_CCCI_BOOT_EVENT_HS1) {
		reason = "first control packet was not HS1";
		goto fence;
	}

	md1->rx_consumer = next;
	event = mt6797_md1_cldma_stop_locked(md1);
	if (event) {
		reason = "HS1 queue stop timed out";
		goto fence;
	}
	dev_info(dev, "received CCCI HS1; fenced before runtime-data TX\n");
out:
	mutex_unlock(&md1->lifecycle_lock);
	return IRQ_HANDLED;

fence:
	mt6797_md1_cldma_fence_locked(dev, md1, reason);
	goto out;
}

static int mt6797_md1_cldma_alloc(struct mt6797_md1 *md1)
{
	size_t data_size = MT6797_CLDMA_RX_RING_COUNT *
			   MT6797_CLDMA_RX_BUFFER_SIZE;
	size_t ring_size = MT6797_CLDMA_RX_RING_COUNT * sizeof(*md1->rx_ring);
	int ret;

	md1->rx_ring = dma_alloc_coherent(md1->dev, ring_size,
					  &md1->rx_ring_dma, GFP_KERNEL);
	if (!md1->rx_ring)
		return -ENOMEM;

	md1->rx_data = dma_alloc_coherent(md1->dev, data_size,
					  &md1->rx_data_dma, GFP_KERNEL);
	if (!md1->rx_data) {
		ret = -ENOMEM;
		goto free_ring;
	}

	ret = mt6797_cldma_rx_ring_build(md1->rx_ring,
					 MT6797_CLDMA_RX_RING_COUNT,
					 md1->rx_ring_dma, md1->rx_data_dma,
					 MT6797_CLDMA_RX_BUFFER_SIZE);
	if (ret)
		goto free_data;

	return 0;

free_data:
	dma_free_coherent(md1->dev, data_size, md1->rx_data,
			  md1->rx_data_dma);
	md1->rx_data = NULL;
free_ring:
	dma_free_coherent(md1->dev, ring_size, md1->rx_ring,
			  md1->rx_ring_dma);
	md1->rx_ring = NULL;
	return ret;
}

static void mt6797_md1_cldma_free(struct mt6797_md1 *md1)
{
	size_t data_size = MT6797_CLDMA_RX_RING_COUNT *
			   MT6797_CLDMA_RX_BUFFER_SIZE;
	size_t ring_size = MT6797_CLDMA_RX_RING_COUNT * sizeof(*md1->rx_ring);

	if (md1->rx_data)
		dma_free_coherent(md1->dev, data_size, md1->rx_data,
				  md1->rx_data_dma);
	if (md1->rx_ring)
		dma_free_coherent(md1->dev, ring_size, md1->rx_ring,
				  md1->rx_ring_dma);
	md1->rx_data = NULL;
	md1->rx_ring = NULL;
}

static int mt6797_md1_cldma_reset(struct mt6797_md1 *md1)
{
	size_t num_resets = ARRAY_SIZE(md1->resets);
	int ret;

	ret = reset_control_bulk_assert(num_resets, md1->resets);
	if (ret)
		return dev_err_probe(md1->dev, ret,
				     "failed to assert CLDMA resets\n");
	usleep_range(10, 20);
	ret = reset_control_bulk_deassert(num_resets, md1->resets);
	if (ret) {
		reset_control_bulk_assert(num_resets, md1->resets);
		return dev_err_probe(md1->dev, ret,
				     "failed to deassert CLDMA resets\n");
	}
	md1->resets_deasserted = true;

	return 0;
}

static int mt6797_md1_cldma_start(struct mt6797_md1 *md1)
{
	u32 value;
	int ret;

	/* Start from a reset-owned block with every interrupt source masked. */
	mt6797_md1_cldma_mask_ack(md1);

	value = readl(md1->regs[MT6797_MD1_AP_AO] +
		      MT6797_MD1_CLDMA_SO_CFG);
	value |= BIT(0) | BIT(2) | BIT(6);
	value &= ~BIT(4);
	writel(value, md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_SO_CFG);
	value = readl(md1->regs[MT6797_MD1_AP_AO] +
		      MT6797_MD1_CLDMA_BUS_CFG);
	writel(value | BIT(1), md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_BUS_CFG);
	writel(0, md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_SO_CHECKSUM);
	writel(0, md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_SO_DEBUG_ID);
	writel(lower_32_bits(md1->rx_ring_dma),
	       md1->regs[MT6797_MD1_AP_AO] + MT6797_MD1_CLDMA_DL_START);

	mt6797_ccci_boot_begin(&md1->boot_state);
	dma_wmb();
	writel(MT6797_MD1_CLDMA_RX_QUEUE,
	       md1->regs[MT6797_MD1_AP_PDN] + MT6797_MD1_CLDMA_SO_START);
	readl(md1->regs[MT6797_MD1_AP_PDN] + MT6797_MD1_CLDMA_SO_START);
	ret = readl_poll_timeout(md1->regs[MT6797_MD1_AP_AO] +
				 MT6797_MD1_CLDMA_DL_STATUS,
				 value, value & MT6797_MD1_CLDMA_RX_QUEUE,
				 10, MT6797_MD1_CLDMA_STOP_TIMEOUT_US);
	if (ret) {
		mt6797_ccci_boot_abort(&md1->boot_state);
		mt6797_md1_cldma_stop_locked(md1);
		return dev_err_probe(md1->dev, ret,
				     "CLDMA control RX queue did not start\n");
	}
	md1->rx_started = true;

	/* Queue zero uses GPDs only; unmask its GPD-error details and L2 IRQ. */
	writel(MT6797_MD1_CLDMA_RX_L3_QUEUE0,
	       md1->regs[MT6797_MD1_AP_PDN] +
	       MT6797_MD1_CLDMA_L3_RX_MASK_CLEAR);
	writel(BIT(0) | BIT(8) | BIT(16) | BIT(31),
	       md1->regs[MT6797_MD1_AP_PDN] +
	       MT6797_MD1_CLDMA_L3_RX_MASK_CLEAR_1);
	writel(MT6797_MD1_CLDMA_RX_L2_ENABLED,
	       md1->regs[MT6797_MD1_AP_AO] +
	       MT6797_MD1_CLDMA_L2_RX_MASK_CLEAR);
	enable_irq(md1->irqs[0]);
	md1->cldma_irq_enabled = true;

	return 0;
}

static void mt6797_md1_cldma_release(struct mt6797_md1 *md1)
{
	bool quiesced = !md1->resets_deasserted;
	int ret;

	if (md1->cldma_irq_enabled) {
		disable_irq(md1->irqs[0]);
		md1->cldma_irq_enabled = false;
	}

	mutex_lock(&md1->lifecycle_lock);
	if (md1->clocks_enabled && md1->resets_deasserted) {
		ret = mt6797_md1_cldma_stop_locked(md1);
		if (ret)
			dev_err(md1->dev, "CLDMA queue stop timed out during teardown\n");
		else
			quiesced = true;
	}
	if (md1->resets_deasserted) {
		ret = reset_control_bulk_assert(ARRAY_SIZE(md1->resets),
						md1->resets);
		if (ret)
			dev_err(md1->dev, "failed to assert CLDMA resets: %d\n",
				ret);
		else
			md1->resets_deasserted = false;
		if (!ret)
			quiesced = true;
	}
	mutex_unlock(&md1->lifecycle_lock);

	if (!quiesced) {
		dev_crit(md1->dev,
			 "retaining CLDMA clock and DMA leases after unsafe teardown\n");
		return;
	}
	if (md1->clocks_enabled) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(md1->clocks), md1->clocks);
		md1->clocks_enabled = false;
	}
	mt6797_md1_cldma_free(md1);
}

static int mt6797_md1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_md1 *md1;
	size_t num_resets;
	unsigned int i;
	int ret;

	md1 = devm_kzalloc(dev, sizeof(*md1), GFP_KERNEL);
	if (!md1)
		return -ENOMEM;
	md1->dev = dev;
	mutex_init(&md1->lifecycle_lock);

	md1->spm = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "mediatek,spm");
	if (IS_ERR(md1->spm))
		return dev_err_probe(dev, PTR_ERR(md1->spm),
				     "failed to acquire SPM syscon\n");
	md1->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node,
							"mediatek,infracfg");
	if (IS_ERR(md1->infracfg))
		return dev_err_probe(dev, PTR_ERR(md1->infracfg),
				     "failed to acquire infracfg syscon\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_reg_names); i++) {
		const char *name = mt6797_md1_reg_names[i];

		md1->regs[i] = devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(md1->regs[i]))
			return PTR_ERR(md1->regs[i]);
	}

	md1->clocks[0].id = "ccif-ap";
	md1->clocks[1].id = "ccif-md";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(md1->clocks), md1->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire clocks\n");

	md1->resets[0].id = "cldma-pdn";
	md1->resets[1].id = "cldma-ao";
	num_resets = ARRAY_SIZE(md1->resets);
	ret = devm_reset_control_bulk_get_exclusive(dev, num_resets, md1->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire resets\n");

	for (i = 0; i < ARRAY_SIZE(mt6797_md1_irq_names); i++) {
		const char *name = mt6797_md1_irq_names[i];

		md1->irqs[i] = platform_get_irq_byname(pdev, name);
		if (md1->irqs[i] < 0)
			return md1->irqs[i];
	}

	ret = mt6797_md1_validate_memory(dev);
	if (ret)
		return ret;
	ret = dma_set_mask(dev, DMA_BIT_MASK(MT6797_MD1_DMA_BITS));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set 36-bit DMA mask\n");
	/* The descriptor links are 36-bit, but RQSAR itself is only 32-bit. */
	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set 32-bit coherent DMA mask\n");
	ret = mt6797_md1_observe_power(dev, md1);
	if (ret)
		return ret;
	if (md1->power_state != MT6797_MD1_POWERED)
		return dev_err_probe(dev, -ENODEV,
				     "MD1 is not powered; power activation is out of scope\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(md1->clocks), md1->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable CLDMA clocks\n");
	md1->clocks_enabled = true;
	ret = mt6797_md1_observe_inherited_transport(dev, md1);
	if (ret)
		goto disable_clocks;
	if (md1->inherited_state != MT6797_MD1_INHERITED_QUIESCENT) {
		ret = dev_err_probe(dev, -EBUSY,
				    "refusing inherited transport state %s\n",
				    mt6797_md1_inherited_state_name(md1->inherited_state));
		goto disable_clocks;
	}

	ret = mt6797_md1_cldma_alloc(md1);
	if (ret)
		goto disable_clocks;
	ret = devm_request_threaded_irq(dev, md1->irqs[0], NULL,
					mt6797_md1_cldma_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), md1);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed to request CLDMA IRQ\n");
		goto free_ring;
	}
	ret = mt6797_md1_cldma_reset(md1);
	if (ret)
		goto free_ring;
	ret = mt6797_md1_cldma_start(md1);
	if (ret)
		goto release_cldma;

	platform_set_drvdata(pdev, md1);
	dev_info(dev, "owner-created CLDMA control RX ring waiting for CCCI HS1\n");
	return 0;

release_cldma:
	mt6797_md1_cldma_release(md1);
	return ret;
free_ring:
	mt6797_md1_cldma_free(md1);
disable_clocks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(md1->clocks), md1->clocks);
	md1->clocks_enabled = false;
	return ret;
}

static void mt6797_md1_remove(struct platform_device *pdev)
{
	struct mt6797_md1 *md1 = platform_get_drvdata(pdev);

	mt6797_md1_cldma_release(md1);
}

static const struct of_device_id mt6797_md1_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_of_match);

static struct platform_driver mt6797_md1_driver = {
	.probe = mt6797_md1_probe,
	.remove = mt6797_md1_remove,
	.driver = {
		.name = "mt6797-md1",
		.of_match_table = mt6797_md1_of_match,
	},
};
module_platform_driver(mt6797_md1_driver);

MODULE_DESCRIPTION("MediaTek MT6797 MD1 resource owner");
MODULE_LICENSE("GPL");
