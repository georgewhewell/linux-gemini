// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explicitly gated MT6797 CONSYS runtime lifecycle provider.
 *
 * The vendor source is the boundary of this deliberately small experiment:
 *
 * - mtk_wcn_consys_hw.c:301-326 enables VCN18, waits 240 us, then enables
 *   VCN28 when co_clock_type is zero (Gemini WMT_SOC.cfg selects zero);
 * - :328-354 sets the Conn-to-AP sleep mask, asserts CONMCU reset, writes
 *   PWRON_CONFG_EN, and votes the vendor SCP_SYS_CONN CCF path;
 * - :401-420 waits 30 us then polls MCU+0x8 for 0x0279;
 * - its DT node has MCU/AP-RGU/TOPCKGEN/SPM in that order and level-low BGF
 *   SPI 284 plus WDT SPI 285 (arch/arm64/boot/dts/mt6797.dtsi:3759-3773);
 * - :757-795 registers BGF then leaves it disabled. WDT recovery semantics
 *   are not established for this driver and its IRQ remains unrequested.
 *
 * The lifecycle core purposefully ends at identity observation. This provider
 * consequently never deasserts reset, touches AFE/shared EMI, starts BTIF,
 * loads firmware, sends WMT, or turns on a connectivity function. The shipped
 * Gemini DT has no compatible node for this driver and stays disabled.
 */

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/mt6351-regulator.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/wait.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_RUNTIME_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-consys-runtime.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define MT6797_CONSYS_RUNTIME_MCU_CHIP_ID		0x8
#define MT6797_CONSYS_RUNTIME_INFRACFG_SLEEP_MASK	0x350
#define MT6797_CONSYS_RUNTIME_SLEEP_MASK_CONN		BIT(8)
#define MT6797_CONSYS_RUNTIME_SPM_PWRON_CONFIG_EN	0x0
#define MT6797_CONSYS_RUNTIME_SPM_PWRON_CONFIG_VALUE	0x0b160001
#define MT6797_CONSYS_RUNTIME_SPM_PWR_STATUS		0x180
#define MT6797_CONSYS_RUNTIME_SPM_PWR_STATUS_2ND	0x184
#define MT6797_CONSYS_RUNTIME_CHIP_ID			0x0279
#define MT6797_CONSYS_RUNTIME_BGF_SPI			284
#define MT6797_CONSYS_RUNTIME_WDT_SPI			285

#define MT6797_CONSYS_RUNTIME_CHIP_POLL_INTERVAL_US	20000
#define MT6797_CONSYS_RUNTIME_CHIP_POLL_TIMEOUT_US	200000

static bool enable_experimental_runtime;
module_param_named(enable_experimental_runtime, enable_experimental_runtime,
			   bool, 0400);
MODULE_PARM_DESC(enable_experimental_runtime,
		 "allow the MT6797 CONSYS runtime lifecycle experiment (default: false)");

struct mt6797_consys_runtime_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

struct mt6797_consys_runtime {
	struct device *dev;
	void __iomem *mcu;
	struct regmap *infracfg;
	struct regmap *spm;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct reset_control *conn_mcu_reset;
	struct resource shared_mem;
	struct of_phandle_args conn_domain;
	struct mt6797_consys_lifecycle lifecycle;
	struct mt6797_consys_runtime_lease lease;
	int bgf_irq;
	int wdt_irq;
	u32 chip_id;
	bool vcn18_vote;
	bool vcn28_vote;
	bool conn_domain_attached;
	bool conn_domain_vote;
	bool runtime_pm_enabled;
	bool lease_held;
	struct mutex provider_lock;
	wait_queue_head_t provider_wait;
	unsigned int external_users;
	enum mt6797_consys_runtime_provider_state provider_state;
};

/* The vendor DT order is MCU, AP-RGU, TOPCKGEN, then SPM. */
static const struct mt6797_consys_runtime_resource
		mt6797_consys_runtime_resources[] = {
	{ "mcu", 0x18070000, SZ_512 },
	{ "ap-rgu", 0x10007000, SZ_256 },
	{ "topckgen", 0x10000000, SZ_8K },
	{ "spm", 0x10006000, SZ_4K },
};

static bool mt6797_consys_runtime_resource_valid(
		const struct resource *resource,
		const struct mt6797_consys_runtime_resource *expected)
{
	return resource && expected && (resource->flags & IORESOURCE_MEM) &&
		resource->start == expected->start &&
		resource_size(resource) == expected->size;
}

static int mt6797_consys_runtime_map_resource(
		struct platform_device *pdev,
		const struct mt6797_consys_runtime_resource *expected,
		void __iomem **base)
{
	const struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						       expected->name);
	if (!mt6797_consys_runtime_resource_valid(resource, expected))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid %s resource\n", expected->name);

	if (!base)
		return 0;

	*base = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(*base))
		return PTR_ERR(*base);

	return 0;
}

/* Vendor power-on rejects a missing CONSYS shared EMI reservation. */
static int mt6797_consys_runtime_acquire_shared_mem(
		struct mt6797_consys_runtime *runtime)
{
	struct device_node *memory_region;
	int ret;

