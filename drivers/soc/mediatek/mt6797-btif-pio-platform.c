// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 BTIF PIO platform acquisition and IRQ adapter.
 *
 * Vendor evidence deliberately limits this adapter to the BTIF core resource
 * and its required core clock:
 *
 * - MT6797's vendor DT describes BTIF as 0x1100c000/0x1000 with one
 *   level-low GIC SPI 130 interrupt (arch/arm64/boot/dts/mt6797.dtsi:2511-2514);
 * - the vendor requests btifc, enables it before hardware initialization, and
 *   disables it after PIO/IRQ teardown (btif_plat.c:264-292,321-325 and
 *   mtk_btif.c:1455-1482);
 * - the vendor registration path requests the IRQ then leaves it disabled
 *   (drivers/misc/mediatek/btif/common/mtk_btif.c:1472-1481), and its teardown
 *   uses synchronous disable_irq() (mtk_btif.c:972-980).
 *
 * Thus probe obtains, but does not enable, btifc and requests a threaded core
 * IRQ with IRQF_NO_AUTOEN. Lease acquisition enables only that PIO-core clock;
 * it deliberately neither obtains nor votes APDMA. A future owner must still
 * establish CONSYS power/reset/wake, pads, mandatory-STP handover, firmware,
 * and an explicit experiment parameter before PIO register setup or IRQ enable
 * is possible. No Gemini DT node matches this driver in this change.
 */

#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_PIO_PLATFORM_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio.h>
#include <linux/soc/mediatek/mt6797-btif-pio-platform.h>
#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP)
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#endif
#include <linux/soc/mediatek/mt6797-consys.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mtk-stp.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define MT6797_BTIF_CORE_BASE	0x1100c000
#define MT6797_BTIF_CORE_SIZE	0x1000
#define MT6797_BTIF_CORE_SPI	130
#define MT6797_BTIF_LOOPBACK_PATTERN	0xa5
/* Safety bound only; vendor BTIF source supplies no loopback deadline. */
#define MT6797_BTIF_LOOPBACK_TIMEOUT_MS	100

static bool enable_experimental_platform_adapter;
module_param_named(enable_experimental_platform_adapter,
		   enable_experimental_platform_adapter, bool, 0400);
MODULE_PARM_DESC(enable_experimental_platform_adapter,
		 "allow the dormant MT6797 BTIF PIO platform experiment (default: false)");

static bool enable_experimental_pio_qualification;
module_param_named(enable_experimental_pio_qualification,
		   enable_experimental_pio_qualification, bool, 0400);
MODULE_PARM_DESC(enable_experimental_pio_qualification,
		 "allow one MT6797 BTIF clocked PIO register qualification (default: false)");

static bool enable_experimental_pio_loopback_qualification;
module_param_named(enable_experimental_pio_loopback_qualification,
		   enable_experimental_pio_loopback_qualification, bool, 0400);
MODULE_PARM_DESC(enable_experimental_pio_loopback_qualification,
		 "allow one MT6797 BTIF PIO loopback byte/IRQ qualification (default: false)");

enum mt6797_btif_pio_platform_state {
	MT6797_BTIF_PIO_PLATFORM_READY,
	MT6797_BTIF_PIO_PLATFORM_LEASED,
	MT6797_BTIF_PIO_PLATFORM_LOOPBACK_QUALIFYING,
	MT6797_BTIF_PIO_PLATFORM_BOOTSTRAP,
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
	void (*synchronize)(void *context, int irq);
};

struct mt6797_btif_pio_platform_clock_ops {
	int (*prepare_enable)(void *context, struct clk *clock);
	void (*disable_unprepare)(void *context, struct clk *clock);
};

