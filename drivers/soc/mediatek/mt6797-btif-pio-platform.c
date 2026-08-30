// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 BTIF PIO platform acquisition and IRQ adapter.
 *
 * Vendor evidence deliberately limits this adapter to the BTIF core resource:
 *
 * - MT6797's vendor DT describes BTIF as 0x1100c000/0x1000 with one
 *   level-low GIC SPI 130 interrupt (arch/arm64/boot/dts/mt6797.dtsi:2511-2514);
 * - that DT also names btifc and apdmac clocks (mt6797.dtsi:2515-2516), but
 *   no reviewed CONSYS owner establishes when either can be enabled here;
 * - the vendor registration path requests the IRQ then leaves it disabled
 *   (drivers/misc/mediatek/btif/common/mtk_btif.c:1472-1481), and its teardown
 *   uses synchronous disable_irq() (mtk_btif.c:972-980).
 *
 * Thus probe maps no APDMA resource, never obtains or votes a clock, and
 * requests a threaded core IRQ with IRQF_NO_AUTOEN. A future owner must still
 * establish CONSYS power/reset/wake, pads, mandatory-STP handover, firmware,
 * and an explicit experiment parameter before PIO register setup or IRQ enable
 * is possible. No Gemini DT node matches this driver in this change.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_PIO_PLATFORM_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio.h>
#include <linux/soc/mediatek/mt6797-btif-pio-platform.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mtk-stp.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define MT6797_BTIF_CORE_BASE	0x1100c000
#define MT6797_BTIF_CORE_SIZE	0x1000
#define MT6797_BTIF_CORE_SPI	130

static bool enable_experimental_platform_adapter;
module_param_named(enable_experimental_platform_adapter,
		   enable_experimental_platform_adapter, bool, 0400);
MODULE_PARM_DESC(enable_experimental_platform_adapter,
		 "allow the dormant MT6797 BTIF PIO platform experiment (default: false)");

enum mt6797_btif_pio_platform_state {
	MT6797_BTIF_PIO_PLATFORM_READY,
	MT6797_BTIF_PIO_PLATFORM_LEASED,
	MT6797_BTIF_PIO_PLATFORM_ACTIVE,
	MT6797_BTIF_PIO_PLATFORM_QUIESCING,
	MT6797_BTIF_PIO_PLATFORM_FENCED,
};

struct mt6797_btif_pio_platform;

struct mt6797_btif_pio_platform_lease {
	struct mt6797_btif_pio_platform *adapter;
};

struct mt6797_btif_pio_platform_irq_ops {
	void (*enable)(void *context, int irq);
	void (*disable_nosync)(void *context, int irq);
	void (*disable_sync)(void *context, int irq);
};

struct mt6797_btif_pio_platform {
	struct device *dev;
	void __iomem *base;
	struct mutex state_lock;
	struct completion close_complete;
	struct mt6797_btif_pio *pio;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_btif_pio_platform_lease lease;
	const struct mt6797_btif_pio_platform_irq_ops *irq_ops;
	void *irq_context;
	int irq;
	bool irq_enabled;
	enum mt6797_btif_pio_platform_state state;
};

static void mt6797_btif_pio_platform_irq_enable(void *context, int irq)
{
	enable_irq(irq);
}

static void mt6797_btif_pio_platform_irq_disable_nosync(void *context, int irq)
{
	disable_irq_nosync(irq);
}

static void mt6797_btif_pio_platform_irq_disable_sync(void *context, int irq)
{
	disable_irq(irq);
}

static const struct mt6797_btif_pio_platform_irq_ops
		mt6797_btif_pio_platform_linux_irq_ops = {
	.enable = mt6797_btif_pio_platform_irq_enable,
	.disable_nosync = mt6797_btif_pio_platform_irq_disable_nosync,
	.disable_sync = mt6797_btif_pio_platform_irq_disable_sync,
};

static u32 mt6797_btif_pio_platform_read32(void *context, u32 offset)
{
	struct mt6797_btif_pio_platform *adapter = context;

	return readl(adapter->base + offset);
}