	memory_region = of_parse_phandle(runtime->dev->of_node, "memory-region", 0);
	if (!memory_region)
		return dev_err_probe(runtime->dev, -EINVAL,
				     "missing CONSYS shared memory\n");
	if (!of_property_read_bool(memory_region, "no-map")) {
		of_node_put(memory_region);
		return dev_err_probe(runtime->dev, -EINVAL,
				     "CONSYS shared memory must be no-map\n");
	}
	of_node_put(memory_region);

	ret = of_reserved_mem_region_to_resource_byname(runtime->dev->of_node,
							"shared", &runtime->shared_mem);
	if (ret)
		return dev_err_probe(runtime->dev, ret,
				     "failed to acquire CONSYS shared memory\n");
	if (!mt6797_consys_shared_mem_valid(runtime->shared_mem.start,
					      resource_size(&runtime->shared_mem)))
		return dev_err_probe(runtime->dev, -EINVAL,
				     "invalid CONSYS shared-memory geometry\n");

	return 0;
}

static int mt6797_consys_runtime_enable_rail(struct regulator *regulator,
					      int microvolts, bool hw_control)
{
	int ret;

	ret = mt6351_regulator_set_hw_control(regulator, hw_control);
	if (ret)
		return ret;
	ret = regulator_set_voltage(regulator, microvolts, microvolts);
	if (ret)
		goto restore_software_control;
	ret = regulator_enable(regulator);
	if (!ret)
		return 0;

restore_software_control:
	/* A failed enable must not leave the rail under unexpected HW control. */
	mt6351_regulator_set_hw_control(regulator, false);
	return ret;
}

static int mt6797_consys_runtime_disable_rail(struct regulator *regulator,
					       bool *vote)
{
	int disable_ret;
	int ret;

	/* Vendor power-off selects software control before dropping its vote. */
	ret = mt6351_regulator_set_hw_control(regulator, false);
	if (!*vote)
		return ret;

	disable_ret = regulator_disable(regulator);
	if (!disable_ret)
		*vote = false;
	if (!ret)
		ret = disable_ret;
	return ret;
}

static int mt6797_consys_runtime_enable_vcn18(void *context)
{
	struct mt6797_consys_runtime *runtime = context;
	int ret;

	ret = mt6797_consys_runtime_enable_rail(runtime->vcn18, 1800000, false);
	if (!ret)
		runtime->vcn18_vote = true;
	return ret;
}

static int mt6797_consys_runtime_disable_vcn18(void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	return mt6797_consys_runtime_disable_rail(runtime->vcn18,
						  &runtime->vcn18_vote);
}

static int mt6797_consys_runtime_enable_vcn28(void *context)
{
	struct mt6797_consys_runtime *runtime = context;
	int ret;

	/* Vendor co-clock_type == 0 selects MT6351 VCN28 hardware control. */
	ret = mt6797_consys_runtime_enable_rail(runtime->vcn28, 2800000, true);
	if (!ret)
		runtime->vcn28_vote = true;
	return ret;
}

static int mt6797_consys_runtime_disable_vcn28(void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	return mt6797_consys_runtime_disable_rail(runtime->vcn28,
						  &runtime->vcn28_vote);
}

static int mt6797_consys_runtime_configure_sleep_mask(void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	/* Vendor TOPCKGEN 0x10000000 + 0x1350 is INFRACFG offset 0x350. */
	return regmap_update_bits(runtime->infracfg,
				  MT6797_CONSYS_RUNTIME_INFRACFG_SLEEP_MASK,
				  MT6797_CONSYS_RUNTIME_SLEEP_MASK_CONN,
				  MT6797_CONSYS_RUNTIME_SLEEP_MASK_CONN);
}

static int mt6797_consys_runtime_assert_reset(void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	return reset_control_assert(runtime->conn_mcu_reset);
}

static int mt6797_consys_runtime_configure_pwron(void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	return regmap_write(runtime->spm,
			    MT6797_CONSYS_RUNTIME_SPM_PWRON_CONFIG_EN,
			    MT6797_CONSYS_RUNTIME_SPM_PWRON_CONFIG_VALUE);
}

static int mt6797_consys_runtime_enable_scp_sys_conn(void *context)
{
	struct mt6797_consys_runtime *runtime = context;
	int ret;

	ret = of_genpd_add_device(&runtime->conn_domain, runtime->dev);
	if (ret)
		return ret;
	runtime->conn_domain_attached = true;

	pm_runtime_enable(runtime->dev);
	runtime->runtime_pm_enabled = true;
	ret = pm_runtime_resume_and_get(runtime->dev);
	if (!ret)
		runtime->conn_domain_vote = true;
	return ret;
}

static int mt6797_consys_runtime_disable_scp_sys_conn(void *context)
{
	struct mt6797_consys_runtime *runtime = context;
	int ret;

	if (runtime->conn_domain_vote) {
		ret = pm_runtime_put_sync_suspend(runtime->dev);
		if (ret < 0) {
			/* The failed put consumed a usage count; keep the vote explicit. */
			pm_runtime_get_noresume(runtime->dev);
			return ret;
		}
		runtime->conn_domain_vote = false;
	}

	if (runtime->runtime_pm_enabled) {
		pm_runtime_disable(runtime->dev);
		runtime->runtime_pm_enabled = false;
	}
	if (!runtime->conn_domain_attached)
		return 0;

	ret = pm_genpd_remove_device(runtime->dev);
	if (!ret)
		runtime->conn_domain_attached = false;
	return ret;
}

