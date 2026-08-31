// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explicitly gated MT6797 CONSYS shared-EMI MPU owner.
 *
 * Vendor MT6797 code installs region 19 through SIP 0x82000209, sets the
 * CONSYS-to-AP remap field, and clears 343 KiB at the upper half of the 2 MiB
 * reservation. The vendor path has no inverse. This provider keeps that
 * lifetime explicit: successful and failed transactions are terminal.
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-consys-emi-mpu.h>
#include <linux/soc/mediatek/mt6797-consys-emi.h>

#define MT6797_EMI_MPU_BASE			0x10203000
#define MT6797_EMI_MPU_SIZE			SZ_4K
#define MT6797_TOPCKGEN_BASE			0x10000000
#define MT6797_TOPCKGEN_SIZE			SZ_8K
#define MT6797_CONSYS_EMI_REMAP_FIELD_MASK	GENMASK(12, 0)
#define MT6797_EMI_MPU_SMC_SET			0x82000209

/* wlan_lib.c:844-850 opens region 18 over exactly the first 512 KiB. */
#define MT6797_CONSYS_EMI_WIFI_WINDOW_SIZE	SZ_512K
#define MT6797_CONSYS_EMI_WIFI_REGION		18
#define MT6797_CONSYS_EMI_WIFI_OPEN_PERMISSION	0x00000000
/* wlan_lib.c:873-877: F,F,F,F,F,N,F,F. */
#define MT6797_CONSYS_EMI_WIFI_CLOSED_PERMISSION	0x00b6da2d

/* Vendor SET_ACCESS_PERMISSON(FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
 * FORBIDDEN, NO_PROTECTION, FORBIDDEN, NO_PROTECTION), for region 19.
 */
#define MT6797_CONSYS_EMI_REGION19_PERMISSION	0x00b6da28

static bool enable_experimental_ownership;
module_param_named(enable_experimental_ownership, enable_experimental_ownership,
		   bool, 0400);
MODULE_PARM_DESC(enable_experimental_ownership,
		 "allow the MT6797 CONSYS persistent shared-EMI ownership experiment");

struct mt6797_consys_emi_mpu_resource {
	const char *name;
	resource_size_t start;
	resource_size_t size;
};

struct mt6797_consys_emi_mpu {
	struct device *dev;
	void __iomem *topckgen;
	struct resource shared_mem;
	struct mt6797_consys_emi_layout layout;
	struct mt6797_consys_emi_owner owner;

	struct mutex wifi_window_lock;
	struct device *wifi_window_consumer;
	bool wifi_window_open;
};

/*
 * This lives in the provider module rather than a DT-created device.  A DT
 * overlay can remove and recreate the platform device, but it must never
 * recreate a path through a one-way EMI transaction in the same boot.
 */
enum mt6797_consys_emi_mpu_boot_state {
	MT6797_CONSYS_EMI_MPU_BOOT_IDLE,
	MT6797_CONSYS_EMI_MPU_BOOT_ATTEMPTED,
	MT6797_CONSYS_EMI_MPU_BOOT_COMMITTED,
	MT6797_CONSYS_EMI_MPU_BOOT_FATAL,
};

static DEFINE_MUTEX(mt6797_consys_emi_mpu_boot_lock);
static enum mt6797_consys_emi_mpu_boot_state mt6797_consys_emi_mpu_boot_state;
static bool mt6797_consys_emi_mpu_boot_module_pinned;

static const struct mt6797_consys_emi_mpu_resource mt6797_consys_emi_mpu_resources[] = {
	{ "emi", MT6797_EMI_MPU_BASE, MT6797_EMI_MPU_SIZE },
	{ "topckgen", MT6797_TOPCKGEN_BASE, MT6797_TOPCKGEN_SIZE },
};

static int
mt6797_consys_emi_mpu_get_resource(struct platform_device *pdev,
				   const struct mt6797_consys_emi_mpu_resource *expected,
				   void __iomem **base)
{
	const struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, expected->name);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL,
					 "missing %s resource\n", expected->name);
	if (res->start != expected->start || resource_size(res) != expected->size)
		return dev_err_probe(&pdev->dev, -EINVAL,
					 "invalid %s resource\n", expected->name);
	if (!base)
		return 0;

	/* TOPCKGEN is already owned by the clock provider; validate, then share. */
	*base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!*base)
		return -ENOMEM;
	return 0;
}