static void mt6797_btif_pio_platform_write32(void *context, u32 offset,
					      u32 value)
{
	struct mt6797_btif_pio_platform *adapter = context;

	writel(value, adapter->base + offset);
}

static u8 mt6797_btif_pio_platform_read8(void *context, u32 offset)
{
	struct mt6797_btif_pio_platform *adapter = context;

	return readb(adapter->base + offset);
}

static void mt6797_btif_pio_platform_write8(void *context, u32 offset,
					     u8 value)
{
	struct mt6797_btif_pio_platform *adapter = context;

	writeb(value, adapter->base + offset);
}

static bool mt6797_btif_pio_platform_resource_valid(const struct resource *res)
{
	return res && (res->flags & IORESOURCE_MEM) &&
		res->start == MT6797_BTIF_CORE_BASE &&
		res->end == MT6797_BTIF_CORE_BASE + MT6797_BTIF_CORE_SIZE - 1;
}

static bool mt6797_btif_pio_platform_irq_contract_valid(int irq_count,
						 unsigned int trigger_type)
{
	return irq_count == 1 &&
		(trigger_type & IRQ_TYPE_SENSE_MASK) == IRQ_TYPE_LEVEL_LOW;
}

static bool mt6797_btif_pio_platform_irq_spec_valid(
		const struct of_phandle_args *spec)
{
	return spec && spec->args_count == 3 &&
		spec->args[0] == GIC_SPI &&
		spec->args[1] == MT6797_BTIF_CORE_SPI &&
		spec->args[2] == IRQ_TYPE_LEVEL_LOW;
}

static int mt6797_btif_pio_platform_of_irq_valid(struct device_node *np)
{
	struct of_phandle_args spec;
	int ret;

	if (!np)
		return -EINVAL;
	ret = of_irq_parse_one(np, 0, &spec);
	if (ret)
		return ret;
	ret = mt6797_btif_pio_platform_irq_spec_valid(&spec) ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
}

static int mt6797_btif_pio_platform_prepare(
		struct mt6797_btif_pio_platform *adapter,
		const struct mt6797_btif_pio_io_ops *io_ops)
{
	adapter->pio = mt6797_btif_pio_create(io_ops);
	if (IS_ERR(adapter->pio))
		return PTR_ERR(adapter->pio);

	adapter->pio_lease = mt6797_btif_pio_acquire(adapter->pio);
	if (IS_ERR(adapter->pio_lease)) {
		int ret = PTR_ERR(adapter->pio_lease);

		mt6797_btif_pio_destroy(adapter->pio);
		adapter->pio = NULL;
		return ret;
	}

	adapter->lease.adapter = adapter;
	adapter->state = MT6797_BTIF_PIO_PLATFORM_READY;
	return 0;
}

static void mt6797_btif_pio_platform_destroy(
		struct mt6797_btif_pio_platform *adapter)
{
	if (!adapter)
		return;

	mt6797_btif_pio_platform_close(&adapter->lease);
	mt6797_btif_pio_destroy(adapter->pio);
	adapter->pio = NULL;
	adapter->pio_lease = NULL;
	adapter->lease.adapter = NULL;
}

static const struct mt6797_btif_pio_platform_lease *
mt6797_btif_pio_platform_reserve(struct mt6797_btif_pio_platform *adapter)
{
	const struct mt6797_btif_pio_platform_lease *lease;

	mutex_lock(&adapter->state_lock);
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_READY) {
		adapter->state = MT6797_BTIF_PIO_PLATFORM_LEASED;
		lease = &adapter->lease;
	} else if (adapter->state == MT6797_BTIF_PIO_PLATFORM_FENCED) {
		lease = ERR_PTR(-EPIPE);
	} else {
		lease = ERR_PTR(-EBUSY);
	}
	mutex_unlock(&adapter->state_lock);

	return lease;
}

