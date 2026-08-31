// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 WLAN GEN3 AHB W1 firmware identity preflight.
 *
 * Vendor wlanAdapterStart() disables WLAN interrupts, resets init TX
 * resources, transfers WIFI_RAM_CODE_6797, sends the Wi-Fi start command, and
 * then polls WCIR.WLAN_READY.  See vendor gen3/common/wlan_lib.c:407-510.
 * This bounded W1 step stops before that irreversible stream/IRQ boundary:
 * it proves that exactly the reviewed image is present while the synchronous
 * full-WMT capability is live.  It creates no netdev and sends no WLAN data.
 */

#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb.h>

#define MT6797_WIFI_W1_FIRMWARE_NAME "WIFI_RAM_CODE_6797"
#define MT6797_WIFI_W1_FIRMWARE_SIZE 411632U

static const u8 mt6797_wifi_w1_firmware_sha256[] = {
	0xa6, 0x93, 0x83, 0xd7, 0x4d, 0x82, 0x94, 0x30,
	0x48, 0x7c, 0x39, 0xee, 0xf6, 0xb5, 0xe2, 0x81,
	0xb2, 0x5f, 0x90, 0x15, 0x95, 0xc9, 0x03, 0xa6,
	0x32, 0xa1, 0x0a, 0xa8, 0x63, 0x14, 0x26, 0xdd,
};

static bool enable_experimental_wifi_w1_firmware_preflight;
module_param_named(enable_experimental_wifi_w1_firmware_preflight,
		   enable_experimental_wifi_w1_firmware_preflight, bool, 0400);
MODULE_PARM_DESC(enable_experimental_wifi_w1_firmware_preflight,
		 "allow MT6797 WLAN W1 pinned firmware identity preflight only");

bool mt6797_wifi_ahb_w1_firmware_enabled(struct device *owner)
{
	return owner && READ_ONCE(enable_experimental_wifi_w1_firmware_preflight) &&
	       device_property_read_bool(owner,
				 "mediatek,experimental-wifi-w1-firmware-preflight");
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_enabled);

static int mt6797_wifi_w1_digest(const u8 *data, size_t size, u8 *digest)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	int ret;

	if (!data || !digest)
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		ret = -ENOMEM;
		goto free_tfm;
	}
	desc->tfm = tfm;
	ret = crypto_shash_digest(desc, data, size, digest);
	kfree(desc);
free_tfm:
	crypto_free_shash(tfm);
	return ret;
}

int mt6797_wifi_ahb_w1_firmware_validate(const u8 *data, size_t size,
					 size_t expected_size,
					 const u8 *expected_digest)
{
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	if (!data || !expected_digest || !expected_size)
		return -EINVAL;
	if (size != expected_size)
		return -EFBIG;
	ret = mt6797_wifi_w1_digest(data, size, digest);
	if (ret)
		return ret;
	return memcmp(digest, expected_digest, sizeof(digest)) ? -EBADMSG : 0;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_validate);

int mt6797_wifi_ahb_w1_firmware_preflight(struct device *owner,
			  const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	const struct firmware *firmware;
	int ret;

	if (!owner || !lease)
		return -EINVAL;
	if (!mt6797_btif_stp_bootstrap_full_wmt_lease_active(lease))
		return -ESHUTDOWN;
	if (!mt6797_wifi_ahb_w1_firmware_enabled(owner))
		return -EPERM;
	ret = request_firmware(&firmware, MT6797_WIFI_W1_FIRMWARE_NAME, owner);
	if (ret) {
		dev_err(owner, "W1 firmware request %s failed: %d\n",
			MT6797_WIFI_W1_FIRMWARE_NAME, ret);
		return ret;
	}
	ret = mt6797_wifi_ahb_w1_firmware_validate(firmware->data,
					      firmware->size,
					      MT6797_WIFI_W1_FIRMWARE_SIZE,
					      mt6797_wifi_w1_firmware_sha256);
	if (ret) {
		if (ret == -EFBIG)
			dev_err(owner, "W1 firmware size=%zu expected=%u\n",
				firmware->size, MT6797_WIFI_W1_FIRMWARE_SIZE);
		else if (ret == -EBADMSG)
			dev_err(owner, "W1 firmware SHA-256 mismatch\n");
		goto release;
	}
	dev_info(owner,
		 "W1 firmware verified: %s bytes=%zu sha256=%*phN; HIF stream/IRQ/netdev fenced\n",
		 MT6797_WIFI_W1_FIRMWARE_NAME, firmware->size,
		 (int)sizeof(mt6797_wifi_w1_firmware_sha256),
		 mt6797_wifi_w1_firmware_sha256);
	ret = 0;
release:
	release_firmware(firmware);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_wifi_ahb_w1_firmware_preflight);

MODULE_DESCRIPTION("MT6797 WLAN GEN3 AHB W1 pinned firmware preflight");
MODULE_LICENSE("GPL");
