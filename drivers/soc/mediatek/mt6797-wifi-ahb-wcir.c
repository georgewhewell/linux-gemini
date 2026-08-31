// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 WLAN GEN3 AHB SDIO-like WCIR observer.
 *
 * MT6797's WLAN HIF is a command portal, not a memory-mapped WLAN register
 * file. The vendor HIF therefore enables SDIO function 1 and sends CMD53
 * through offsets 0 and 0x1000 before it reads WCIR. Keep that protocol here;
 * a direct readl(HIF + WCIR) would read the command setup register instead.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <linux/soc/mediatek/mt6797-wifi-ahb.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>

static DEFINE_MUTEX(mt6797_wifi_ahb_portal_lock);
static bool enable_experimental_wifi_wcir_observe;
module_param_named(enable_experimental_wifi_wcir_observe,
		   enable_experimental_wifi_wcir_observe, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_wcir_observe,
		 "allow one MT6797 post-patch WLAN WCIR observation");

int mt6797_wifi_ahb_wcir_decode(u32 raw, struct mt6797_wifi_ahb_wcir *wcir)
{
	if (!wcir)
		return -EINVAL;

	wcir->raw = raw;
	wcir->chip_id = raw & GENMASK(15, 0);
	wcir->revision = FIELD_GET(GENMASK(19, 16), raw);
	wcir->por_indicator = !!(raw & BIT(20));
	wcir->wlan_ready = !!(raw & BIT(21));

	if (!wcir->chip_id || wcir->chip_id == U16_MAX)
		return -ENODEV;
	/* Vendor ahb.c maps raw Everest 0x0279 to logical MT6797 0x6797. */
	if (wcir->chip_id != 0x0279 || wcir->revision != 0)
		return -EPROTO;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_wcir_decode);

bool mt6797_wifi_ahb_wcir_read_permitted(const struct mt6797_wifi_ahb_wcir_guard *guard)
{
	return guard && guard->full_postpatch_session && guard->wifi_paldo_held &&
		guard->hif_owner && guard->hif_mapping;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_wcir_read_permitted);

static int mt6797_wifi_ahb_cmd52_read(void __iomem *portal, u32 address,
				      u8 *data)
{
	u32 command;
	int ret;

	if (!data)
		return -EINVAL;
	ret = mt6797_wifi_ahb_cmd52_pack(false, 0, address, 0, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	*data = readl(portal + MT6797_WIFI_AHB_CMD52_DATA);
	return 0;
}

static int mt6797_wifi_ahb_cmd52_write(void __iomem *portal, u32 address,
				       u8 data)
{
	u32 command;
	int ret;

	ret = mt6797_wifi_ahb_cmd52_pack(true, 0, address, data, &command);
	if (ret)
		return ret;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	writel(data, portal + MT6797_WIFI_AHB_CMD52_DATA);
	return 0;
}

static int mt6797_wifi_ahb_enable_wifi_function(void __iomem *portal,
						u8 *ioex_before, bool *enabled)
{
	u8 iorx;
	int ret;

	if (!ioex_before || !enabled)
		return -EINVAL;
	ret = mt6797_wifi_ahb_cmd52_read(portal, MT6797_WIFI_AHB_SDIO_CCCR_IOEX,
					 ioex_before);
	if (ret)
		return ret;
	if (*ioex_before & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI))
		return -EBUSY;
	ret = mt6797_wifi_ahb_cmd52_write(portal, MT6797_WIFI_AHB_SDIO_CCCR_IOEX,
					  *ioex_before |
					 BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI));
	if (ret)
		return ret;
	*enabled = true;
	ret = mt6797_wifi_ahb_cmd52_read(portal, MT6797_WIFI_AHB_SDIO_CCCR_IORX,
					 &iorx);
	if (ret)
		return ret;
	if (!(iorx & BIT(MT6797_WIFI_AHB_SDIO_FUNC_WIFI)))
		return -ETIMEDOUT;
	return 0;
}