const struct mt6797_btif_pio_platform_lease *
mt6797_btif_pio_platform_acquire(struct device *dev)
{
	struct mt6797_btif_pio_platform *adapter;

	if (!dev)
		return ERR_PTR(-EINVAL);
	adapter = dev_get_drvdata(dev);
	if (!adapter || !adapter->pio_lease)
		return ERR_PTR(-ENODEV);

	return mt6797_btif_pio_platform_reserve(adapter);
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_platform_acquire);

int mt6797_btif_pio_platform_attach_full_stp(
		const struct mt6797_btif_pio_platform_lease *platform_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease,
		const struct mt6797_consys_stp_session_lease *stp_lease)
{
	struct mt6797_btif_pio_platform *adapter;
	int ret;

	if (!platform_lease || !platform_lease->adapter || !runtime_lease ||
	    !stp_lease)
		return -EINVAL;
	if (!READ_ONCE(enable_experimental_platform_adapter))
		return -EPERM;

	adapter = platform_lease->adapter;
	mutex_lock(&adapter->state_lock);
	if (platform_lease != &adapter->lease ||
	    adapter->state == MT6797_BTIF_PIO_PLATFORM_FENCED) {
		ret = -ESHUTDOWN;
	} else if (adapter->state != MT6797_BTIF_PIO_PLATFORM_LEASED) {
		ret = -EBUSY;
	} else {
		/* PIO setup completes before the masked Linux IRQ can run. */
		ret = mt6797_btif_pio_attach_full_stp(adapter->pio_lease,
						      runtime_lease, stp_lease);
		if (!ret) {
			adapter->state = MT6797_BTIF_PIO_PLATFORM_ACTIVE;
			adapter->irq_enabled = true;
			adapter->irq_ops->enable(adapter->irq_context, adapter->irq);
		}
	}
	mutex_unlock(&adapter->state_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_platform_attach_full_stp);

void mt6797_btif_pio_platform_close(
		const struct mt6797_btif_pio_platform_lease *platform_lease)
{
	struct mt6797_btif_pio_platform *adapter;
	bool disable_irq = false;

	if (!platform_lease || !platform_lease->adapter)
		return;
	adapter = platform_lease->adapter;

	mutex_lock(&adapter->state_lock);
	if (platform_lease != &adapter->lease ||
	    adapter->state == MT6797_BTIF_PIO_PLATFORM_FENCED) {
		mutex_unlock(&adapter->state_lock);
		return;
	}
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_QUIESCING) {
		mutex_unlock(&adapter->state_lock);
		wait_for_completion(&adapter->close_complete);
		return;
	}

	disable_irq = adapter->irq_enabled;
	adapter->irq_enabled = false;
	adapter->state = MT6797_BTIF_PIO_PLATFORM_QUIESCING;
	reinit_completion(&adapter->close_complete);
	mutex_unlock(&adapter->state_lock);

	/* Synchronous IRQ quiesce precedes the PIO IER clear and lease fence. */
	if (disable_irq)
		adapter->irq_ops->disable_sync(adapter->irq_context, adapter->irq);
	mt6797_btif_pio_close(adapter->pio);

	mutex_lock(&adapter->state_lock);
	adapter->state = MT6797_BTIF_PIO_PLATFORM_FENCED;
	complete_all(&adapter->close_complete);
	mutex_unlock(&adapter->state_lock);
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_platform_close);

static void mt6797_btif_pio_platform_fence_irq(
		struct mt6797_btif_pio_platform *adapter)
{
	bool disable_irq = false;

	/* Caller already put the PIO owner into its permanent fenced state. */
	mutex_lock(&adapter->state_lock);
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_ACTIVE &&
	    adapter->irq_enabled) {
		adapter->state = MT6797_BTIF_PIO_PLATFORM_FENCED;
		adapter->irq_enabled = false;
		disable_irq = true;
	}
	mutex_unlock(&adapter->state_lock);
	if (disable_irq)
		adapter->irq_ops->disable_nosync(adapter->irq_context, adapter->irq);
}