static int
mt6797_consys_emi_mpu_get_shared_mem(struct mt6797_consys_emi_mpu *mpu)
{
	struct device_node *memory_region;
	int ret;

	memory_region = of_parse_phandle(mpu->dev->of_node, "memory-region", 0);
	if (!memory_region)
		return dev_err_probe(mpu->dev, -EINVAL,
					 "missing shared-memory reservation\n");
	if (!of_property_read_bool(memory_region, "no-map")) {
		of_node_put(memory_region);
		return dev_err_probe(mpu->dev, -EINVAL,
					 "shared-memory reservation must be no-map\n");
	}
	of_node_put(memory_region);

	ret = of_reserved_mem_region_to_resource_byname(mpu->dev->of_node,
							"shared", &mpu->shared_mem);
	if (ret)
		return dev_err_probe(mpu->dev, ret,
					 "failed to acquire shared-memory reservation\n");

	return mt6797_consys_emi_layout_build(mpu->shared_mem.start,
					      resource_size(&mpu->shared_mem), &mpu->layout);
}

static int
mt6797_consys_emi_mpu_protect(void *context,
			      const struct mt6797_consys_emi_layout *layout)
{
	struct mt6797_consys_emi_mpu *mpu = context;
	struct arm_smccc_res res;
	long status;

	arm_smccc_smc(MT6797_EMI_MPU_SMC_SET, layout->protection_start,
		      layout->protection_end,
		      MT6797_CONSYS_EMI_REGION19_PERMISSION |
		      (layout->protection_region << 27),
		      0, 0, 0, 0, &res);
	status = (long)res.a0;
	if (!status)
		return 0;
	if (status < 0 && status >= -MAX_ERRNO)
		return status;

	dev_err(mpu->dev, "region-19 SIP returned %#lx\n", status);
	return -EIO;
}

static int
mt6797_consys_emi_mpu_remap(void *context,
			    const struct mt6797_consys_emi_layout *layout)
{
	struct mt6797_consys_emi_mpu *mpu = context;
	u32 old;
	u32 new;

	old = readl(mpu->topckgen + MT6797_CONSYS_EMI_MAPPING_OFFSET);
	if (old & MT6797_CONSYS_EMI_REMAP_FIELD_MASK) {
		dev_err(mpu->dev, "CONSYS EMI remap field already owns %#lx\n",
			old & MT6797_CONSYS_EMI_REMAP_FIELD_MASK);
		return -EBUSY;
	}

	new = old | layout->remap_value;
	writel(new, mpu->topckgen + MT6797_CONSYS_EMI_MAPPING_OFFSET);
	if ((readl(mpu->topckgen + MT6797_CONSYS_EMI_MAPPING_OFFSET) &
	     MT6797_CONSYS_EMI_REMAP_FIELD_MASK) != layout->remap_value) {
		dev_err(mpu->dev, "CONSYS EMI remap write did not stick\n");
		return -EIO;
	}

	return 0;
}

static int
mt6797_consys_emi_mpu_map_clear(void *context,
				const struct mt6797_consys_emi_layout *layout)
{
	struct mt6797_consys_emi_mpu *mpu = context;
	void __iomem *clear;

	clear = ioremap(layout->clear_start, layout->clear_size);
	if (!clear) {
		dev_err(mpu->dev, "failed to map shared-EMI clear window\n");
		return -ENOMEM;
	}

	memset_io(clear, 0, layout->clear_size);
	iounmap(clear);
	return 0;
}

static const struct mt6797_consys_emi_ops mt6797_consys_emi_mpu_ops = {
	.abi = MT6797_CONSYS_EMI_ABI,
	.protect = mt6797_consys_emi_mpu_protect,
	.remap = mt6797_consys_emi_mpu_remap,
	.map_clear = mt6797_consys_emi_mpu_map_clear,
};