static int mt6797_wifi_ahb_set_block_size(void __iomem *portal, u8 *low_before,
					  u8 *high_before, bool *changed)
{
	int ret;

	if (!low_before || !high_before || !changed)
		return -EINVAL;
	ret = mt6797_wifi_ahb_cmd52_read(portal,
					 MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
	if (ret)
		return ret;
	ret = mt6797_wifi_ahb_cmd52_read(portal,
					 MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
	if (ret)
		return ret;
	if (*low_before != 0) {
		ret = mt6797_wifi_ahb_cmd52_write(portal,
						  MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, 0);
		if (ret)
			return ret;
		*changed = true;
	}
	if (*high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8) {
		ret = mt6797_wifi_ahb_cmd52_write(portal,
						  MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1,
			MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8);
		if (ret)
			return ret;
		*changed = true;
	}
	return 0;
}

static void mt6797_wifi_ahb_restore_block_size(void __iomem *portal,
					       u8 low_before, u8 high_before)
{
	/* The portal has no error response; preserve the vendor's write semantics. */
	if (high_before != MT6797_WIFI_AHB_SDIO_BLOCK_SIZE >> 8)
		mt6797_wifi_ahb_cmd52_write(portal,
					    MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE + 1, high_before);
	if (low_before)
		mt6797_wifi_ahb_cmd52_write(portal,
					    MT6797_WIFI_AHB_SDIO_FBR1_BLKSIZE, low_before);
}

int mt6797_wifi_ahb_wcir_observe(struct device *owner,
				 const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease,
				  struct regulator *wifi_paldo,
				  struct mt6797_wifi_ahb_wcir *wcir)
{
	struct device_node *wifi_np;
	struct resource hif;
	struct mt6797_wifi_ahb_wcir_guard guard = { };
	void __iomem *portal;
	u8 ioex_before, block_low_before, block_high_before;
	u32 command;
	bool function_enabled = false, block_size_set = false;
	int ret;

	if (!owner || !lease || !wifi_paldo || !wcir)
		return -EINVAL;
	/* The opaque lease is a live stage capability, not just a non-NULL token. */
	if (!mt6797_btif_stp_bootstrap_full_wmt_lease_active(lease))
		return -EPERM;
	if (!READ_ONCE(enable_experimental_wifi_wcir_observe) ||
	    !device_property_read_bool(owner, "mediatek,experimental-wifi-wcir-observe"))
		return -EPERM;
	guard.full_postpatch_session = true;

	ret = regulator_is_enabled(wifi_paldo);
	if (ret < 0)
		return ret;
	if (!ret)
		return -EHOSTDOWN;
	guard.wifi_paldo_held = true;

	wifi_np = of_find_compatible_node(NULL, NULL, "mediatek,WIFI");
	if (!wifi_np)
		return -ENODEV;
	ret = of_address_to_resource(wifi_np, 0, &hif);
	of_node_put(wifi_np);
	if (ret)
		return ret;
	if (hif.start != MT6797_WIFI_AHB_HIF_BASE ||
	    resource_size(&hif) < MT6797_WIFI_AHB_HIF_PORTAL_SIZE)
		return -EINVAL;
	if (!request_mem_region(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE,
				"mt6797-wifi-wcir"))
		return -EBUSY;
	guard.hif_owner = true;

	portal = ioremap(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
	if (!portal) {
		ret = -ENOMEM;
		goto release_region;
	}
	guard.hif_mapping = true;
	if (!mt6797_wifi_ahb_wcir_read_permitted(&guard)) {
		ret = -EPERM;
		goto unmap;
	}

	mutex_lock(&mt6797_wifi_ahb_portal_lock);
	ret = mt6797_wifi_ahb_enable_wifi_function(portal, &ioex_before,
						   &function_enabled);
	if (ret)
		goto disable_function;
	ret = mt6797_wifi_ahb_set_block_size(portal, &block_low_before,
					     &block_high_before, &block_size_set);
	if (ret)
		goto disable_function;
	ret = mt6797_wifi_ahb_cmd53_read_pack(MT6797_WIFI_AHB_WCIR_OFFSET, 4,
					      &command);
	if (ret)
		goto restore_block_size;
	writel(command, portal + MT6797_WIFI_AHB_CMD_SETUP);
	command = readl(portal + MT6797_WIFI_AHB_CMD53_DATA);
	ret = mt6797_wifi_ahb_wcir_decode(command, wcir);
restore_block_size:
	if (block_size_set)
		mt6797_wifi_ahb_restore_block_size(portal, block_low_before,
						   block_high_before);
disable_function:
	if (function_enabled)
		mt6797_wifi_ahb_cmd52_write(portal, MT6797_WIFI_AHB_SDIO_CCCR_IOEX,
					    ioex_before);
	mutex_unlock(&mt6797_wifi_ahb_portal_lock);
unmap:
	iounmap(portal);
release_region:
	release_mem_region(hif.start, MT6797_WIFI_AHB_HIF_PORTAL_SIZE);
	if (ret) {
		if (ret == -ENODEV)
			dev_err(owner, "WCIR invalid raw=%#08x\n", wcir->raw);
		else if (ret == -EPROTO)
			dev_err(owner, "WCIR unexpected raw=%#08x; expected Everest 0x0279 rev0\n",
				wcir->raw);
		return ret;
	}

	/* Wi-Fi RAM firmware is deliberately not loaded by the G2d milestone. */
	if (wcir->wlan_ready) {
		dev_err(owner, "WCIR raw=%#08x chip=%#06x rev=%u por=%u ready=1\n",
			wcir->raw, wcir->chip_id, wcir->revision, wcir->por_indicator);
		return -EUCLEAN;
	}
	dev_info(owner, "WCIR raw=%#08x chip=%#06x rev=%u por=%u ready=0\n",
		 wcir->raw, wcir->chip_id, wcir->revision, wcir->por_indicator);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_wcir_observe);

MODULE_DESCRIPTION("MT6797 post-patch WLAN GEN3 AHB SDIO-like WCIR observer");
MODULE_LICENSE("GPL");
