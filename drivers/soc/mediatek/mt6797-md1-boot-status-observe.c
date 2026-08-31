// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, explicitly gated MT6797 MD1 boot-status observation.
 *
 * The MT6797 vendor CCCI driver reads MD1_CFG_BOOT_STATS0/1 from the AP
 * infrastructure view when reporting modem boot status.  It maps the two
 * words at 0x10201300, makes two dummy reads, and reports the third:
 *
 * drivers/misc/mediatek/eccci/mt6797/modem_reg_base.h:70-72
 * drivers/misc/mediatek/eccci/mt6797/cldma_platform.c:360-378
 *
 * This is deliberately narrower than a CCCI/CLDMA driver.  It reads only the
 * vendor-diagnostic AP/MD CCIF BUSY/ACK words and the AP receive-channel word
 * after status sampling; it never maps CLDMA, CCIF SRAM, CCCI carve-outs or
 * transport state. There are no writes, clock/power/reset/IRQ/DMA requests,
 * firmware or shared-memory I/O.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6797-md1-boot-status-observe.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>

#define MT6797_MD1_BOOT_STATUS_BASE	0x10201300
#define MT6797_MD1_BOOT_STATUS_SIZE	0x8
#define MT6797_MD1_BOOT_STATUS0		0x0
#define MT6797_MD1_BOOT_STATUS1		0x4
#define MT6797_MD1_BOOT_STATUS_SAMPLES	3
#define MT6797_MD1_AP_CCIF_BASE		0x10209000
#define MT6797_MD1_MD_CCIF_BASE		0x1020a000
#define MT6797_MD1_CCIF_SIZE			0x1000
#define MT6797_MD1_CCIF_BUSY			0x4
#define MT6797_MD1_CCIF_RCHNUM			0x10
#define MT6797_MD1_CCIF_ACK			0x14

static bool enable_experimental_boot_status;
module_param_named(enable_experimental_boot_status,
			   enable_experimental_boot_status, bool, 0400);
MODULE_PARM_DESC(enable_experimental_boot_status,
		 "allow the one-shot MT6797 MD1 boot-status observation (default: false)");

static DEFINE_MUTEX(mt6797_md1_boot_status_evidence_lock);
static struct mt6797_md1_boot_status_evidence mt6797_md1_boot_status_evidence;

int mt6797_md1_boot_status_evidence_get(
	struct mt6797_md1_boot_status_evidence *evidence)
{
	if (!evidence)
		return -EINVAL;

	mutex_lock(&mt6797_md1_boot_status_evidence_lock);
	*evidence = mt6797_md1_boot_status_evidence;
	mutex_unlock(&mt6797_md1_boot_status_evidence_lock);
	return evidence->observed ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(mt6797_md1_boot_status_evidence_get);

static int mt6797_md1_boot_status_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct resource *res;
	const struct resource *ap_ccif_res, *md_ccif_res;
	void __iomem *base, *ap_ccif, *md_ccif;
	struct mt6797_md1_mpu_remap_plan plan;
	u32 stats0[MT6797_MD1_BOOT_STATUS_SAMPLES];
	u32 stats1[MT6797_MD1_BOOT_STATUS_SAMPLES];
	u32 ap_busy, ap_rchnum, ap_ack, md_busy, md_ack;
	unsigned int i;
	int ret;