static void mt6797_consys_runtime_release_domain(
		struct mt6797_consys_runtime *runtime)
{
	int ret;

	if (!runtime->conn_domain_vote && !runtime->runtime_pm_enabled &&
	    !runtime->conn_domain_attached)
		return;

	ret = mt6797_consys_runtime_disable_scp_sys_conn(runtime);
	if (ret)
		dev_err(runtime->dev,
			"failed to release residual CONN domain vote: %d\n", ret);
}

static int mt6797_consys_runtime_poll(void *context,
				      enum mt6797_consys_poll poll)
{
	struct mt6797_consys_runtime *runtime = context;

	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;
	runtime->chip_id = readl(runtime->mcu + MT6797_CONSYS_RUNTIME_MCU_CHIP_ID);
	return runtime->chip_id == MT6797_CONSYS_RUNTIME_CHIP_ID ? 0 : -EAGAIN;
}

static void mt6797_consys_runtime_delay_us(void *context, u32 min, u32 max)
{
	usleep_range(min, max);
}

static const struct mt6797_consys_ops mt6797_consys_runtime_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_runtime_enable_vcn18,
	.disable_vcn18 = mt6797_consys_runtime_disable_vcn18,
	.enable_vcn28 = mt6797_consys_runtime_enable_vcn28,
	.disable_vcn28 = mt6797_consys_runtime_disable_vcn28,
	.configure_sleep_mask = mt6797_consys_runtime_configure_sleep_mask,
	.assert_reset = mt6797_consys_runtime_assert_reset,
	.configure_pwron = mt6797_consys_runtime_configure_pwron,
	.enable_scp_sys_conn = mt6797_consys_runtime_enable_scp_sys_conn,
	.disable_scp_sys_conn = mt6797_consys_runtime_disable_scp_sys_conn,
	.poll = mt6797_consys_runtime_poll,
	.delay_us = mt6797_consys_runtime_delay_us,
};

static bool mt6797_consys_runtime_irq_spec_valid(
		const struct of_phandle_args *spec, unsigned int spi)
{
	return spec && spec->args_count == 3 && spec->args[0] == GIC_SPI &&
		spec->args[1] == spi && spec->args[2] == IRQ_TYPE_LEVEL_LOW;
}

static int mt6797_consys_runtime_validate_of_irq(struct device_node *node,
						  int index, unsigned int spi)
{
	struct of_phandle_args spec;
	int ret;

	ret = of_irq_parse_one(node, index, &spec);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_irq_spec_valid(&spec, spi) ? 0 : -EINVAL;
	of_node_put(spec.np);
	return ret;
}

static int mt6797_consys_runtime_validate_irqs(
		struct platform_device *pdev, struct mt6797_consys_runtime *runtime)
{
	int irq_count;
	int ret;

	irq_count = platform_irq_count(pdev);
	if (irq_count != 2)
		return dev_err_probe(&pdev->dev, irq_count < 0 ? irq_count : -EINVAL,
				     "CONSYS must have exactly BGF and WDT IRQs, got %d\n",
				     irq_count);
	ret = mt6797_consys_runtime_validate_of_irq(pdev->dev.of_node, 0,
						    MT6797_CONSYS_RUNTIME_BGF_SPI);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "BGF must use GIC SPI %u level-low\n",
				     MT6797_CONSYS_RUNTIME_BGF_SPI);
	ret = mt6797_consys_runtime_validate_of_irq(pdev->dev.of_node, 1,
						    MT6797_CONSYS_RUNTIME_WDT_SPI);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "WDT must use GIC SPI %u level-low\n",
				     MT6797_CONSYS_RUNTIME_WDT_SPI);

	runtime->bgf_irq = platform_get_irq_byname(pdev, "bgf");
	if (runtime->bgf_irq < 0)
		return runtime->bgf_irq;
	runtime->wdt_irq = platform_get_irq_byname(pdev, "wdt");
	if (runtime->wdt_irq < 0)
		return runtime->wdt_irq;
	if ((irq_get_trigger_type(runtime->bgf_irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW ||
	    (irq_get_trigger_type(runtime->wdt_irq) & IRQ_TYPE_SENSE_MASK) !=
	    IRQ_TYPE_LEVEL_LOW)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "CONSYS IRQs must resolve as level-low\n");

	return 0;
}

static irqreturn_t mt6797_consys_runtime_bgf_thread(int irq, void *context)
{
	struct mt6797_consys_runtime *runtime = context;

	/* This line must never be enabled before a separately reviewed BGF owner. */
	disable_irq_nosync(irq);
	dev_err_ratelimited(runtime->dev,
			    "unexpected BGF IRQ; line fenced without recovery\n");
	return IRQ_HANDLED;
}