struct mt6797_btif_pio_platform {
	struct device *dev;
	void __iomem *base;
	struct mutex state_lock;
	struct completion close_complete;
	struct completion loopback_complete;
	struct work_struct fatal_close_work;
	struct mt6797_btif_pio *pio;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_btif_pio_platform_lease lease;
	const struct mt6797_btif_pio_platform_irq_ops *irq_ops;
	void *irq_context;
	struct clk *btifc_clk;
	const struct mt6797_btif_pio_platform_clock_ops *clock_ops;
	void *clock_context;
	int irq;
	bool irq_enabled;
	bool btifc_enabled;
	int loopback_status;
	u8 loopback_byte;
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

static void mt6797_btif_pio_platform_irq_synchronize(void *context, int irq)
{
	synchronize_irq(irq);
}

static const struct mt6797_btif_pio_platform_irq_ops
		mt6797_btif_pio_platform_linux_irq_ops = {
	.enable = mt6797_btif_pio_platform_irq_enable,
	.disable_nosync = mt6797_btif_pio_platform_irq_disable_nosync,
	.disable_sync = mt6797_btif_pio_platform_irq_disable_sync,
	.synchronize = mt6797_btif_pio_platform_irq_synchronize,
};

static int mt6797_btif_pio_platform_clock_prepare_enable(void *context, struct clk *clock)
{
	return clk_prepare_enable(clock);
}

static void mt6797_btif_pio_platform_clock_disable_unprepare(void *context,
							     struct clk *clock)
{
	clk_disable_unprepare(clock);
}

static const struct mt6797_btif_pio_platform_clock_ops
		mt6797_btif_pio_platform_linux_clock_ops = {
	.prepare_enable = mt6797_btif_pio_platform_clock_prepare_enable,
	.disable_unprepare = mt6797_btif_pio_platform_clock_disable_unprepare,
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

static void mt6797_btif_pio_platform_fatal_close_work(struct work_struct *work);

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
	init_completion(&adapter->loopback_complete);
	INIT_WORK(&adapter->fatal_close_work,
		  mt6797_btif_pio_platform_fatal_close_work);
	return 0;
}

static void mt6797_btif_pio_platform_destroy(
		struct mt6797_btif_pio_platform *adapter)
{
	if (!adapter)
		return;

	mt6797_btif_pio_platform_close(&adapter->lease);
	cancel_work_sync(&adapter->fatal_close_work);
	mt6797_btif_pio_destroy(adapter->pio);
	adapter->pio = NULL;
	adapter->pio_lease = NULL;
	adapter->lease.adapter = NULL;
}

static const struct mt6797_btif_pio_platform_lease *
mt6797_btif_pio_platform_reserve(struct mt6797_btif_pio_platform *adapter)
{
	const struct mt6797_btif_pio_platform_lease *lease;
	int ret;

	mutex_lock(&adapter->state_lock);
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_READY) {
		/*
		 * Vendor enables btifc before hal_btif_hw_init() (mtk_btif.c:
		 * 1455-1466).  The reserve boundary covers both the mandatory and
		 * full-STP PIO initialization paths without making probe live.
		 */
		if (!adapter->btifc_clk || !adapter->clock_ops) {
			lease = ERR_PTR(-ENODEV);
		} else {
			ret = adapter->clock_ops->prepare_enable(adapter->clock_context,
							 adapter->btifc_clk);
			if (ret) {
				lease = ERR_PTR(ret);
			} else {
				adapter->btifc_enabled = true;
				adapter->state = MT6797_BTIF_PIO_PLATFORM_LEASED;
				lease = &adapter->lease;
			}
		}
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

static int
mt6797_btif_pio_platform_loopback_begin(struct mt6797_btif_pio_platform *adapter)
{
	int ret = 0;

	mutex_lock(&adapter->state_lock);
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_FENCED) {
		ret = -ESHUTDOWN;
	} else if (adapter->state != MT6797_BTIF_PIO_PLATFORM_LEASED) {
		ret = -EBUSY;
	} else {
		reinit_completion(&adapter->loopback_complete);
		adapter->loopback_status = -ETIMEDOUT;
		ret = mt6797_btif_pio_loopback_prepare(adapter->pio_lease,
						       MT6797_BTIF_LOOPBACK_PATTERN);
		if (!ret) {
			/* Queue the byte before unmasking the vendor-disabled IRQ line. */
			adapter->state = MT6797_BTIF_PIO_PLATFORM_LOOPBACK_QUALIFYING;
			adapter->irq_enabled = true;
			adapter->irq_ops->enable(adapter->irq_context, adapter->irq);
		}
	}
	mutex_unlock(&adapter->state_lock);

	return ret;
}

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP)

static int mt6797_btif_pio_platform_bootstrap_enable_irq(void *context)
{
	struct mt6797_btif_pio_platform *adapter = context;
	int ret = 0;

	mutex_lock(&adapter->state_lock);
	if (adapter->state != MT6797_BTIF_PIO_PLATFORM_BOOTSTRAP) {
		ret = -ESHUTDOWN;
	} else {
		/* The mandatory RX owner was installed before this callback. */
		adapter->state = MT6797_BTIF_PIO_PLATFORM_ACTIVE;
		adapter->irq_enabled = true;
		adapter->irq_ops->enable(adapter->irq_context, adapter->irq);
	}
	mutex_unlock(&adapter->state_lock);

	return ret;
}