static int
mt6797_consys_emi_mpu_claim_provider(struct mt6797_consys_emi_mpu *mpu)
{
	int ret;

	if (!enable_experimental_ownership)
		return -EPERM;

	/*
	 * protect() can have changed hardware before returning an error and the
	 * vendor supplies no inverse.  This must outlive a DT overlay's platform
	 * device, so consume a module-global boot latch before calling it.
	 */
	mutex_lock(&mt6797_consys_emi_mpu_boot_lock);
	if (mt6797_consys_emi_mpu_boot_state != MT6797_CONSYS_EMI_MPU_BOOT_IDLE) {
		ret = -EALREADY;
		goto unlock_boot_state;
	}
	if (!mt6797_consys_emi_mpu_boot_module_pinned) {
		if (!try_module_get(THIS_MODULE)) {
			ret = -ESHUTDOWN;
			goto unlock_boot_state;
		}
		mt6797_consys_emi_mpu_boot_module_pinned = true;
	}
	mt6797_consys_emi_mpu_boot_state = MT6797_CONSYS_EMI_MPU_BOOT_ATTEMPTED;
	mutex_unlock(&mt6797_consys_emi_mpu_boot_lock);

	ret = mt6797_consys_emi_start(&mpu->owner, &mpu->layout, true);
	mutex_lock(&mt6797_consys_emi_mpu_boot_lock);
	mt6797_consys_emi_mpu_boot_state = ret ? MT6797_CONSYS_EMI_MPU_BOOT_FATAL :
		MT6797_CONSYS_EMI_MPU_BOOT_COMMITTED;

unlock_boot_state:
	mutex_unlock(&mt6797_consys_emi_mpu_boot_lock);
	return ret;
}

int mt6797_consys_emi_mpu_claim(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct mt6797_consys_emi_mpu *mpu;
	int ret;

	if (!consumer || !consumer->of_node)
		return -EINVAL;

	np = of_parse_phandle(consumer->of_node, "mediatek,emi-mpu", 0);
	if (!np)
		return -EINVAL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;

	/* Serialize against provider unbind while using its devm-owned state. */
	device_lock(&pdev->dev);
	mpu = platform_get_drvdata(pdev);
	if (!mpu)
		ret = -EPROBE_DEFER;
	else
		ret = mt6797_consys_emi_mpu_claim_provider(mpu);
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_mpu_claim);
/* Return a locked provider whose devm state cannot change during this call. */
static int mt6797_consys_emi_mpu_from_consumer(struct device *consumer,
						struct platform_device **provider,
						struct mt6797_consys_emi_mpu **mpu)
{
	struct device_node *np;
	struct platform_device *pdev;

	if (!consumer || !consumer->of_node || !provider || !mpu)
		return -EINVAL;
	np = of_parse_phandle(consumer->of_node, "mediatek,emi-mpu", 0);
	if (!np)
		return -EINVAL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;
	device_lock(&pdev->dev);
	*mpu = platform_get_drvdata(pdev);
	if (!*mpu) {
		device_unlock(&pdev->dev);
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	*provider = pdev;
	return 0;
}

static int mt6797_consys_emi_mpu_wifi_window_protect(
		struct mt6797_consys_emi_mpu *mpu, u32 permission)
{
	struct arm_smccc_res res;
	long status;

	arm_smccc_smc(MT6797_EMI_MPU_SMC_SET, mpu->shared_mem.start,
		mpu->shared_mem.start + MT6797_CONSYS_EMI_WIFI_WINDOW_SIZE - 1,
		permission | (MT6797_CONSYS_EMI_WIFI_REGION << 27),
		0, 0, 0, 0, &res);
	status = (long)res.a0;
	if (!status)
		return 0;
	return status < 0 ? status : -EIO;
}

int mt6797_consys_emi_mpu_wifi_window_open(struct device *consumer,
					   struct resource *window)
{
	struct platform_device *provider;
	struct mt6797_consys_emi_mpu *mpu;
	int ret;

	if (!window)
		return -EINVAL;
	ret = mt6797_consys_emi_mpu_from_consumer(consumer, &provider, &mpu);
	if (ret)
		return ret;
	mutex_lock(&mt6797_consys_emi_mpu_boot_lock);
	if (mt6797_consys_emi_mpu_boot_state != MT6797_CONSYS_EMI_MPU_BOOT_COMMITTED)
		ret = -ESHUTDOWN;
	mutex_unlock(&mt6797_consys_emi_mpu_boot_lock);
	if (ret)
		goto out_provider;

