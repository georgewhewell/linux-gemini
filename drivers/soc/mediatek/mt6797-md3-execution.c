// SPDX-License-Identifier: GPL-2.0-only
/* Explicit one-shot C2K execution to its first CCIF event. No TX, filesystem,
 * runtime response, CSS completion, registration or continued execution.
 * Active vendor: eccci/mt6797/ccif_c2k_platform.c md_ccif_let_md_go().
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-md1-cold-owner.h>
#include <linux/soc/mediatek/mt6797-md3-ccif.h>
#include <linux/soc/mediatek/mt6797-md3-execution.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#define MD3_WAKE_MS 1000
#define MD3_EVENT_MS 10000
#define MD3_CONFIG 0x360
#define MD3_STATUS 0x364
#define MD3_FEATURE_PATTERN 0x49434343

struct mt6797_md3_execution {
	struct device *dev;
	struct reset_control *reset;
	struct mt6797_md3_ccif *ccif;
	void __iomem *misc, *pll, *clk;
	struct mutex lock;
	spinlock_t rx_lock;
	wait_queue_head_t wait;
	bool attempted, lease, released, release_attempted, irqs, handshake, reset_held;
	bool event, wdt, sram, hs1;
	u32 event_mask, ccif_irqs, wdt_irqs, header[4], feature_words[18];
	u32 phase, wake_status;
	int error, cleanup_error;
};

/* IRQ context: copy bounded public protocol fields before acknowledging.
 * Ring notifications are recorded, never consumed. This driver cannot service
 * FS or other ring requests and therefore terminates rather than inventing an
 * ACK, advancing an RX pointer or advertising the missing AP service.
 */
static void md3_execution_irq(void *data, bool wdt, void __iomem *ap)
{
	struct mt6797_md3_execution *e = data;
	unsigned long flags;
	u32 mask, i;

	spin_lock_irqsave(&e->rx_lock, flags);
	if (wdt) {
		e->wdt = true;
		e->wdt_irqs++;
	} else {
		e->ccif_irqs++;
		mask = readl(ap + 0x10);
		e->event_mask |= mask;
		if (!e->event && (mask & BIT(7))) {
			for (i = 0; i < ARRAY_SIZE(e->header); i++)
				e->header[i] = readl(ap + 0x100 + i * 4);
			for (i = 0; i < ARRAY_SIZE(e->feature_words); i++)
				e->feature_words[i] = readl(ap + 0x110 + i * 4);
			rmb();
			e->sram = true;
			/* Header may not change before its doorbell ACK. */
			for (i = 0; i < ARRAY_SIZE(e->header); i++)
				if (readl(ap + 0x100 + i * 4) != e->header[i])
					e->sram = false;
		}
		/* A zero/spurious or out-of-range event is an observed protocol
		 * error, not authority to keep the firmware running indefinitely.
		 */
		writel(mask & 0xffff, ap + 0x14);
		e->event = true;
	}
	spin_unlock_irqrestore(&e->rx_lock, flags);
	wake_up(&e->wait);
}

static int md3_execution_live(struct mt6797_md3_execution *e)
{
	if (READ_ONCE(e->wdt))
		return -EIO;
	return mt6797_md1_cold_owner_md3_execution_live(e->dev, e->reset,
						       e->released);
}

/* SRAM layout is exactly header16 + md_query_ap_feature72. HS1 data[1]
 * is the control opcode zero, NOT the total frame length. Wire C2K control
 * channel is zero (different from arbitrary standard CCCI channel mapping).
 */
static int md3_execution_validate_hs1(struct mt6797_md3_execution *e)
{
	u32 i, j, mask;

	if (e->wdt)
		return -EIO;
	if (!e->event || !e->sram || !(e->event_mask & BIT(7)))
		return -EOPNOTSUPP;
	if (e->event_mask & ~0xff80U)
		return -EPROTO; /* exception/sequence-error/unknown indications */
	/* Vendor cold RX sequence is -1; asserted first packet must be zero.
	 * An unasserted packet follows the vendor's optional-sequence contract.
	 */
	if (e->header[1] || (e->header[2] & 0xffff) ||
	    ((e->header[2] & BIT(31)) && (e->header[2] & 0x7fff0000)) ||
	    e->header[3] != 0x5555ffff ||
	    e->feature_words[0] != MD3_FEATURE_PATTERN ||
	    e->feature_words[17] != MD3_FEATURE_PATTERN)
		return -EPROTO;
	for (i = 1; i < 17; i++)
		for (j = 0; j < 4; j++) {
			mask = (e->feature_words[i] >> (j * 8)) & 0xf;
			if (mask > 4)
				return -EPROTO;
		}
	e->hs1 = true;
	return 0;
}

/* Exact 16-bit active GPL writes; DSP PLL write and C2K_CONFIG core-hold
 * writes are commented out in the vendor and deliberately NOT introduced.
 * No AP PLL/clock-source change is invented from a diagnostic read.
 */