static void mt6797_btif_pio_platform_bootstrap_close(void *context)
{
	struct mt6797_btif_pio_platform *adapter = context;

	mt6797_btif_pio_platform_close(&adapter->lease);
}

struct mt6797_btif_stp_bootstrap *
mt6797_btif_pio_platform_bootstrap_create(
		const struct mt6797_btif_pio_platform_lease *platform_lease,
		const struct mt6797_consys_runtime_lease *runtime_lease)
{
	struct mt6797_btif_pio_platform *adapter;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;

	if (!platform_lease || !platform_lease->adapter || !runtime_lease)
		return ERR_PTR(-EINVAL);
	if (!READ_ONCE(enable_experimental_platform_adapter))
		return ERR_PTR(-EPERM);
	if (!mt6797_consys_runtime_lease_active(runtime_lease))
		return ERR_PTR(-ESHUTDOWN);

	adapter = platform_lease->adapter;
	mutex_lock(&adapter->state_lock);
	if (platform_lease != &adapter->lease ||
	    adapter->state == MT6797_BTIF_PIO_PLATFORM_FENCED) {
		mutex_unlock(&adapter->state_lock);
		return ERR_PTR(-ESHUTDOWN);
	}
	if (adapter->state != MT6797_BTIF_PIO_PLATFORM_LEASED) {
		mutex_unlock(&adapter->state_lock);
		return ERR_PTR(-EBUSY);
	}
	/* Prevent the direct full-session path from racing this one owner. */
	adapter->state = MT6797_BTIF_PIO_PLATFORM_BOOTSTRAP;
	mutex_unlock(&adapter->state_lock);

	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_pio_platform_bootstrap_enable_irq,
		.close = mt6797_btif_pio_platform_bootstrap_close,
		.context = adapter,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(adapter->pio_lease,
						     runtime_lease, &ops);
	if (!IS_ERR(bootstrap))
		return bootstrap;

	/* Allocation failed before bootstrap could own any PIO/IRQ transition. */
	mutex_lock(&adapter->state_lock);
	if (adapter->state == MT6797_BTIF_PIO_PLATFORM_BOOTSTRAP)
		adapter->state = MT6797_BTIF_PIO_PLATFORM_LEASED;
	mutex_unlock(&adapter->state_lock);
	return bootstrap;
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_platform_bootstrap_create);

#endif /* CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP */

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

static void
mt6797_btif_pio_platform_finish_close(struct mt6797_btif_pio_platform *adapter)
{
	bool disable_clock;

	/* The PIO owner clears IER/fences before the only clock vote is dropped. */
	mt6797_btif_pio_close(adapter->pio);

	mutex_lock(&adapter->state_lock);
	disable_clock = adapter->btifc_enabled;
	adapter->btifc_enabled = false;
	mutex_unlock(&adapter->state_lock);
	if (disable_clock)
		adapter->clock_ops->disable_unprepare(adapter->clock_context,
						     adapter->btifc_clk);

	mutex_lock(&adapter->state_lock);
	adapter->state = MT6797_BTIF_PIO_PLATFORM_FENCED;
	complete_all(&adapter->close_complete);
	mutex_unlock(&adapter->state_lock);
}

static void mt6797_btif_pio_platform_fatal_close_work(struct work_struct *work)
{
	struct mt6797_btif_pio_platform *adapter = container_of(work,
			struct mt6797_btif_pio_platform, fatal_close_work);

	/* IRQF_NO_AUTOEN was masked in the IRQ thread; wait for it to return. */
	adapter->irq_ops->synchronize(adapter->irq_context, adapter->irq);
	mt6797_btif_pio_platform_finish_close(adapter);
}

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
	mt6797_btif_pio_platform_finish_close(adapter);
}
EXPORT_SYMBOL_GPL(mt6797_btif_pio_platform_close);

static void mt6797_btif_pio_platform_fence_irq(
		struct mt6797_btif_pio_platform *adapter)
{
	bool disable_irq = false;