static irqreturn_t mt6797_btif_pio_platform_irq_thread(int irq, void *context)
{
	struct mt6797_btif_pio_platform *adapter = context;
	const struct mt6797_btif_pio_lease *pio_lease;
	int ret;

	mutex_lock(&adapter->state_lock);
	if (adapter->state != MT6797_BTIF_PIO_PLATFORM_ACTIVE ||
	    !adapter->irq_enabled) {
		mutex_unlock(&adapter->state_lock);
		return IRQ_NONE;
	}
	pio_lease = adapter->pio_lease;
	mutex_unlock(&adapter->state_lock);

	ret = mt6797_btif_pio_irq(pio_lease);
	if (ret == -EAGAIN)
		return IRQ_NONE;
	if (ret == -EOVERFLOW || ret == -EPROTO || ret == -ESHUTDOWN) {
		/*
		 * These PIO paths already clear BTIF IER and invalidate its lease.
		 * Mask the Linux line from its own threaded handler without waiting on
		 * ourselves; normal close/remove uses the synchronous variant.
		 */
		mt6797_btif_pio_platform_fence_irq(adapter);
	}
	if (ret && ret != -ESHUTDOWN)
		dev_warn_ratelimited(adapter->dev, "BTIF PIO IRQ fenced: %d\n", ret);
	return IRQ_HANDLED;
}

static int mt6797_btif_pio_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_btif_pio_platform *adapter;
	struct mt6797_btif_pio_io_ops io_ops;
	struct resource *res;
	int irq_count;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mt6797_btif_pio_platform_resource_valid(res))
		return dev_err_probe(dev, -EINVAL,
				     "BTIF core must be 0x%08x/0x%x\n",
				     MT6797_BTIF_CORE_BASE, MT6797_BTIF_CORE_SIZE);

	adapter = devm_kzalloc(dev, sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return -ENOMEM;
	adapter->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(adapter->base))
		return PTR_ERR(adapter->base);

	irq_count = platform_irq_count(pdev);
	if (irq_count != 1)
		return dev_err_probe(dev, irq_count < 0 ? irq_count : -EINVAL,
				     "BTIF must have exactly one core IRQ, got %d\n",
				     irq_count);
	ret = mt6797_btif_pio_platform_of_irq_valid(dev->of_node);
	if (ret)
		return dev_err_probe(dev, ret,
				     "BTIF must use GIC SPI %u level-low\n",
				     MT6797_BTIF_CORE_SPI);

	adapter->irq = platform_get_irq(pdev, 0);
	if (adapter->irq < 0)
		return adapter->irq;
	if (!mt6797_btif_pio_platform_irq_contract_valid(irq_count,
							 irq_get_trigger_type(adapter->irq)))
		return dev_err_probe(dev, -EINVAL,
				     "BTIF IRQ must resolve as level-low\n");

	adapter->dev = dev;
	adapter->irq_ops = &mt6797_btif_pio_platform_linux_irq_ops;
	adapter->irq_context = adapter;
	mutex_init(&adapter->state_lock);
	init_completion(&adapter->close_complete);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_platform_read32,
		.write32 = mt6797_btif_pio_platform_write32,
		.read8 = mt6797_btif_pio_platform_read8,
		.write8 = mt6797_btif_pio_platform_write8,
		.context = adapter,
	};
	ret = mt6797_btif_pio_platform_prepare(adapter, &io_ops);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, adapter->irq, NULL,
					mt6797_btif_pio_platform_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), adapter);
	if (ret) {
		mt6797_btif_pio_platform_destroy(adapter);
		return ret;
	}

	platform_set_drvdata(pdev, adapter);
	return 0;
}

static void mt6797_btif_pio_platform_remove(struct platform_device *pdev)
{
	struct mt6797_btif_pio_platform *adapter = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	mt6797_btif_pio_platform_destroy(adapter);
}