static int md3_execution_pll(struct mt6797_md3_execution *e)
{
	int ret = md3_execution_live(e);

	if (ret)
		return ret;
	writew(0x8805, e->pll + 0x08);
	writew(0x0005, e->pll + 0x08);
	writew(0x0001, e->pll + 0x08);
	writew(0, e->pll + 0x04);
	writew(0x0010, e->pll + 0x74);
	writew(readw(e->pll + 0x40) | BIT(15), e->pll + 0x40);
	writew(readw(e->clk + 0x234) | BIT(15), e->clk + 0x234);
	udelay(30);
	ret = md3_execution_live(e);
	if (ret)
		return ret;
	writew(0xc124, e->clk + 0x234);
	writew(0x8e43, e->clk + 0x10);
	writew(0xa207, e->clk + 0x29c);
	wmb();
	/* Readback only stable final programmed words; pulse writes above are
	 * order-sensitive and cannot be 'verified' by expecting their old value.
	 */
	if (readw(e->clk + 0x234) != 0xc124 ||
	    readw(e->clk + 0x10) != 0x8e43 ||
	    readw(e->clk + 0x29c) != 0xa207)
		return -EIO;
	return md3_execution_live(e);
}

/* Always drain IRQ callbacks before terminal reset, while keeping all clocks
 * and mappings. Never power down a partially changed domain. A failed reset
 * remains explicitly unknown, and still retains every resource until reboot.
 */
static void md3_execution_stop(struct mt6797_md3_execution *e)
{
	int ret;

	if (e->irqs) {
		mt6797_md3_ccif_observe_stop(e->ccif);
		e->irqs = false;
	}
	if (!e->lease)
		return;
	ret = reset_control_assert(e->reset);
	if (!ret) {
		ret = reset_control_status(e->reset);
		ret = ret < 0 ? ret : ret == 1 ? 0 : -EIO;
	}
	e->reset_held = !ret;
	e->cleanup_error = ret;
	if (!ret)
		e->released = false;
	if (e->handshake) {
		ret = mt6797_md1_cold_owner_md3_execution_update(e->dev,
							     0x368, BIT(1), 0);
		if (ret && !e->cleanup_error)
			e->cleanup_error = ret;
	}
	mt6797_md1_cold_owner_md3_execution_end(e->dev);
	e->lease = false;
}

static int md3_execution_sequence(struct mt6797_md3_execution *e)
{
	s64 deadline;
	u32 value;
	int ret;

	ret = mt6797_md1_cold_owner_md3_execution_begin(e->dev, e->reset);
	if (ret)
		return ret;
	e->lease = true;
	e->phase = 1;
	ret = mt6797_md3_ccif_observe_start(e->ccif, md3_execution_irq, e);
	if (ret)
		return ret;
	e->irqs = true;
	ret = md3_execution_live(e);
	if (ret)
		return ret;
	if (READ_ONCE(e->event))
		return -EUCLEAN; /* event while actual reset is still held */
	value = readl(e->misc + MD3_CONFIG);
	writel(value | BIT(3), e->misc + MD3_CONFIG);
	ret = mt6797_md1_cold_owner_md3_execution_update(e->dev, 0xf18, 0x44, 0x44);
	if (ret)
		return ret;
	ret = md3_execution_live(e);
	if (ret)
		return ret;
	/* Even a failed deassert can have reached hardware. stop() unconditionally
	 * asserts again because the execution lease exists, not because ret==0.
	 */
	e->release_attempted = true;
	ret = reset_control_deassert(e->reset);
	if (ret)
		return ret;
	e->released = true;
	e->phase = 2;
	ret = md3_execution_live(e);
	if (ret)
		return ret;
	e->handshake = true; /* update may partially reach the shared register */
	ret = mt6797_md1_cold_owner_md3_execution_update(e->dev, 0x368, BIT(1), BIT(1));
	if (ret)
		return ret;
	value = readl(e->misc + MD3_CONFIG);
	writel(value & ~GENMASK(12, 11), e->misc + MD3_CONFIG);
	deadline = ktime_to_ms(ktime_get()) + MD3_WAKE_MS;
	for (;;) {
		ret = md3_execution_live(e);
		if (ret)
			return ret;
		if (ktime_to_ms(ktime_get()) >= deadline)
			return -ETIMEDOUT;
		e->wake_status = readl(e->misc + MD3_STATUS);
		if (ktime_to_ms(ktime_get()) >= deadline)
			return -ETIMEDOUT;
		if (e->wake_status & BIT(1))
			break;
		if (ktime_to_ms(ktime_get()) >= deadline)
			return -ETIMEDOUT;
		usleep_range(100, 200);
	}
	e->phase = 3;
	ret = mt6797_md1_cold_owner_md3_execution_update(e->dev, 0x368, BIT(1), 0);
	if (ret)
		return ret;
	e->handshake = false;
	ret = mt6797_md1_cold_owner_md3_execution_update(e->dev, 0x220, GENMASK(15, 13), 0);
	if (ret)
		return ret;
	ret = md3_execution_pll(e);
	if (ret)
		return ret;
	e->phase = 4;
	deadline = ktime_to_ms(ktime_get()) + MD3_EVENT_MS;
	while (!READ_ONCE(e->event) && !READ_ONCE(e->wdt)) {
		ret = md3_execution_live(e);
		if (ret)
			return ret;
		if (ktime_to_ms(ktime_get()) >= deadline)
			return -ETIMEDOUT;
		wait_event_timeout(e->wait, READ_ONCE(e->event) || READ_ONCE(e->wdt),
				   msecs_to_jiffies(20));
	}
	if (ktime_to_ms(ktime_get()) >= deadline)
		return -ETIMEDOUT;
	ret = md3_execution_live(e);
	if (ret)
		return ret;
	e->phase = 5;
	return 0; /* Validate copied fields only after IRQs are drained. */
}

