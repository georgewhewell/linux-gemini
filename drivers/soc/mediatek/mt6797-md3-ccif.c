// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 C2K reset-held AP CCIF preparation. This is not a running transport.
 * Exact active modem_ccif_c2k.c layout, not the old modem_ccif.c 2MiB offset.
 */
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md3-ccif.h>
#include <linux/soc/mediatek/mt6797-md3-lk-handoff.h>
#include <dt-bindings/clock/mt6797-clk.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

#define MD3_AP_CCIF_BASE 0x1020b000
#define MD3_AP_CCIF_SIZE 0x1000
#define MD3_AP3_BASE 0x88400000
#define MD3_AP3_SIZE 0x200000
#define MD3_RING_START 0x19000
#define MD3_RING_HEADER 0xee0000ee
#define MD3_RING_FOOTER 0xff0000ff
#define MD3_FLOW_HEADER 0x464c4f57
#define MD3_RING_CTL 40
#define MD3_SRAM 0x100
#define MD3_SRAM_SIZE 512

struct mt6797_md3_ccif {
	struct device *dev;
	struct reset_control *reset;
	struct mutex lock;
	void __iomem *ap, *smem;
	struct clk_bulk_data clocks[2];
	int irq[2];
	bool attempted, clocks_on;
	int status;
	atomic_t unexpected_irq;
	atomic_t irq_enabled[2];
	mt6797_md3_ccif_observer observer;
	void *observer_data;
	bool observed;
};

static const u32 md3_rx_size[8] = { 10, 100, 100, 150, 50, 10, 10, 10 };
static const u32 md3_tx_size[8] = { 10, 100, 50, 50, 50, 10, 10, 10 };

static int md3_ccif_live(struct mt6797_md3_ccif *c)
{
	int ret;

	if (atomic_read(&c->unexpected_irq))
		return -EIO;
	ret = reset_control_status(c->reset);
	if (ret != 1)
		return ret < 0 ? ret : -EBUSY;
	return mt6797_md1_cold_owner_verify_md3_domain(c->dev, c->reset);
}

/* IRQs are reserved with NO_AUTOEN. Only the optional observer may enable
 * them after domain/clock/reset admission. Without that observer, fence any
 * unexpected delivery without accessing a potentially inaccessible device.
 */
static irqreturn_t md3_ccif_unexpected_irq(int irq, void *data)
{
	struct mt6797_md3_ccif *c = data;
	mt6797_md3_ccif_observer observer = smp_load_acquire(&c->observer);

	if (observer) {
		unsigned int index = irq == c->irq[1];

		observer(c->observer_data, index, c->ap);
		if (atomic_xchg(&c->irq_enabled[index], 0))
			disable_irq_nosync(irq);
		return IRQ_HANDLED;
	}
	atomic_set(&c->unexpected_irq, 1);
	disable_irq_nosync(irq);
	return IRQ_HANDLED;
}

static void md3_ccif_release_clocks(void *data)
{
	struct mt6797_md3_ccif *c = data;

	/* Registered before IRQ leases. Devres drains both IRQs before this action.
	 * The built-in platform owner remains bound after any hardware mutation.
	 */
	if (c->clocks_on)
		clk_bulk_disable_unprepare(ARRAY_SIZE(c->clocks), c->clocks);
}

static int md3_ccif_irq_spec(struct device *dev, unsigned int index, u32 number,
			     u32 type)
{
	struct of_phandle_args args;
	int ret = of_irq_parse_one(dev->of_node, index, &args);

	if (ret)
		return ret;
	ret = args.args_count == 3 && args.args[0] == GIC_SPI &&
			      args.args[1] == number && args.args[2] == type &&
			      of_device_is_compatible(
				      args.np, "mediatek,mt6797-sysirq") ?
		      0 :
		      -EINVAL;
	of_node_put(args.np);
	return ret;
}

static int md3_ccif_clock_spec(struct device *dev, unsigned int index, u32 id)
{
	struct of_phandle_args args;
	struct resource resource;
	int ret = of_parse_phandle_with_args(dev->of_node, "clocks",
					     "#clock-cells", index, &args);

	if (ret)
		return ret;
	ret = -EINVAL;
	if (args.args_count != 1 || args.args[0] != id ||
	    !of_device_is_compatible(args.np, "mediatek,mt6797-infracfg"))
		goto out;
	ret = of_address_to_resource(args.np, 0, &resource);
	if (!ret && (resource.start != 0x10001000 ||
		     resource_size(&resource) != 0x1000))
		ret = -ERANGE;
out:
	of_node_put(args.np);
	return ret;
}