static const struct of_device_id mt6797_btif_pio_platform_of_match[] = {
	{ .compatible = "mediatek,mt6797-btif-pio" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_btif_pio_platform_of_match);

static struct platform_driver mt6797_btif_pio_platform_driver = {
	.probe = mt6797_btif_pio_platform_probe,
	.remove = mt6797_btif_pio_platform_remove,
	.driver = {
		.name = "mt6797-btif-pio-platform",
		.of_match_table = mt6797_btif_pio_platform_of_match,
	},
};
module_platform_driver(mt6797_btif_pio_platform_driver);

MODULE_DESCRIPTION("Dormant MT6797 BTIF PIO platform acquisition adapter");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_PIO_PLATFORM_KUNIT_TEST)

#define MT6797_BTIF_PIO_PLATFORM_TEST_REGS	(0x70 / sizeof(u32))

struct mt6797_btif_pio_platform_test_bus {
	u32 regs[MT6797_BTIF_PIO_PLATFORM_TEST_REGS];
	u32 iir;
	unsigned int enable_count;
	unsigned int disable_nosync_count;
	unsigned int disable_count;
};

struct mt6797_btif_pio_platform_test_endpoint {
	unsigned int receive_count;
};

struct mt6797_btif_pio_platform_test_context {
	struct mt6797_btif_pio_platform adapter;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease runtime_lease;
	struct mt6797_btif_pio_platform_test_bus *bus;
	struct mt6797_btif_pio_platform_test_endpoint *endpoint;
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_lease *stp_lease;
};

static int mt6797_btif_pio_platform_test_lifecycle_op(void *context)
{
	return 0;
}

static int mt6797_btif_pio_platform_test_lifecycle_poll(
		void *context, enum mt6797_consys_poll poll)
{
	return poll == MT6797_CONSYS_POLL_CHIP_READY ? 0 : -EINVAL;
}

static const struct mt6797_consys_ops
		mt6797_btif_pio_platform_test_lifecycle_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_btif_pio_platform_test_lifecycle_op,
	.disable_vcn18 = mt6797_btif_pio_platform_test_lifecycle_op,
	.enable_vcn28 = mt6797_btif_pio_platform_test_lifecycle_op,
	.disable_vcn28 = mt6797_btif_pio_platform_test_lifecycle_op,
	.configure_sleep_mask = mt6797_btif_pio_platform_test_lifecycle_op,
	.assert_reset = mt6797_btif_pio_platform_test_lifecycle_op,
	.configure_pwron = mt6797_btif_pio_platform_test_lifecycle_op,
	.enable_scp_sys_conn = mt6797_btif_pio_platform_test_lifecycle_op,
	.disable_scp_sys_conn = mt6797_btif_pio_platform_test_lifecycle_op,
	.poll = mt6797_btif_pio_platform_test_lifecycle_poll,
};

static const struct mt6797_consys_timeouts
		mt6797_btif_pio_platform_test_timeouts = {
	.poll_interval_us = 1,
	.chip_ready_timeout_us = 1,
};

static u32 mt6797_btif_pio_platform_test_read32(void *context, u32 offset)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (offset == 0x08)
		return bus->iir;
	return bus->regs[offset / sizeof(u32)];
}

static void mt6797_btif_pio_platform_test_write32(void *context, u32 offset,
						   u32 value)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	bus->regs[offset / sizeof(u32)] = value;
}

static u8 mt6797_btif_pio_platform_test_read8(void *context, u32 offset)
{
	return 0;
}

static void mt6797_btif_pio_platform_test_write8(void *context, u32 offset,
						  u8 value)
{
}

static void mt6797_btif_pio_platform_test_irq_enable(void *context, int irq)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	bus->enable_count++;
}

static void mt6797_btif_pio_platform_test_irq_disable(void *context, int irq)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	bus->disable_count++;
}

static void mt6797_btif_pio_platform_test_irq_disable_nosync(void *context,
						       int irq)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	bus->disable_nosync_count++;
}

static const struct mt6797_btif_pio_platform_irq_ops
		mt6797_btif_pio_platform_test_irq_ops = {
	.enable = mt6797_btif_pio_platform_test_irq_enable,
	.disable_nosync = mt6797_btif_pio_platform_test_irq_disable_nosync,
	.disable_sync = mt6797_btif_pio_platform_test_irq_disable,
};

