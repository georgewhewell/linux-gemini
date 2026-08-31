// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explicitly gated MT6797 MD1 infracfg remap/lock observer.
 *
 * The 3.18 vendor CCCI source maps infracfg-AO and names bank0/bank4 offsets
 * at drivers/misc/mediatek/eccci/mt6797/ccci_platform.c:183-187. Public LK
 * identifies the MD1 persistent lock at 0x10001f80 bit 16 and writes it only
 * after its remap work: lk/platform/mt6797/ccci_lk_load_img_plat.c:72-90,
 * 208-229, 813-817. This driver samples those public words only after the
 * separate LK hand-off probe has published a validated diagnostic plan.
 *
 * It never writes the map or lock. It contains no MPU/SMC, WDT, reset, power,
 * clock, PLL, CCIF, firmware, partition, CLDMA, DMA, transport, SIM, RF, or
 * boot-vector operation. A read cannot make a fabric wedge recoverable, so a
 * log line is emitted before and after each individual read.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>

#define MT6797_MD1_INFRACFG_AO_BASE	0x10001000
#define MT6797_MD1_INFRACFG_AO_SIZE	0x1000

static bool enable_experimental_md1_remap_observe;
module_param_named(enable_experimental_md1_remap_observe,
			   enable_experimental_md1_remap_observe, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_remap_observe,
	"allow one explicitly gated, read-only MT6797 MD1 remap/lock observation");

struct mt6797_md1_remap_observe_io {
	struct device *dev;
	void __iomem *base;
};

static DEFINE_MUTEX(mt6797_md1_remap_observation_lock);
static struct mt6797_md1_remap_observation mt6797_md1_remap_observation;

int mt6797_md1_remap_observation_get(
	struct mt6797_md1_remap_observation *observation)
{
	if (!observation)
		return -EINVAL;

	mutex_lock(&mt6797_md1_remap_observation_lock);
	*observation = mt6797_md1_remap_observation;
	mutex_unlock(&mt6797_md1_remap_observation_lock);
	return observation->observed ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(mt6797_md1_remap_observation_get);

static u32 mt6797_md1_remap_observe_read(void *context, u32 offset)
{
	struct mt6797_md1_remap_observe_io *io = context;
	u32 value;

	dev_info(io->dev, "before MD1 remap read offset=%#x\n", offset);
	value = readl(io->base + offset);
	dev_info(io->dev, "after MD1 remap read offset=%#x value=%#08x\n",
		 offset, value);
	return value;
}

static const char *mt6797_md1_remap_mode_name(
	enum mt6797_md1_remap_attested_mode mode)
{
	switch (mode) {
	case MT6797_MD1_REMAP_ATTEST_PHYS32:
		return "phys32";
	case MT6797_MD1_REMAP_ATTEST_KERNEL_EMI_40000000:
		return "kernel-emi-40000000";
	default:
		return "none";
	}
}

static int mt6797_md1_remap_observe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_remap_observation observation;
	struct mt6797_md1_remap_observe_io io;
	const struct resource *res;
	void __iomem *base;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_remap_observe) ||
	    !device_property_read_bool(dev,
		"mediatek,experimental-md1-remap-observe") ||
	    !device_property_read_bool(dev,
		"mediatek,stage-lk-remap-observe-read-only"))
		return dev_err_probe(dev, -EPERM,
			"remap observer requires independent cmdline and read-only DT gates\n");

	/* Do not map or read infracfg until the other gate has validated LK data. */
	ret = mt6797_md1_lk_handoff_plan_get(&plan);
	if (ret == -ENODATA)
		return dev_err_probe(dev, -EPROBE_DEFER,
			"waiting for validated LK hand-off plan\n");
	if (ret)
		return dev_err_probe(dev, ret, "could not copy LK hand-off plan\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "infracfg-ao");
	if (!res)
		return dev_err_probe(dev, -EINVAL, "missing infracfg-ao resource\n");
	if (res->start != MT6797_MD1_INFRACFG_AO_BASE ||
	    resource_size(res) != MT6797_MD1_INFRACFG_AO_SIZE)
		return dev_err_probe(dev, -EINVAL,
			"infracfg-ao must be %08x/%#x\n",
			MT6797_MD1_INFRACFG_AO_BASE, MT6797_MD1_INFRACFG_AO_SIZE);

	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	io = (struct mt6797_md1_remap_observe_io) {
		.dev = dev,
		.base = base,
	};
	ret = mt6797_md1_remap_observe(&plan, mt6797_md1_remap_observe_read,
					&io, &observation);
	if (ret)
		return dev_err_probe(dev, ret,
			"remap observer rejected LK plan before any MMIO read\n");

	mutex_lock(&mt6797_md1_remap_observation_lock);
	mt6797_md1_remap_observation = observation;
	mutex_unlock(&mt6797_md1_remap_observation_lock);
	if (observation.classification)
		dev_warn(dev,
			 "MD1 remap observed bank0=%08x/%08x bank4=%08x/%08x lock=%08x; classifier rejected=%d, no write\n",
			 observation.snapshot.bank0_map0, observation.snapshot.bank0_map1,
			 observation.snapshot.bank4_map0, observation.snapshot.bank4_map1,
			 observation.snapshot.lock, observation.classification);
	else
		dev_info(dev,
			 "MD1 remap observed bank0=%08x/%08x bank4=%08x/%08x lock=%08x; LK-shaped locked mode=%s, no ownership or write\n",
			 observation.snapshot.bank0_map0, observation.snapshot.bank0_map1,
			 observation.snapshot.bank4_map0, observation.snapshot.bank4_map1,
			 observation.snapshot.lock,
			 mt6797_md1_remap_mode_name(observation.attestation.mode));

	return 0;
}

static const struct of_device_id mt6797_md1_remap_observe_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-remap-observe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_remap_observe_of_match);

static struct platform_driver mt6797_md1_remap_observe_driver = {
	.probe = mt6797_md1_remap_observe_probe,
	.driver = {
		.name = "mt6797-md1-remap-observe",
		.of_match_table = mt6797_md1_remap_observe_of_match,
	},
};
module_platform_driver(mt6797_md1_remap_observe_driver);

MODULE_DESCRIPTION("Explicitly gated read-only MT6797 MD1 remap observer");
MODULE_LICENSE("GPL");