static void mt6797_consys_runtime_log_state(
		struct mt6797_consys_runtime *runtime, const char *stage, int result)
{
	unsigned int pwron = U32_MAX;
	unsigned int pwr_status = U32_MAX;
	unsigned int pwr_status_2nd = U32_MAX;
	int reset;
	int pwron_ret;
	int status_ret;
	int status_2nd_ret;

	reset = reset_control_status(runtime->conn_mcu_reset);
	pwron_ret = regmap_read(runtime->spm,
				MT6797_CONSYS_RUNTIME_SPM_PWRON_CONFIG_EN, &pwron);
	status_ret = regmap_read(runtime->spm,
				 MT6797_CONSYS_RUNTIME_SPM_PWR_STATUS, &pwr_status);
	status_2nd_ret = regmap_read(runtime->spm,
				     MT6797_CONSYS_RUNTIME_SPM_PWR_STATUS_2ND,
				     &pwr_status_2nd);
	if (result) {
		dev_err(runtime->dev,
			"%s refused: %d chip=0x%04x reset=%d pwron=0x%08x(%d)\n",
			stage, result, runtime->chip_id, reset, pwron, pwron_ret);
		dev_err(runtime->dev, "pwr=0x%08x(%d)/0x%08x(%d)\n",
			pwr_status, status_ret, pwr_status_2nd, status_2nd_ret);
		return;
	}

	dev_info(runtime->dev,
		 "%s held: chip=0x%04x reset=%d shared=%pa+0x%llx\n",
		 stage, runtime->chip_id, reset, &runtime->shared_mem.start,
		 (unsigned long long)resource_size(&runtime->shared_mem));
	dev_info(runtime->dev, "BGF=%d(disabled) WDT=%d(unrequested)\n",
		 runtime->bgf_irq, runtime->wdt_irq);
}

const struct mt6797_consys_runtime_lease *
mt6797_consys_runtime_platform_lease(struct device *dev)
{
	struct mt6797_consys_runtime *runtime;

	if (!dev)
		return ERR_PTR(-EINVAL);
	runtime = dev_get_drvdata(dev);
	if (!runtime || !runtime->lease_held)
		return ERR_PTR(-ENODEV);
	if (!mt6797_consys_runtime_lease_active(&runtime->lease))
		return ERR_PTR(-ESHUTDOWN);
	return &runtime->lease;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_platform_lease);

enum mt6797_consys_runtime_provider_state
mt6797_consys_runtime_platform_state(struct device *dev)
{
	struct mt6797_consys_runtime *runtime;
	enum mt6797_consys_runtime_provider_state state;

	if (!dev)
		return MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED;
	runtime = dev_get_drvdata(dev);
	if (!runtime)
		return MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED;
	mutex_lock(&runtime->provider_lock);
	state = runtime->provider_state;
	mutex_unlock(&runtime->provider_lock);
	return state;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_platform_state);

int mt6797_consys_runtime_platform_lease_acquire(struct device *dev,
		struct mt6797_consys_runtime_provider_lease *lease)
{
	struct mt6797_consys_runtime *runtime;
	int ret;

	if (!dev || !lease || lease->device)
		return -EINVAL;
	runtime = dev_get_drvdata(dev);
	if (!runtime)
		return -ENODEV;

	mutex_lock(&runtime->provider_lock);
	if (runtime->provider_state !=
		    MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD ||
		    !runtime->lease_held) {
		ret = -ESHUTDOWN;
	} else if (runtime->external_users == UINT_MAX) {
		ret = -EOVERFLOW;
	} else {
		ret = mt6797_consys_runtime_lease_pin(&runtime->lease);
		if (!ret) {
			lease->device = get_device(dev);
			if (!lease->device) {
				mt6797_consys_runtime_lease_unpin(&runtime->lease);
				ret = -ENODEV;
			} else {
				lease->identity = &runtime->lease;
				runtime->external_users++;
			}
		}
	}
	mutex_unlock(&runtime->provider_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_platform_lease_acquire);

int mt6797_consys_runtime_platform_lease_release(
		struct mt6797_consys_runtime_provider_lease *lease)
{
	struct mt6797_consys_runtime *runtime;
	struct device *dev;
	int ret = 0;

	if (!lease || !lease->device || !lease->identity)
		return -EINVAL;
	dev = lease->device;
	runtime = dev_get_drvdata(dev);
	if (!runtime)
		return -ESHUTDOWN;