	/* Caller already put the PIO owner into its permanent fenced state. */
	mutex_lock(&adapter->state_lock);
	if ((adapter->state == MT6797_BTIF_PIO_PLATFORM_ACTIVE ||
	     adapter->state == MT6797_BTIF_PIO_PLATFORM_LOOPBACK_QUALIFYING) &&
	    adapter->irq_enabled) {
		adapter->state = MT6797_BTIF_PIO_PLATFORM_QUIESCING;
		adapter->irq_enabled = false;
		reinit_completion(&adapter->close_complete);
		disable_irq = true;
	}
	mutex_unlock(&adapter->state_lock);
	if (disable_irq) {
		adapter->irq_ops->disable_nosync(adapter->irq_context, adapter->irq);
		schedule_work(&adapter->fatal_close_work);
	}
}

static irqreturn_t mt6797_btif_pio_platform_irq_thread(int irq, void *context)
{
	struct mt6797_btif_pio_platform *adapter = context;
	const struct mt6797_btif_pio_lease *pio_lease;
	bool loopback;
	u8 byte;
	int ret;

	mutex_lock(&adapter->state_lock);
	loopback = adapter->state == MT6797_BTIF_PIO_PLATFORM_LOOPBACK_QUALIFYING;
	if ((!loopback && adapter->state != MT6797_BTIF_PIO_PLATFORM_ACTIVE) ||
	    !adapter->irq_enabled) {
		mutex_unlock(&adapter->state_lock);
		return IRQ_NONE;
	}
	pio_lease = adapter->pio_lease;
	mutex_unlock(&adapter->state_lock);

	if (loopback) {
		ret = mt6797_btif_pio_loopback_irq(pio_lease, &byte);
		if (ret == -EAGAIN)
			return IRQ_NONE;
		if (!ret)
			adapter->loopback_byte = byte;
		WRITE_ONCE(adapter->loopback_status, ret);
		complete(&adapter->loopback_complete);
		/* PIO is already fenced on every non-spurious qualification result. */
		mt6797_btif_pio_platform_fence_irq(adapter);
		if (ret)
			dev_warn_ratelimited(adapter->dev,
					     "BTIF loopback qualification fenced: %d\n", ret);
		return IRQ_HANDLED;
	}

	ret = mt6797_btif_pio_irq(pio_lease);
	if (ret == -EAGAIN)
		return IRQ_NONE;
	if (ret) {
		/*
		 * Every non-spurious mt6797_btif_pio_irq() error follows a PIO
		 * fence: callback errors clear BTIF IER before being returned, as do
		 * the transport's own protocol errors. Keep the Linux IRQ and clock
		 * lifecycle in lockstep with that fenced owner.
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
	bool pio_qualification;
	bool loopback_qualification;
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
	adapter->btifc_clk = devm_clk_get(dev, "btifc");
	if (IS_ERR(adapter->btifc_clk))
		return dev_err_probe(dev, PTR_ERR(adapter->btifc_clk),
				     "BTIF must name its core clock btifc\n");

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
	adapter->clock_ops = &mt6797_btif_pio_platform_linux_clock_ops;
	adapter->clock_context = adapter;
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
	pio_qualification =
		device_property_read_bool(dev, "mediatek,experimental-pio-qualification");
	loopback_qualification =
		device_property_read_bool(dev,
					  "mediatek,experimental-pio-loopback-qualification");
	if (pio_qualification && loopback_qualification) {
		ret = dev_err_probe(dev, -EINVAL,
				    "choose at most one BTIF PIO qualification\n");
		goto qualification_destroy;
	}
	if (pio_qualification) {
		const struct mt6797_btif_pio_platform_lease *lease;
		u32 tri_level;

		if (!READ_ONCE(enable_experimental_pio_qualification)) {
			ret = dev_err_probe(dev, -EPERM,
					    "PIO qualification requires explicit parameter opt-in\n");
			goto qualification_destroy;
		}

		lease = mt6797_btif_pio_platform_reserve(adapter);
		if (IS_ERR(lease)) {
			ret = PTR_ERR(lease);
			goto qualification_destroy;
		}
		ret = mt6797_btif_pio_qualify(adapter->pio_lease, &tri_level);
		mt6797_btif_pio_platform_close(lease);
		if (ret)
			goto qualification_destroy;

		dev_info(dev,
			 "BTIF PIO qualification passed; TRI_LVL=0x%08x, loopback disabled\n",
			 tri_level);
	}
	if (loopback_qualification) {
		const struct mt6797_btif_pio_platform_lease *lease;
		unsigned long complete;
		unsigned long timeout;

		if (!READ_ONCE(enable_experimental_pio_loopback_qualification)) {
			ret = dev_err_probe(dev, -EPERM,
					    "PIO loopback qualification requires explicit parameter opt-in\n");
			goto qualification_destroy;
		}

		lease = mt6797_btif_pio_platform_reserve(adapter);
		if (IS_ERR(lease)) {
			ret = PTR_ERR(lease);
			goto qualification_destroy;
		}
		ret = mt6797_btif_pio_platform_loopback_begin(adapter);
		if (!ret) {
			timeout = msecs_to_jiffies(MT6797_BTIF_LOOPBACK_TIMEOUT_MS);
			complete = wait_for_completion_timeout(&adapter->loopback_complete,
							       timeout);
			ret = complete ? READ_ONCE(adapter->loopback_status) : -ETIMEDOUT;
			if (!ret && READ_ONCE(adapter->loopback_byte) !=
			    MT6797_BTIF_LOOPBACK_PATTERN)
				ret = -EBADMSG;
		}
		mt6797_btif_pio_platform_close(lease);
		if (ret)
			goto qualification_destroy;

		dev_info(dev,
			 "BTIF PIO loopback passed; byte=0x%02x via RX IRQ, PIO fenced\n",
			 MT6797_BTIF_LOOPBACK_PATTERN);
	}
	return 0;

qualification_destroy:
	platform_set_drvdata(pdev, NULL);
	mt6797_btif_pio_platform_destroy(adapter);
	return ret;
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
	u8 loopback_byte;
	unsigned int write8_count;
	bool loopback_pending;
	unsigned int enable_count;
	unsigned int disable_nosync_count;
	unsigned int disable_count;
	unsigned int synchronize_count;
	unsigned int clock_enable_count;
	unsigned int clock_disable_count;
	int clock_enable_error;
	bool btifc_enabled;
	bool io_while_btifc_disabled;
	bool clock_disabled_before_irq_quiesce;
	bool clock_disabled_before_pio_fence;
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

	if (!bus->btifc_enabled)
		bus->io_while_btifc_disabled = true;
	if (offset == 0x08)
		if (bus->loopback_pending)
			return BIT(2);
	if (offset == 0x08)
		return bus->iir;
	return bus->regs[offset / sizeof(u32)];
}

static void mt6797_btif_pio_platform_test_write32(void *context, u32 offset,
						   u32 value)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (!bus->btifc_enabled)
		bus->io_while_btifc_disabled = true;
	bus->regs[offset / sizeof(u32)] = value;
}

static u8 mt6797_btif_pio_platform_test_read8(void *context, u32 offset)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (!bus->btifc_enabled)
		bus->io_while_btifc_disabled = true;
	if (offset != 0 || !bus->loopback_pending)
		return 0;
	bus->loopback_pending = false;
	return bus->loopback_byte;
}

static void mt6797_btif_pio_platform_test_write8(void *context, u32 offset,
						  u8 value)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (!bus->btifc_enabled)
		bus->io_while_btifc_disabled = true;
	if (offset != 0)
		return;
	bus->write8_count++;
	if (bus->regs[0x60 / sizeof(u32)] & BIT(7)) {
		bus->loopback_byte = value;
		bus->loopback_pending = true;
	}
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

static void mt6797_btif_pio_platform_test_irq_synchronize(void *context, int irq)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	bus->synchronize_count++;
}

static const struct mt6797_btif_pio_platform_irq_ops
		mt6797_btif_pio_platform_test_irq_ops = {
	.enable = mt6797_btif_pio_platform_test_irq_enable,
	.disable_nosync = mt6797_btif_pio_platform_test_irq_disable_nosync,
	.disable_sync = mt6797_btif_pio_platform_test_irq_disable,
	.synchronize = mt6797_btif_pio_platform_test_irq_synchronize,
};

static int
mt6797_btif_pio_platform_test_clock_enable(void *context, struct clk *clock)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (bus->clock_enable_error)
		return bus->clock_enable_error;
	bus->clock_enable_count++;
	bus->btifc_enabled = true;
	return 0;
}

static void
mt6797_btif_pio_platform_test_clock_disable(void *context, struct clk *clock)
{
	struct mt6797_btif_pio_platform_test_bus *bus = context;

	if (bus->enable_count && !bus->disable_count && !bus->synchronize_count)
		bus->clock_disabled_before_irq_quiesce = true;
	if (bus->regs[0x04 / sizeof(u32)])
		bus->clock_disabled_before_pio_fence = true;
	bus->clock_disable_count++;
	bus->btifc_enabled = false;
}

static const struct mt6797_btif_pio_platform_clock_ops
		mt6797_btif_pio_platform_test_clock_ops = {
	.prepare_enable = mt6797_btif_pio_platform_test_clock_enable,
	.disable_unprepare = mt6797_btif_pio_platform_test_clock_disable,
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
	ctx->adapter.btifc_clk = (struct clk *)ctx->bus;
	ctx->adapter.clock_ops = &mt6797_btif_pio_platform_test_clock_ops;
	ctx->adapter.clock_context = ctx->bus;
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
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_enable_count, 1U);
	WRITE_ONCE(enable_experimental_platform_adapter, false);
	KUNIT_EXPECT_EQ(test, mt6797_btif_pio_platform_attach_full_stp(lease,
									&ctx->runtime_lease,
									ctx->stp_lease), -EPERM);
	KUNIT_EXPECT_EQ(test, ctx->bus->enable_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], 0U);
	WRITE_ONCE(enable_experimental_platform_adapter, enabled);
	mt6797_btif_pio_platform_test_stop(ctx);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->io_while_btifc_disabled);
}

static void mt6797_btif_pio_platform_test_clock_enable_failure(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;

	ctx = mt6797_btif_pio_platform_test_start(test);
	ctx->bus->clock_enable_error = -ETIMEDOUT;
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_EXPECT_EQ(test, PTR_ERR(lease), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
			MT6797_BTIF_PIO_PLATFORM_READY);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_enable_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 0U);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static void mt6797_btif_pio_platform_test_qualification_releases_clock(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	u32 tri_level;
	int ret;

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	ret = mt6797_btif_pio_qualify(ctx->adapter.pio_lease, &tri_level);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, tri_level, (u32)0x18);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], BIT(0));
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x60 / sizeof(u32)], (u32)0x18);

	mt6797_btif_pio_platform_close(lease);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_pio_fence);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static void mt6797_btif_pio_platform_test_loopback_byte_irq_fences(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	int irq_result;

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_platform_loopback_begin(&ctx->adapter), 0);
	KUNIT_EXPECT_EQ(test, ctx->bus->write8_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->enable_count, 1U);
	irq_result = mt6797_btif_pio_platform_irq_thread(ctx->adapter.irq, &ctx->adapter);
	KUNIT_ASSERT_EQ(test, irq_result, IRQ_HANDLED);
	KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&ctx->adapter.loopback_complete));
	KUNIT_EXPECT_EQ(test, ctx->adapter.loopback_status, 0);
	KUNIT_EXPECT_EQ(test, ctx->adapter.loopback_byte,
			MT6797_BTIF_LOOPBACK_PATTERN);
	flush_work(&ctx->adapter.fatal_close_work);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->synchronize_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x60 / sizeof(u32)] & BIT(7), 0U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_irq_quiesce);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_pio_fence);
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
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_enable_count, 1U);
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
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->btifc_enabled);
	KUNIT_EXPECT_FALSE(test, ctx->bus->io_while_btifc_disabled);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_irq_quiesce);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_pio_fence);
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
	const struct mt6797_btif_pio_platform_lease *lease;

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	ctx->adapter.state = MT6797_BTIF_PIO_PLATFORM_ACTIVE;
	ctx->adapter.irq_enabled = true;
	ctx->bus->enable_count = 1;
	mt6797_btif_pio_platform_fence_irq(&ctx->adapter);
	flush_work(&ctx->adapter.fatal_close_work);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->synchronize_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
			MT6797_BTIF_PIO_PLATFORM_FENCED);
	KUNIT_EXPECT_FALSE(test, ctx->adapter.irq_enabled);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_irq_quiesce);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_pio_fence);
	mt6797_btif_pio_platform_fence_irq(&ctx->adapter);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	mt6797_btif_pio_platform_test_stop(ctx);
}

static void mt6797_btif_pio_platform_test_irq_badmsg_fences(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	bool enabled = READ_ONCE(enable_experimental_platform_adapter);
	int irq_result;

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	WRITE_ONCE(enable_experimental_platform_adapter, true);
	KUNIT_ASSERT_EQ(test, mt6797_btif_pio_platform_attach_full_stp(lease,
								&ctx->runtime_lease,
								ctx->stp_lease), 0);
	/* The test RBR returns zero, which is not full-STP sync. */
	ctx->bus->iir = BIT(1);
	irq_result = mt6797_btif_pio_platform_irq_thread(ctx->adapter.irq,
								 &ctx->adapter);
	KUNIT_ASSERT_EQ(test, irq_result, IRQ_HANDLED);
	flush_work(&ctx->adapter.fatal_close_work);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_nosync_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->bus->synchronize_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
				MT6797_BTIF_PIO_PLATFORM_FENCED);
	KUNIT_EXPECT_FALSE(test, ctx->adapter.irq_enabled);
	KUNIT_EXPECT_EQ(test, ctx->bus->clock_disable_count, 1U);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_irq_quiesce);
	KUNIT_EXPECT_FALSE(test, ctx->bus->clock_disabled_before_pio_fence);
	WRITE_ONCE(enable_experimental_platform_adapter, enabled);
	mt6797_btif_pio_platform_test_stop(ctx);
}

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP)