int mt6797_md3_ccif_acquire(struct platform_device *pdev,
			    const struct mt6797_md3_lk_snapshot *s,
			    struct reset_control *reset,
			    struct mt6797_md3_ccif **out)
{
	struct device *dev;
	struct mt6797_md3_ccif *c;
	struct resource *resource;
	int ret, i;

	if (!pdev || !s || !reset || IS_ERR(reset) || !out)
		return -EINVAL;
	dev = &pdev->dev;
	/* Detached validated reservation only; NEVER map intermodem or MD1 RAM. */
	if (s->mpu_error || s->smem.base_phys != 0x88000000 ||
	    s->smem.ap_md3_offset != 0x400000 ||
	    s->smem.ap_md3_size != MD3_AP3_SIZE ||
	    s->mpu[0].start != MD3_AP3_BASE ||
	    s->mpu[0].end != MD3_AP3_BASE + MD3_AP3_SIZE - 1)
		return -ERANGE;
	if (of_count_phandle_with_args(dev->of_node, "clocks",
				       "#clock-cells") != 2 ||
	    of_irq_count(dev->of_node) != 2 ||
	    of_property_match_string(dev->of_node, "clock-names",
				     "ap-c2k-ccif-0") != 0 ||
	    of_property_match_string(dev->of_node, "clock-names",
				     "ap-c2k-ccif-1") != 1)
		return -EINVAL;
	ret = md3_ccif_irq_spec(dev, 0, 149, IRQ_TYPE_LEVEL_LOW);
	if (!ret)
		ret = md3_ccif_irq_spec(dev, 1, 287, IRQ_TYPE_EDGE_FALLING);
	if (!ret)
		ret = md3_ccif_clock_spec(dev, 0, CLK_INFRA_AP_C2K_CCIF_0);
	if (!ret)
		ret = md3_ccif_clock_spec(dev, 1, CLK_INFRA_AP_C2K_CCIF_1);
	if (ret)
		return ret;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"ap-c2k-ccif");
	if (!resource || resource->start != MD3_AP_CCIF_BASE ||
	    resource_size(resource) != MD3_AP_CCIF_SIZE)
		return -EINVAL;
	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->reset = reset;
	c->status = -EAGAIN;
	mutex_init(&c->lock);
	atomic_set(&c->unexpected_irq, 0);
	c->ap = devm_ioremap_resource(dev, resource);
	if (IS_ERR(c->ap))
		return PTR_ERR(c->ap);
	if (!devm_request_mem_region(dev, MD3_AP3_BASE, MD3_AP3_SIZE,
				     dev_name(dev)))
		return -EBUSY;
	c->smem = devm_ioremap_wc(dev, MD3_AP3_BASE, MD3_AP3_SIZE);
	if (!c->smem)
		return -ENOMEM;
	c->clocks[0].id = "ap-c2k-ccif-0";
	c->clocks[1].id = "ap-c2k-ccif-1";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(c->clocks), c->clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, md3_ccif_release_clocks, c);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(c->irq); i++) {
		c->irq[i] = platform_get_irq(pdev, i);
		if (c->irq[i] < 0)
			return c->irq[i];
		if (i && c->irq[0] == c->irq[1])
			return -EINVAL;
		ret = devm_request_irq(dev, c->irq[i], md3_ccif_unexpected_irq,
				       IRQF_NO_AUTOEN,
				       i ? "md3-held-wdt" : "md3-held-ccif", c);
		if (ret)
			return ret;
	}
	*out = c;
	return 0;
}

static int md3_ccif_write_verify(struct mt6797_md3_ccif *c, u32 offset,
				 u32 value)
{
	writel(value, c->smem + offset);
	return readl(c->smem + offset) == value ? 0 : -EIO;
}

/* All ring offsets and lengths are derived from the ACTIVE C2K contract.
 * Whole AP3 clear is the vendor first-boot action; no intermodem mapping exists.
 */