	mutex_lock(&mpu->wifi_window_lock);
	if (mpu->wifi_window_open) {
		ret = -EBUSY;
		goto out_window;
	}
	/* Vendor wlan_lib.c:844-850: open all eight domains before ioremap. */
	ret = mt6797_consys_emi_mpu_wifi_window_protect(mpu,
		MT6797_CONSYS_EMI_WIFI_OPEN_PERMISSION);
	if (ret) {
		/* SIP can alter protection before returning an error. */
		mt6797_consys_emi_mpu_latch_fatal();
		goto out_window;
	}
	window->name = "mt6797-wifi-emi";
	window->start = mpu->shared_mem.start;
	window->end = mpu->shared_mem.start + MT6797_CONSYS_EMI_WIFI_WINDOW_SIZE - 1;
	window->flags = IORESOURCE_MEM;
	mpu->wifi_window_consumer = consumer;
	mpu->wifi_window_open = true;
out_window:
	mutex_unlock(&mpu->wifi_window_lock);
out_provider:
	device_unlock(&provider->dev);
	put_device(&provider->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_mpu_wifi_window_open);

int mt6797_consys_emi_mpu_wifi_window_close(struct device *consumer)
{
	struct platform_device *provider;
	struct mt6797_consys_emi_mpu *mpu;
	int ret;

	ret = mt6797_consys_emi_mpu_from_consumer(consumer, &provider, &mpu);
	if (ret)
		return ret;
	mutex_lock(&mpu->wifi_window_lock);
	if (!mpu->wifi_window_open || mpu->wifi_window_consumer != consumer) {
		ret = -EPERM;
		goto out_window;
	}
	/* Vendor wlan_lib.c:873-877 restores the protected region before return. */
	ret = mt6797_consys_emi_mpu_wifi_window_protect(mpu,
		MT6797_CONSYS_EMI_WIFI_CLOSED_PERMISSION);
	mpu->wifi_window_open = false;
	mpu->wifi_window_consumer = NULL;
	if (ret)
		mt6797_consys_emi_mpu_latch_fatal();
out_window:
	mutex_unlock(&mpu->wifi_window_lock);
	device_unlock(&provider->dev);
	put_device(&provider->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_mpu_wifi_window_close);

void mt6797_consys_emi_mpu_latch_fatal(void)
{
	mutex_lock(&mt6797_consys_emi_mpu_boot_lock);
	if (mt6797_consys_emi_mpu_boot_state != MT6797_CONSYS_EMI_MPU_BOOT_IDLE)
		mt6797_consys_emi_mpu_boot_state = MT6797_CONSYS_EMI_MPU_BOOT_FATAL;
	mutex_unlock(&mt6797_consys_emi_mpu_boot_lock);
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_mpu_latch_fatal);

static int mt6797_consys_emi_mpu_probe(struct platform_device *pdev)
{
	struct mt6797_consys_emi_mpu *mpu;
	int ret;

	if (!of_property_read_bool(pdev->dev.of_node,
				   "mediatek,experimental-shared-emi-ownership"))
		return dev_err_probe(&pdev->dev, -EPERM,
					 "shared-EMI ownership needs explicit DT opt-in\n");
	if (!enable_experimental_ownership)
		return dev_err_probe(&pdev->dev, -EPERM,
					 "shared-EMI ownership needs explicit boot opt-in\n");

	mpu = devm_kzalloc(&pdev->dev, sizeof(*mpu), GFP_KERNEL);
	if (!mpu)
		return -ENOMEM;
	mpu->dev = &pdev->dev;

	mutex_init(&mpu->wifi_window_lock);

	ret = mt6797_consys_emi_mpu_get_resource(pdev,
						 &mt6797_consys_emi_mpu_resources[0], NULL);
	if (ret)
		return ret;
	ret = mt6797_consys_emi_mpu_get_resource(pdev,
						 &mt6797_consys_emi_mpu_resources[1],
						 &mpu->topckgen);
	if (ret)
		return ret;
	ret = mt6797_consys_emi_mpu_get_shared_mem(mpu);
	if (ret)
		return ret;
	ret = mt6797_consys_emi_init(&mpu->owner, &mt6797_consys_emi_mpu_ops, mpu);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mpu);
	dev_info(&pdev->dev,
		 "shared EMI prepared; ownership remains inactive until claimed\n");
	return 0;
}

static const struct of_device_id mt6797_consys_emi_mpu_of_match[] = {
	{ .compatible = "mediatek,mt6797-consys-emi-mpu" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_consys_emi_mpu_of_match);

static struct platform_driver mt6797_consys_emi_mpu_driver = {
	.probe = mt6797_consys_emi_mpu_probe,
	.driver = {
		.name = "mt6797-consys-emi-mpu",
		.of_match_table = mt6797_consys_emi_mpu_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_consys_emi_mpu_driver);

MODULE_DESCRIPTION("Explicitly gated MT6797 CONSYS shared-EMI MPU owner");
MODULE_LICENSE("GPL");