static void mt6797_btif_pio_platform_test_bootstrap_factory(struct kunit *test)
{
	struct mt6797_btif_pio_platform_test_context *ctx;
	const struct mt6797_btif_pio_platform_lease *lease;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	bool enabled = READ_ONCE(enable_experimental_platform_adapter);

	ctx = mt6797_btif_pio_platform_test_start(test);
	lease = mt6797_btif_pio_platform_reserve(&ctx->adapter);
	KUNIT_ASSERT_FALSE(test, IS_ERR(lease));
	WRITE_ONCE(enable_experimental_platform_adapter, false);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mt6797_btif_pio_platform_bootstrap_create(
							lease,
							&ctx->runtime_lease)), -EPERM);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
				MT6797_BTIF_PIO_PLATFORM_LEASED);

	WRITE_ONCE(enable_experimental_platform_adapter, true);
	bootstrap = mt6797_btif_pio_platform_bootstrap_create(lease,
								      &ctx->runtime_lease);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
				MT6797_BTIF_PIO_PLATFORM_BOOTSTRAP);
	KUNIT_EXPECT_EQ(test, ctx->bus->enable_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->bus->regs[0x04 / sizeof(u32)], 0U);

	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	/* A never-run bootstrap has not invoked its close callback. */
	mt6797_btif_pio_platform_close(lease);
	KUNIT_EXPECT_EQ(test, ctx->adapter.state,
				MT6797_BTIF_PIO_PLATFORM_FENCED);
	KUNIT_EXPECT_EQ(test, ctx->bus->disable_count, 0U);
	WRITE_ONCE(enable_experimental_platform_adapter, enabled);
	mt6797_btif_pio_platform_test_stop(ctx);
}