static int md3_ccif_initialize(struct mt6797_md3_ccif *c)
{
	u32 offset = MD3_RING_START, size, rx, tx, i, j;
	int ret;

	for (i = 0; i < ARRAY_SIZE(md3_rx_size); i++) {
		size = (md3_rx_size[i] + md3_tx_size[i]) * 1024 + MD3_RING_CTL;
		if (offset > MD3_AP3_SIZE - size)
			return -EOVERFLOW;
		offset += size;
	}
	if (offset > MD3_AP3_SIZE - 16)
		return -EOVERFLOW;
	ret = md3_ccif_live(c);
	if (ret)
		return ret;
	memset_io(c->smem, 0, MD3_AP3_SIZE);
	/* Publish WC zeroing before ring control/guard initialization. */
	wmb();
	offset = MD3_RING_START;
	for (i = 0; i < ARRAY_SIZE(md3_rx_size); i++) {
		rx = md3_rx_size[i] * 1024;
		tx = md3_tx_size[i] * 1024;
		size = rx + tx + MD3_RING_CTL;
		ret = md3_ccif_live(c);
		if (ret)
			return ret;
		const u32 control[] = {
			MD3_RING_HEADER, MD3_RING_HEADER, 0, 0, rx, 0, 0, tx
		};
		for (j = 0; j < ARRAY_SIZE(control); j++) {
			ret = md3_ccif_write_verify(c, offset + j * 4,
						    control[j]);
			if (ret)
				return ret;
		}
		ret = md3_ccif_write_verify(c, offset + size - 8,
					    MD3_RING_FOOTER);
		if (!ret)
			ret = md3_ccif_write_verify(c, offset + size - 4,
						    MD3_RING_FOOTER);
		if (ret)
			return ret;
		offset += size;
	}
	/* Vendor AP initializes FLOW/APbusy/MDbusy only. CTRL tail is MD-owned;
	 * zero from the cold clear does not claim firmware flow-control support.
	 */
	ret = md3_ccif_write_verify(c, offset, MD3_FLOW_HEADER);
	for (i = 1; !ret && i < 4; i++)
		if (readl(c->smem + offset + i * 4))
			ret = -EIO;
	if (ret)
		return ret;
	ret = md3_ccif_live(c);
	if (ret)
		return ret;
	/* Only AP CCIF arbitration/SRAM. No MD-side or intermodem PCCIF reset. */
	writel(1, c->ap);
	writel(0xffff, c->ap + 0x14);
	for (i = 0; i < MD3_SRAM_SIZE; i += 4)
		writel(0, c->ap + MD3_SRAM + i);
	/* Order initialization before final peripheral/SRAM readback. */
	wmb();
	if (readl(c->ap) != 1 || readl(c->ap + 0x04) || readl(c->ap + 0x10))
		return -EUCLEAN;
	for (i = 0; i < MD3_SRAM_SIZE; i += 4)
		if (readl(c->ap + MD3_SRAM + i))
			return -EIO;
	return md3_ccif_live(c);
}

int mt6797_md3_ccif_prepare(struct mt6797_md3_ccif *c)
{
	int ret;

	if (!c)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (c->attempted) {
		ret = -EALREADY;
		goto out;
	}
	c->attempted = true;
	ret = md3_ccif_live(c);
	if (ret)
		goto failed;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(c->clocks), c->clocks);
	if (ret)
		goto failed;
	c->clocks_on = true;
	ret = md3_ccif_initialize(c);
failed:
	c->status = ret;
out:
	mutex_unlock(&c->lock);
	return ret;
}

int mt6797_md3_ccif_status(struct mt6797_md3_ccif *c)
{
	int ret;

	if (!c)
		return -ENODEV;
	mutex_lock(&c->lock);
	ret = atomic_read(&c->unexpected_irq) ? -EIO : c->status;
	mutex_unlock(&c->lock);
	return ret;
}

/* c->lock excludes start/stop; callback never takes it. No resources are
 * released here. NO_AUTOEN ownership is balanced even if the ISR already
 * masked itself. synchronize_irq also drains that masked-but-running case.
 */
static void md3_ccif_observe_stop_locked(struct mt6797_md3_ccif *c)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c->irq); i++) {
		if (atomic_xchg(&c->irq_enabled[i], 0))
			disable_irq(c->irq[i]);
		synchronize_irq(c->irq[i]);
	}
	WRITE_ONCE(c->observer, NULL);
	c->observer_data = NULL;
}

int mt6797_md3_ccif_observe_start(struct mt6797_md3_ccif *c,
	mt6797_md3_ccif_observer observer, void *data)
{
	int ret;
	unsigned int i;

	if (!c || !observer || !data)
		return -EINVAL;
	mutex_lock(&c->lock);
	ret = -EPERM;
	if (c->observed || !c->attempted || c->status || !c->clocks_on)
		goto out;
	c->observed = true;
	ret = md3_ccif_live(c);
	if (ret)
		goto out;
	if (readl(c->ap + 0x10)) {
		ret = -EUCLEAN;
		goto out;
	}
	c->observer_data = data;
	smp_store_release(&c->observer, observer);
	for (i = 0; i < ARRAY_SIZE(c->irq); i++) {
		atomic_set(&c->irq_enabled[i], 1);
		enable_irq(c->irq[i]);
	}
	ret = 0;
out:
	mutex_unlock(&c->lock);
	return ret;
}

void mt6797_md3_ccif_observe_stop(struct mt6797_md3_ccif *c)
{
	if (!c)
		return;
	mutex_lock(&c->lock);
	md3_ccif_observe_stop_locked(c);
	mutex_unlock(&c->lock);
}