static int mt6797_btif_pio_platform_test_receive(void *context,
		enum mtk_stp_channel channel, const u8 *payload, size_t payload_length)
{
	struct mt6797_btif_pio_platform_test_endpoint *endpoint = context;

	if (channel != MTK_STP_WMT)
		return -EOPNOTSUPP;
	endpoint->receive_count++;
	return 0;
}

static struct mt6797_btif_pio_platform_test_context *
mt6797_btif_pio_platform_test_start(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_consys_stp_session_ops stp_ops;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->bus = kunit_kzalloc(test, sizeof(*ctx->bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->bus);
	ctx->endpoint = kunit_kzalloc(test, sizeof(*ctx->endpoint), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->endpoint);
	ctx->bus->iir = BIT(0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_lifecycle_init(&ctx->lifecycle,
							    &mt6797_btif_pio_platform_test_lifecycle_ops,
							    NULL,
							    &mt6797_btif_pio_platform_test_timeouts), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&ctx->lifecycle,
									  &ctx->runtime_lease), 0);
	mutex_init(&ctx->adapter.state_lock);
	init_completion(&ctx->adapter.close_complete);
	ctx->adapter.irq = 1;
	ctx->adapter.irq_ops = &mt6797_btif_pio_platform_test_irq_ops;
	ctx->adapter.irq_context = ctx->bus;
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_pio_platform_test_read32,
		.write32 = mt6797_btif_pio_platform_test_write32,
		.read8 = mt6797_btif_pio_platform_test_read8,
		.write8 = mt6797_btif_pio_platform_test_write8,
		.context = ctx->bus,
	};
	ret = mt6797_btif_pio_platform_prepare(&ctx->adapter, &io_ops);
	KUNIT_ASSERT_EQ(test, ret, 0);
	stp_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_btif_pio_transmit,
		.receive = mt6797_btif_pio_platform_test_receive,
		.transmit_context = (void *)ctx->adapter.pio_lease,
		.receive_context = ctx->endpoint,
	};
	ctx->session = mt6797_consys_stp_session_create(&stp_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->session));
	ctx->stp_lease = mt6797_consys_stp_session_begin_full(ctx->session);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->stp_lease));
	return ctx;
}

static void mt6797_btif_pio_platform_test_stop(
		struct mt6797_btif_pio_platform_test_context *ctx)
{
	mt6797_btif_pio_platform_destroy(&ctx->adapter);
	WARN_ON_ONCE(mt6797_consys_runtime_lease_release(&ctx->runtime_lease));
	mt6797_consys_stp_session_quiesce(ctx->session);
	mt6797_consys_stp_session_destroy(ctx->session);
}

static void mt6797_btif_pio_platform_test_resource_bounds(struct kunit *test)
{
	struct resource resource = {
		.start = MT6797_BTIF_CORE_BASE,
		.end = MT6797_BTIF_CORE_BASE + MT6797_BTIF_CORE_SIZE - 1,
		.flags = IORESOURCE_MEM,
	};

	KUNIT_EXPECT_TRUE(test, mt6797_btif_pio_platform_resource_valid(&resource));
	resource.start++;
	KUNIT_EXPECT_FALSE(test,
			 mt6797_btif_pio_platform_resource_valid(&resource));
	resource.start--;
	resource.end++;
	KUNIT_EXPECT_FALSE(test,
			 mt6797_btif_pio_platform_resource_valid(&resource));
	resource.end--;
	resource.flags = 0;
	KUNIT_EXPECT_FALSE(test,
			 mt6797_btif_pio_platform_resource_valid(&resource));
}