#endif /* CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP */

static struct kunit_case mt6797_btif_pio_platform_test_cases[] = {
	KUNIT_CASE(mt6797_btif_pio_platform_test_irq_badmsg_fences),
	KUNIT_CASE(mt6797_btif_pio_platform_test_resource_bounds),
	KUNIT_CASE(mt6797_btif_pio_platform_test_irq_contract),
	KUNIT_CASE(mt6797_btif_pio_platform_test_explicit_gate),
	KUNIT_CASE(mt6797_btif_pio_platform_test_clock_enable_failure),
	KUNIT_CASE(mt6797_btif_pio_platform_test_qualification_releases_clock),
	KUNIT_CASE(mt6797_btif_pio_platform_test_loopback_byte_irq_fences),
	KUNIT_CASE(mt6797_btif_pio_platform_test_activate_close_once),
	KUNIT_CASE(mt6797_btif_pio_platform_test_irq_fence),
#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP)
	KUNIT_CASE(mt6797_btif_pio_platform_test_bootstrap_factory),
#endif
	{ }
};

static struct kunit_suite mt6797_btif_pio_platform_test_suite = {
	.name = "mt6797-btif-pio-platform",
	.test_cases = mt6797_btif_pio_platform_test_cases,
};
kunit_test_suite(mt6797_btif_pio_platform_test_suite);

#endif /* CONFIG_MTK_MT6797_BTIF_PIO_PLATFORM_KUNIT_TEST */