int mt6797_md3_execution_run(struct mt6797_md3_execution *e)
{
	int ret;

	if (!e)
		return -EINVAL;
	mutex_lock(&e->lock);
	if (e->attempted) {
		ret = -EALREADY;
		goto out;
	}
	e->attempted = true;
	ret = md3_execution_sequence(e);
	md3_execution_stop(e);
	if (!ret)
		ret = md3_execution_validate_hs1(e);
	if (!ret)
		ret = e->cleanup_error;
	e->error = ret;
out:
	mutex_unlock(&e->lock);
	return ret;
}

int mt6797_md3_execution_acquire(struct platform_device *pdev,
	struct reset_control *reset, struct mt6797_md3_ccif *ccif,
	struct mt6797_md3_execution **out)
{
	static const struct { const char *name; u64 base, size; } resources[] = {
		{ "c2k-misc", 0x1001b000, 0x1000 },
		{ "c2k-pll", 0x3a013000, 0x80 },
		{ "c2k-clock", 0x3a000000, 0x300 },
	};
	struct mt6797_md3_execution *e;
	void __iomem *maps[3];
	struct resource *resource;
	unsigned int i;

	if (!pdev || !reset || IS_ERR(reset) || !ccif || !out)
		return -EINVAL;
	e = devm_kzalloc(&pdev->dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, resources[i].name);
		if (!resource || resource->start != resources[i].base ||
		    resource_size(resource) != resources[i].size)
			return -EINVAL;
		maps[i] = devm_ioremap_resource(&pdev->dev, resource);
		if (IS_ERR(maps[i]))
			return PTR_ERR(maps[i]);
	}
	e->dev = &pdev->dev;
	e->reset = reset;
	e->ccif = ccif;
	e->misc = maps[0];
	e->pll = maps[1];
	e->clk = maps[2];
	e->error = -EAGAIN;
	mutex_init(&e->lock);
	spin_lock_init(&e->rx_lock);
	init_waitqueue_head(&e->wait);
	*out = e;
	return 0;
}

ssize_t mt6797_md3_execution_show(struct mt6797_md3_execution *e, char *buf)
{
	ssize_t len;
	unsigned int i;

	if (!e)
		return sysfs_emit(buf, "requested=0 execution_owned=0 runtime_sent=0 css_ready=0\n");
	mutex_lock(&e->lock);
	len = sysfs_emit(buf, "requested=1 attempted=%u phase=%u error=%d cleanup_error=%d reset_held=%u wake_status=%#x ccif_irqs=%u wdt_irqs=%u event_mask=%#x sram=%u hs1=%u execution_may_be_running=%u runtime_sent=0 css_ready=0 header=%08x,%08x,%08x,%08x features=",
		e->attempted, e->phase, e->error, e->cleanup_error, e->reset_held,
		e->wake_status, e->ccif_irqs, e->wdt_irqs, e->event_mask, e->sram,
		e->hs1, e->release_attempted && !e->reset_held, e->header[0], e->header[1], e->header[2], e->header[3]);
	/* Publish protocol feature descriptors only after a real validated HS1;
	 * arbitrary SRAM bytes from malformed frames are never exposed.
	 */
	if (e->hs1)
		for (i = 1; i < 17; i++)
			len += sysfs_emit_at(buf, len, "%s%08x", i == 1 ? "" : ",", e->feature_words[i]);
	len += sysfs_emit_at(buf, len, "\n");
	mutex_unlock(&e->lock);
	return len;
}

void mt6797_md3_execution_shutdown(struct mt6797_md3_execution *e)
{
	if (!e)
		return;
	mutex_lock(&e->lock);
	md3_execution_stop(e);
	mutex_unlock(&e->lock);
}