	mutex_lock(&runtime->provider_lock);
	if (lease->identity != &runtime->lease || !runtime->external_users) {
		ret = -EINVAL;
	} else {
		mt6797_consys_runtime_lease_unpin(lease->identity);
		runtime->external_users--;
		lease->device = NULL;
		lease->identity = NULL;
		if (!runtime->external_users)
			wake_up_all(&runtime->provider_wait);
	}
	mutex_unlock(&runtime->provider_lock);
	if (!ret)
		put_device(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_runtime_platform_lease_release);

static void mt6797_consys_runtime_put_domain_args(
		struct mt6797_consys_runtime *runtime)
{
	if (runtime->conn_domain.np) {
		of_node_put(runtime->conn_domain.np);
		runtime->conn_domain.np = NULL;
	}
}

static void mt6797_consys_runtime_wait_external(
		struct mt6797_consys_runtime *runtime)
{
	wait_event(runtime->provider_wait, !READ_ONCE(runtime->external_users));
}

static int mt6797_consys_runtime_probe(struct platform_device *pdev)
{
	struct mt6797_consys_runtime *runtime;
	struct mt6797_consys_timeouts timeouts = {
		.poll_interval_us = MT6797_CONSYS_RUNTIME_CHIP_POLL_INTERVAL_US,
		.chip_ready_timeout_us = MT6797_CONSYS_RUNTIME_CHIP_POLL_TIMEOUT_US,
	};
	int ret;

	if (!READ_ONCE(enable_experimental_runtime) ||
	    !device_property_read_bool(&pdev->dev,
				       "mediatek,experimental-runtime"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "runtime requires explicit DT and parameter opt-in\n");

	runtime = devm_kzalloc(&pdev->dev, sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return -ENOMEM;
	runtime->dev = &pdev->dev;
	mutex_init(&runtime->provider_lock);
	init_waitqueue_head(&runtime->provider_wait);
	runtime->provider_state =
		MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED;

	ret = mt6797_consys_runtime_map_resource(pdev,
						  &mt6797_consys_runtime_resources[0],
						  &runtime->mcu);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_map_resource(pdev,
						  &mt6797_consys_runtime_resources[1], NULL);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_map_resource(pdev,
						  &mt6797_consys_runtime_resources[2], NULL);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_map_resource(pdev,
						  &mt6797_consys_runtime_resources[3], NULL);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_acquire_shared_mem(runtime);
	if (ret)
		return ret;
	ret = mt6797_consys_runtime_validate_irqs(pdev, runtime);
	if (ret)
		return ret;

	runtime->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							    "mediatek,infracfg");
	if (IS_ERR(runtime->infracfg))
		return dev_err_probe(&pdev->dev, PTR_ERR(runtime->infracfg),
				     "failed to acquire INFRACFG regmap\n");
	runtime->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "mediatek,spm");
	if (IS_ERR(runtime->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(runtime->spm),
				     "failed to acquire SPM regmap\n");
	runtime->vcn18 = devm_regulator_get(&pdev->dev, "vcn18");
	if (IS_ERR(runtime->vcn18))
		return dev_err_probe(&pdev->dev, PTR_ERR(runtime->vcn18),
				     "failed to acquire VCN18\n");
	runtime->vcn28 = devm_regulator_get(&pdev->dev, "vcn28");
	if (IS_ERR(runtime->vcn28))
		return dev_err_probe(&pdev->dev, PTR_ERR(runtime->vcn28),
				     "failed to acquire VCN28\n");
	runtime->conn_mcu_reset =
		devm_reset_control_get_exclusive(&pdev->dev, "conn-mcu");
	if (IS_ERR(runtime->conn_mcu_reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(runtime->conn_mcu_reset),
				     "failed to acquire CONMCU reset\n");

	ret = of_parse_phandle_with_args(pdev->dev.of_node,
					 "mediatek,conn-power-domain",
					 "#power-domain-cells", 0,
					 &runtime->conn_domain);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to acquire CONN power domain\n");

	ret = devm_request_threaded_irq(&pdev->dev, runtime->bgf_irq, NULL,
					mt6797_consys_runtime_bgf_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(&pdev->dev), runtime);
	if (ret)
		goto put_domain_args;

	ret = mt6797_consys_lifecycle_init(&runtime->lifecycle,
						  &mt6797_consys_runtime_ops,
						  runtime, &timeouts);
	if (ret)
		goto put_domain_args;
	runtime->provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_RESOURCES_READY;

	ret = mt6797_consys_runtime_lease_acquire(&runtime->lifecycle,
							   &runtime->lease);
	if (ret) {
		mt6797_consys_runtime_log_state(runtime, "runtime identity", ret);
		goto put_domain_args;
	}
	runtime->lease_held = true;
	runtime->provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD;
	platform_set_drvdata(pdev, runtime);
	mt6797_consys_runtime_log_state(runtime, "runtime identity", 0);
	return 0;

put_domain_args:
	/* Retry only the provider teardown if a core unwind reported an error. */
	mt6797_consys_runtime_release_domain(runtime);
	mt6797_consys_runtime_put_domain_args(runtime);
	return ret;
}

static void mt6797_consys_runtime_remove(struct platform_device *pdev)
{
	struct mt6797_consys_runtime *runtime = platform_get_drvdata(pdev);
	int ret;

	if (!runtime)
		return;

	mutex_lock(&runtime->provider_lock);
	runtime->provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_REMOVING;
	mutex_unlock(&runtime->provider_lock);
	mt6797_consys_runtime_wait_external(runtime);

	ret = mt6797_consys_runtime_lease_release(&runtime->lease);
	if (ret) {
		/* No consumer is wired yet: failure means a future contract was broken. */
		mt6797_consys_runtime_log_state(runtime, "runtime release", ret);
		mt6797_consys_runtime_release_domain(runtime);
		return;
	}
	runtime->lease_held = false;
	platform_set_drvdata(pdev, NULL);
	mt6797_consys_runtime_put_domain_args(runtime);
	dev_info(&pdev->dev, "runtime lease released with reset asserted\n");
}

static const struct of_device_id mt6797_consys_runtime_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-runtime" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_runtime_of_match);

static struct platform_driver mt6797_consys_runtime_driver = {
	.probe = mt6797_consys_runtime_probe,
	.remove = mt6797_consys_runtime_remove,
	.driver = {
		.name = "mt6797-consys-runtime",
		.of_match_table = mt6797_consys_runtime_of_match,
		/* A held lease is not a safely unbindable transient experiment. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_runtime_driver);

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_RUNTIME_KUNIT_TEST)

enum mt6797_consys_runtime_test_call {
	MT6797_CONSYS_RUNTIME_TEST_VCN18_ON,
	MT6797_CONSYS_RUNTIME_TEST_DELAY,
	MT6797_CONSYS_RUNTIME_TEST_VCN28_ON,
	MT6797_CONSYS_RUNTIME_TEST_SLEEP_MASK,
	MT6797_CONSYS_RUNTIME_TEST_RESET,
	MT6797_CONSYS_RUNTIME_TEST_PWRON,
	MT6797_CONSYS_RUNTIME_TEST_DOMAIN_ON,
	MT6797_CONSYS_RUNTIME_TEST_POLL,
	MT6797_CONSYS_RUNTIME_TEST_DOMAIN_OFF,
	MT6797_CONSYS_RUNTIME_TEST_VCN28_OFF,
	MT6797_CONSYS_RUNTIME_TEST_VCN18_OFF,
};

struct mt6797_consys_runtime_test_backend {
	enum mt6797_consys_runtime_test_call calls[16];
	u32 delays[2];
	unsigned int call_count;
	unsigned int delay_count;
	int fail_call;
};

static int mt6797_consys_runtime_test_record(void *context,
					      enum mt6797_consys_runtime_test_call call)
{
	struct mt6797_consys_runtime_test_backend *backend = context;

	if (backend->call_count < ARRAY_SIZE(backend->calls))
		backend->calls[backend->call_count++] = call;
	return backend->fail_call == call ? -EIO : 0;
}

static int mt6797_consys_runtime_test_vcn18_on(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_VCN18_ON);
}

static int mt6797_consys_runtime_test_vcn18_off(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_VCN18_OFF);
}

static int mt6797_consys_runtime_test_vcn28_on(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_VCN28_ON);
}

static int mt6797_consys_runtime_test_vcn28_off(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_VCN28_OFF);
}

static int mt6797_consys_runtime_test_sleep_mask(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_SLEEP_MASK);
}

static int mt6797_consys_runtime_test_reset(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_RESET);
}

static int mt6797_consys_runtime_test_pwron(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_PWRON);
}

static int mt6797_consys_runtime_test_domain_on(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_DOMAIN_ON);
}

static int mt6797_consys_runtime_test_domain_off(void *context)
{
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_DOMAIN_OFF);
}

static int mt6797_consys_runtime_test_poll(void *context,
					  enum mt6797_consys_poll poll)
{
	if (poll != MT6797_CONSYS_POLL_CHIP_READY)
		return -EINVAL;
	return mt6797_consys_runtime_test_record(context,
						MT6797_CONSYS_RUNTIME_TEST_POLL);
}

static void mt6797_consys_runtime_test_delay(void *context, u32 min, u32 max)
{
	struct mt6797_consys_runtime_test_backend *backend = context;

	if (backend->delay_count < ARRAY_SIZE(backend->delays))
		backend->delays[backend->delay_count++] = min;
	mt6797_consys_runtime_test_record(context,
					 MT6797_CONSYS_RUNTIME_TEST_DELAY);
}

static const struct mt6797_consys_ops mt6797_consys_runtime_test_ops = {
	.abi = MT6797_CONSYS_LIFECYCLE_ABI,
	.enable_vcn18 = mt6797_consys_runtime_test_vcn18_on,
	.disable_vcn18 = mt6797_consys_runtime_test_vcn18_off,
	.enable_vcn28 = mt6797_consys_runtime_test_vcn28_on,
	.disable_vcn28 = mt6797_consys_runtime_test_vcn28_off,
	.configure_sleep_mask = mt6797_consys_runtime_test_sleep_mask,
	.assert_reset = mt6797_consys_runtime_test_reset,
	.configure_pwron = mt6797_consys_runtime_test_pwron,
	.enable_scp_sys_conn = mt6797_consys_runtime_test_domain_on,
	.disable_scp_sys_conn = mt6797_consys_runtime_test_domain_off,
	.poll = mt6797_consys_runtime_test_poll,
	.delay_us = mt6797_consys_runtime_test_delay,
};

static void mt6797_consys_runtime_test_init(struct kunit *test,
					     struct mt6797_consys_lifecycle *lifecycle,
					     struct mt6797_consys_runtime_test_backend *backend)
{
	struct mt6797_consys_timeouts timeouts = {
		.poll_interval_us = 1,
		.chip_ready_timeout_us = 1,
	};

	backend->fail_call = -1;
	KUNIT_ASSERT_EQ(test, mt6797_consys_lifecycle_init(lifecycle,
						       &mt6797_consys_runtime_test_ops,
						       backend, &timeouts), 0);
}

static void mt6797_consys_runtime_test_lease_order(struct kunit *test)
{
	static const enum mt6797_consys_runtime_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_TEST_VCN18_ON,
		MT6797_CONSYS_RUNTIME_TEST_DELAY,
		MT6797_CONSYS_RUNTIME_TEST_VCN28_ON,
		MT6797_CONSYS_RUNTIME_TEST_SLEEP_MASK,
		MT6797_CONSYS_RUNTIME_TEST_RESET,
		MT6797_CONSYS_RUNTIME_TEST_PWRON,
		MT6797_CONSYS_RUNTIME_TEST_DOMAIN_ON,
		MT6797_CONSYS_RUNTIME_TEST_DELAY,
		MT6797_CONSYS_RUNTIME_TEST_POLL,
		MT6797_CONSYS_RUNTIME_TEST_RESET,
		MT6797_CONSYS_RUNTIME_TEST_DOMAIN_OFF,
		MT6797_CONSYS_RUNTIME_TEST_VCN28_OFF,
		MT6797_CONSYS_RUNTIME_TEST_VCN18_OFF,
	};
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_runtime_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_runtime_test_init(test, &lifecycle, &backend);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
							   &lease), 0);
	KUNIT_EXPECT_TRUE(test, mt6797_consys_runtime_lease_active(&lease));
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&lease), 0);
	KUNIT_EXPECT_EQ(test, backend.delay_count, 2U);
	KUNIT_EXPECT_EQ(test, backend.delays[0], MT6797_CONSYS_VCN18_SETTLE_US);
	KUNIT_EXPECT_EQ(test, backend.delays[1], MT6797_CONSYS_DOMAIN_SETTLE_US);
	KUNIT_EXPECT_EQ(test, backend.call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend.calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_test_failure_unwinds(struct kunit *test)
{
	static const enum mt6797_consys_runtime_test_call expected[] = {
		MT6797_CONSYS_RUNTIME_TEST_VCN18_ON,
		MT6797_CONSYS_RUNTIME_TEST_DELAY,
		MT6797_CONSYS_RUNTIME_TEST_VCN28_ON,
		MT6797_CONSYS_RUNTIME_TEST_SLEEP_MASK,
		MT6797_CONSYS_RUNTIME_TEST_RESET,
		MT6797_CONSYS_RUNTIME_TEST_PWRON,
		MT6797_CONSYS_RUNTIME_TEST_DOMAIN_ON,
		MT6797_CONSYS_RUNTIME_TEST_RESET,
		MT6797_CONSYS_RUNTIME_TEST_DOMAIN_OFF,
		MT6797_CONSYS_RUNTIME_TEST_VCN28_OFF,
		MT6797_CONSYS_RUNTIME_TEST_VCN18_OFF,
	};
	struct mt6797_consys_runtime_lease lease = { };
	struct mt6797_consys_runtime_test_backend backend = { };
	struct mt6797_consys_lifecycle lifecycle;

	mt6797_consys_runtime_test_init(test, &lifecycle, &backend);
	backend.fail_call = MT6797_CONSYS_RUNTIME_TEST_DOMAIN_ON;
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_acquire(&lifecycle,
							   &lease), -EIO);
	KUNIT_EXPECT_FALSE(test, mt6797_consys_runtime_lease_active(&lease));
	KUNIT_EXPECT_EQ(test, mt6797_consys_lifecycle_state(&lifecycle),
				MT6797_CONSYS_OFF);
	KUNIT_EXPECT_EQ(test, backend.call_count, ARRAY_SIZE(expected));
	KUNIT_EXPECT_MEMEQ(test, backend.calls, expected, sizeof(expected));
}

static void mt6797_consys_runtime_test_contracts(struct kunit *test)
{
	struct mt6797_consys_runtime_resource expected = {
		.name = "mcu",
		.start = 0x18070000,
		.size = SZ_512,
	};
	struct of_phandle_args spec = {
		.args_count = 3,
		.args = { GIC_SPI, MT6797_CONSYS_RUNTIME_BGF_SPI,
				  IRQ_TYPE_LEVEL_LOW },
	};
	struct resource resource = DEFINE_RES_MEM(0x18070000, SZ_512);

	KUNIT_EXPECT_TRUE(test, mt6797_consys_runtime_resource_valid(&resource,
									&expected));
	resource.end++;
	KUNIT_EXPECT_FALSE(test, mt6797_consys_runtime_resource_valid(&resource,
									 &expected));
	KUNIT_EXPECT_TRUE(test, mt6797_consys_runtime_irq_spec_valid(&spec,
									 MT6797_CONSYS_RUNTIME_BGF_SPI));
	spec.args[2] = IRQ_TYPE_EDGE_FALLING;
	KUNIT_EXPECT_FALSE(test, mt6797_consys_runtime_irq_spec_valid(&spec,
								 MT6797_CONSYS_RUNTIME_BGF_SPI));
}

static void mt6797_consys_runtime_test_device_release(struct device *dev)
{
}

static void mt6797_consys_runtime_test_provider_setup(
		struct kunit *test, struct mt6797_consys_runtime *runtime,
		struct device *dev, struct mt6797_consys_runtime_test_backend *backend)
{
	device_initialize(dev);
	dev->release = mt6797_consys_runtime_test_device_release;
	runtime->dev = dev;
	mutex_init(&runtime->provider_lock);
	init_waitqueue_head(&runtime->provider_wait);
	runtime->provider_state =
		MT6797_CONSYS_RUNTIME_PROVIDER_UNINITIALIZED;
	dev_set_drvdata(dev, runtime);
	mt6797_consys_runtime_test_init(test, &runtime->lifecycle, backend);
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_lease_acquire(&runtime->lifecycle,
							   &runtime->lease), 0);
	runtime->lease_held = true;
	runtime->provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD;
}

static void mt6797_consys_runtime_test_provider_lease_and_remove_fence(
		struct kunit *test)
{
	struct mt6797_consys_runtime_test_backend backend = { };
	struct mt6797_consys_runtime_provider_lease external = { };
	struct mt6797_consys_runtime runtime = { };
	struct device dev;

	mt6797_consys_runtime_test_provider_setup(test, &runtime, &dev,
							 &backend);
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_platform_state(&dev),
			MT6797_CONSYS_RUNTIME_PROVIDER_IDENTITY_HELD);
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_platform_lease_acquire(&dev, &external),
			0);
	KUNIT_EXPECT_EQ(test, runtime.external_users, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_consys_runtime_lease_release(&runtime.lease),
			-EBUSY);
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_platform_lease_release(&external), 0);
	KUNIT_EXPECT_EQ(test, runtime.external_users, 0U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&runtime.lease),
			0);
	put_device(&dev);
}

struct mt6797_consys_runtime_test_waiter {
	struct mt6797_consys_runtime *runtime;
	struct completion started;
	struct completion finished;
};

static int mt6797_consys_runtime_test_waiter_thread(void *data)
{
	struct mt6797_consys_runtime_test_waiter *waiter = data;

	complete(&waiter->started);
	mt6797_consys_runtime_wait_external(waiter->runtime);
	complete(&waiter->finished);
	return 0;
}

static void mt6797_consys_runtime_test_provider_waits_concurrently(
		struct kunit *test)
{
	struct mt6797_consys_runtime_test_backend backend = { };
	struct mt6797_consys_runtime_provider_lease external = { };
	struct mt6797_consys_runtime_test_waiter waiter;
	struct mt6797_consys_runtime runtime = { };
	struct task_struct *task;
	struct device dev;

	mt6797_consys_runtime_test_provider_setup(test, &runtime, &dev,
							 &backend);
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_platform_lease_acquire(&dev, &external),
			0);
	waiter.runtime = &runtime;
	init_completion(&waiter.started);
	init_completion(&waiter.finished);
	task = kthread_run(mt6797_consys_runtime_test_waiter_thread, &waiter,
			   "mt6797-runtime-wait");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	wait_for_completion(&waiter.started);
	KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&waiter.finished,
							 msecs_to_jiffies(10)), 0UL);
	KUNIT_ASSERT_EQ(test,
			mt6797_consys_runtime_platform_lease_release(&external), 0);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&waiter.finished,
							 msecs_to_jiffies(100)), 0UL);
	kthread_stop(task);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&runtime.lease),
			0);
	put_device(&dev);
}

static void mt6797_consys_runtime_test_provider_rejects_unready_state(
		struct kunit *test)
{
	struct mt6797_consys_runtime_test_backend backend = { };
	struct mt6797_consys_runtime_provider_lease external = { };
	struct mt6797_consys_runtime runtime = { };
	struct device dev;

	mt6797_consys_runtime_test_provider_setup(test, &runtime, &dev,
							 &backend);
	runtime.provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_RESOURCES_READY;
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_platform_lease_acquire(&dev, &external),
			-ESHUTDOWN);
	runtime.provider_state = MT6797_CONSYS_RUNTIME_PROVIDER_REMOVING;
	KUNIT_EXPECT_EQ(test,
			mt6797_consys_runtime_platform_lease_acquire(&dev, &external),
			-ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, runtime.external_users, 0U);
	KUNIT_ASSERT_EQ(test, mt6797_consys_runtime_lease_release(&runtime.lease),
			0);
	put_device(&dev);
}

static struct kunit_case mt6797_consys_runtime_test_cases[] = {
	KUNIT_CASE(mt6797_consys_runtime_test_lease_order),
	KUNIT_CASE(mt6797_consys_runtime_test_failure_unwinds),
	KUNIT_CASE(mt6797_consys_runtime_test_contracts),
	KUNIT_CASE(mt6797_consys_runtime_test_provider_lease_and_remove_fence),
	KUNIT_CASE(mt6797_consys_runtime_test_provider_waits_concurrently),
	KUNIT_CASE(mt6797_consys_runtime_test_provider_rejects_unready_state),
	{ }
};

static struct kunit_suite mt6797_consys_runtime_test_suite = {
	.name = "mt6797-consys-runtime",
	.test_cases = mt6797_consys_runtime_test_cases,
};

kunit_test_suite(mt6797_consys_runtime_test_suite);

#endif

MODULE_DESCRIPTION("Explicitly gated MT6797 CONSYS runtime lifecycle provider");
MODULE_LICENSE("GPL");