	if (!READ_ONCE(enable_experimental_boot_status) ||
	    !device_property_read_bool(dev,
				       "mediatek,experimental-md1-boot-status"))
		return dev_err_probe(dev, -EPERM,
			"refusing MD1 boot-status observation without both explicit gates\n");
	/*
	 * This observer is meaningful only after the LK hand-off probe has verified
	 * the native v2 table, image/load evidence and live CCCI mblock contract.
	 * The plan getter is an attestation boundary, not permission for control.
	 */
	ret = mt6797_md1_lk_handoff_plan_get(&plan);
	if (ret == -ENODATA)
		return dev_err_probe(dev, -EPROBE_DEFER,
			"waiting for read-only LK hand-off preflight publication\n");
	if (ret)
		return dev_err_probe(dev, ret,
			"MD1 boot-status rejected by LK hand-off preflight\n");
	if (!mt6797_md1_lk_handoff_image_loaded())
		return dev_err_probe(dev, -ENODATA,
			"MD1 boot-status requires completed read-only LK hand-off preflight\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "md1-boot-status");
	if (!res)
		return dev_err_probe(dev, -EINVAL,
			"missing md1-boot-status resource\n");

	if (res->start != MT6797_MD1_BOOT_STATUS_BASE ||
	    resource_size(res) != MT6797_MD1_BOOT_STATUS_SIZE)
		return dev_err_probe(dev, -EINVAL,
			"md1-boot-status must be %08x/%#x\n",
			MT6797_MD1_BOOT_STATUS_BASE, MT6797_MD1_BOOT_STATUS_SIZE);

	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/*
	 * This fixed sequence matches the vendor's two dummy reads and one report
	 * read per word.  It has no delay, poll, retry, or write path.  A printk
	 * immediately before every read leaves a serial/log boundary if a read
	 * itself wedges the fabric; software cannot recover from such a wedge.
	 */
	for (i = 0; i < MT6797_MD1_BOOT_STATUS_SAMPLES; i++) {
		dev_info(dev, "before MD1_BOOT_STATS0 sample %u/%u\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES);
		stats0[i] = readl(base + MT6797_MD1_BOOT_STATUS0);
		dev_info(dev, "after MD1_BOOT_STATS0 sample %u/%u = %#08x\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES, stats0[i]);
	}

	for (i = 0; i < MT6797_MD1_BOOT_STATUS_SAMPLES; i++) {
		dev_info(dev, "before MD1_BOOT_STATS1 sample %u/%u\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES);
		stats1[i] = readl(base + MT6797_MD1_BOOT_STATUS1);
		dev_info(dev, "after MD1_BOOT_STATS1 sample %u/%u = %#08x\n", i + 1,
			 MT6797_MD1_BOOT_STATUS_SAMPLES, stats1[i]);
	}

	dev_info(dev,
		 "MD1 boot-status complete: stats0=%#08x/%#08x/%#08x stats1=%#08x/%#08x/%#08x; no writes\n",
		 stats0[0], stats0[1], stats0[2], stats1[0], stats1[1], stats1[2]);
	/* Publish only after every one of the fixed read-only samples completed. */
	mutex_lock(&mt6797_md1_boot_status_evidence_lock);
	mt6797_md1_boot_status_evidence = (struct mt6797_md1_boot_status_evidence) {
		.stats0_samples = MT6797_MD1_BOOT_STATUS_SAMPLES,
		.stats1_samples = MT6797_MD1_BOOT_STATUS_SAMPLES,
		.stats0_final = stats0[MT6797_MD1_BOOT_STATUS_SAMPLES - 1],
		.stats1_final = stats1[MT6797_MD1_BOOT_STATUS_SAMPLES - 1],
		.observed = true,
	};
	mutex_unlock(&mt6797_md1_boot_status_evidence_lock);

	/*
	 * Vendor CCCI's diagnostic dump reads these AP/MD CCIF registers
	 * (modem_cldma.c:3158-3186).  Its CCIF interrupt path treats AP_RCHNUM as
	 * the received channel bitmap before ACKing it (modem_cldma.c:1940-1948).
	 * This snapshot deliberately keeps that value raw: it does not ACK, request
	 * the IRQ, decode a channel, or access CHDATA/CLDMA state.
	 */
	ap_ccif_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ap-ccif");
	md_ccif_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "md-ccif");
	if (!ap_ccif_res || !md_ccif_res ||
	    ap_ccif_res->start != MT6797_MD1_AP_CCIF_BASE ||
	    md_ccif_res->start != MT6797_MD1_MD_CCIF_BASE ||
	    resource_size(ap_ccif_res) != MT6797_MD1_CCIF_SIZE ||
	    resource_size(md_ccif_res) != MT6797_MD1_CCIF_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid read-only CCIF resources\n");
	ap_ccif = devm_ioremap_resource(dev, ap_ccif_res);
	md_ccif = devm_ioremap_resource(dev, md_ccif_res);
	if (IS_ERR(ap_ccif) || IS_ERR(md_ccif))
		return PTR_ERR(IS_ERR(ap_ccif) ? ap_ccif : md_ccif);
	dev_info(dev, "before passive MD1 CCIF snapshot\n");
	ap_busy = readl(ap_ccif + MT6797_MD1_CCIF_BUSY);
	ap_rchnum = readl(ap_ccif + MT6797_MD1_CCIF_RCHNUM);
	ap_ack = readl(ap_ccif + MT6797_MD1_CCIF_ACK);
	md_busy = readl(md_ccif + MT6797_MD1_CCIF_BUSY);
	md_ack = readl(md_ccif + MT6797_MD1_CCIF_ACK);
	dev_info(dev,
		 "passive MD1 CCIF snapshot: ap_busy=%#08x ap_rchnum=%#08x ap_ack=%#08x md_busy=%#08x md_ack=%#08x; interpretation=raw-only writes=none\n",
		 ap_busy, ap_rchnum, ap_ack, md_busy, md_ack);
	return 0;
}

static const struct of_device_id mt6797_md1_boot_status_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-boot-status-observe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_boot_status_of_match);

static struct platform_driver mt6797_md1_boot_status_driver = {
	.probe = mt6797_md1_boot_status_probe,
	.driver = {
		.name = "mt6797-md1-boot-status-observe",
		.of_match_table = mt6797_md1_boot_status_of_match,
	},
};
module_platform_driver(mt6797_md1_boot_status_driver);

MODULE_DESCRIPTION("Explicitly gated MT6797 MD1 boot-status observer");
MODULE_LICENSE("GPL");