static void mt6797_btif_pio_platform_test_irq_contract(struct kunit *test)
{
	struct of_phandle_args spec = {
		.args_count = 3,
		.args = { GIC_SPI, MT6797_BTIF_CORE_SPI, IRQ_TYPE_LEVEL_LOW },
	};

	KUNIT_EXPECT_TRUE(test, mt6797_btif_pio_platform_irq_contract_valid(
							1, IRQ_TYPE_LEVEL_LOW));
	KUNIT_EXPECT_FALSE(test, mt6797_btif_pio_platform_irq_contract_valid(
							2, IRQ_TYPE_LEVEL_LOW));
	KUNIT_EXPECT_FALSE(test, mt6797_btif_pio_platform_irq_contract_valid(
							1, IRQ_TYPE_LEVEL_HIGH));
	KUNIT_EXPECT_TRUE(test, mt6797_btif_pio_platform_irq_spec_valid(&spec));
	spec.args[1]++;
	KUNIT_EXPECT_FALSE(test, mt6797_btif_pio_platform_irq_spec_valid(&spec));
	spec.args[1]--;
	spec.args[2] = IRQ_TYPE_LEVEL_HIGH;
	KUNIT_EXPECT_FALSE(test, mt6797_btif_pio_platform_irq_spec_valid(&spec));
	spec.args[2] = IRQ_TYPE_LEVEL_LOW;
	spec.args_count = 2;
	KUNIT_EXPECT_FALSE(test, mt6797_btif_pio_platform_irq_spec_valid(&spec));
}

static void mt6797_btif_pio_platform_test_explicit_gate(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	bool enabled = READ_ONCE(enable_experimental_platform_adapter);

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	WRITE_ONCE(enable_experimental_platform_adapter, false);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_platform_attach_full_stp(lease,
									&ctx->runtime_lease,
									ctx->stp_lease), -EPERM);
	KUNIT_EXPECT_EQ(test, ctx->bus->enable_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], 0U);
	WRITE_ONCE(enable_experimental_platform_adapter, enabled);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static void mt6797_btif_pio_platform_test_activate_close_once(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	u8 byte = 0x5a;
	bool enabled = READ_ONCE(enable_experimental_platform_adapter);

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	WRITE_ONCE(enable_experimental_platform_adapter, true);
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_platform_attach_full_stp(lease,
									&ctx->runtime_lease,
									ctx->stp_lease), 0);
	KUNIT_EXPECT_EQ(test, ctx->bus->enable_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_platform_attach_full_stp(lease,
									&ctx->runtime_lease,
									ctx->stp_lease), -EBUSY);
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_lease_release(&ctx->runtime_lease), -EBUSY);
	mt6797_btif_pio_platform_close(lease);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_transmit(
				(void *)ctx->adapter.pio_lease, &byte, sizeof(byte)),
				-ESHUTDOWN);
	mt6797_btif_pio_platform_close(lease);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_count, 1U);
	WRITE_ONCE(enable_experimental_platform_adapter, enabled);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static void mt6797_btif_pio_platform_test_irq_fence(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;

	ctx = mt6797_btif_pio_platform_test_start(test);
	ctx->adapter.state = MT6797_BTIF_PIO_PLATFORM_ACTIVE;
	ctx->adapter.irq_enabled = true;
	mt6797_btif_pio_platform_fence_irq(&ctx->adapter);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
			MT6797_BTIF_PIO_PLATFORM_FENCED);
	KUNIT_EXPECT_FALSE(test, ctx->adapter.irq_enabled);
	mt6797_btif_pio_platform_fence_irq(&ctx->adapter);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static struct kunit_case mt6797_btif_pio_platform_test_cases[] = {
	KUNIT_CASE(mt6797_btif_pio_platform_test_resource_bounds),
	KUNIT_CASE(mt6797_btif_pio_platform_test_irq_contract),
	KUNIT_CASE(mt6797_btif_pio_platform_test_explicit_gate),
	KUNIT_CASE(mt6797_btif_pio_platform_test_activate_close_once),
	KUNIT_CASE(mt6797_btif_pio_platform_test_irq_fence),
	{ }
};

static struct kunit_suite mt6797_btif_pio_platform_test_suite = {
	.name = "mt6797-btif-pio-platform",
	.test_cases = mt6797_btif_pio_platform_test_cases,
};
kunit_test_suite(mt6797_btif_pio_platform_test_suite);

#endif /* CONFIG_MTK_MT6797_BTIF_PIO_PLATFORM_KUNIT_TEST */
