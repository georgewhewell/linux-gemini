// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 MD1 request_firmware split-image loader. */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/soc/mediatek/mt6797-md1-firmware-loader.h>

struct mt6797_md1_aux_image {
	const struct firmware *firmware;
	size_t payload_offset;
	size_t payload_size;
};

static void mt6797_md1_aux_release(struct mt6797_md1_aux_image *image)
{
	if (image->firmware)
		release_firmware(image->firmware);
	memset(image, 0, sizeof(*image));
}

static int mt6797_md1_aux_request(struct device *dev, const char *name,
				  u32 expected_size,
				  struct mt6797_md1_aux_image *image)
{
	int ret;

	if (!name || !expected_size)
		return -EINVAL;
	ret = request_firmware(&image->firmware, name, dev);
	if (ret)
		return ret;
	ret = mt6797_md1_firmware_payload(image->firmware->data,
		image->firmware->size, &image->payload_offset, &image->payload_size);
	if (ret)
		goto out_release;
	/* Vendor accepts an auxiliary up to its reserved CHECK_HEADER extent. */
	if (image->payload_size > expected_size) {
		ret = -EMSGSIZE;
		goto out_release;
	}
	return 0;

out_release:
	mt6797_md1_aux_release(image);
	return ret;
}

int mt6797_md1_firmware_load(struct device *dev,
	const struct mt6797_md1_firmware_names *names,
	const struct mt6797_md1_mpu_remap_layout *layout,
	const struct mt6797_md1_mpu_remap_plan *lk_plan,
	mt6797_md1_firmware_cold_verify_fn verify_cold, void *verify_context,
	struct mt6797_md1_mpu_remap_plan *plan)
{
	struct mt6797_md1_firmware_contract contract;
	struct mt6797_md1_firmware_image md_image;
	struct mt6797_md1_aux_image dsp = {}, arm7 = {};
	const struct firmware *md = NULL;
	struct mt6797_md1_mpu_remap_layout loaded_layout;
	void __iomem *memory = NULL;
	const u8 *header;
	int ret;

	if (!dev || !names || !names->md || !layout || !lk_plan || !verify_cold ||
	    !plan ||
	    !layout->md_base || !layout->md_size)
		return -EINVAL;

	contract = (struct mt6797_md1_firmware_contract) {
		.md_reserved_size = layout->md_size,
		.ap_md_shared_reserved_size = layout->ap_md_smem_size,
		.md_to_md_shared_reserved_size =
			lk_plan->firmware.md_to_md_smem_size,
	};
	ret = request_firmware(&md, names->md, dev);
	if (ret)
		return ret;
	ret = mt6797_md1_firmware_image_validate(md->data, md->size, &contract,
						 &md_image);
	if (ret)
		goto out;
	if (md_image.payload_size > layout->md_size) {
		ret = -EFBIG;
		goto out;
	}
	if (md_image.info.dsp_on_emi && md_image.info.dsp_size) {
		ret = mt6797_md1_aux_request(dev, names->dsp, md_image.info.dsp_size,
					     &dsp);
		if (ret)
			goto out;
	}
	if (md_image.info.has_arm7) {
		ret = mt6797_md1_aux_request(dev, names->arm7,
					     md_image.info.arm7_size, &arm7);
		if (ret)
			goto out;
	}
	/* A split image must not overwrite the validated MD image or its peer. */
	if ((dsp.firmware && md_image.info.dsp_offset < md_image.payload_size) ||
	    (arm7.firmware && md_image.info.arm7_offset < md_image.payload_size) ||
	    (dsp.firmware && arm7.firmware &&
	     md_image.info.dsp_offset < md_image.info.arm7_offset + arm7.payload_size &&
	     md_image.info.arm7_offset < md_image.info.dsp_offset + dsp.payload_size)) {
		ret = -EINVAL;
		goto out;
	}

	header = md->data + md_image.check_header_offset;
	loaded_layout = *layout;
	ret = mt6797_md1_mpu_remap_plan_build(header,
		md_image.info.header_size, &(struct mt6797_md1_firmware_contract) {
			.md_reserved_size = contract.md_reserved_size,
			.ap_md_shared_reserved_size = contract.ap_md_shared_reserved_size,
			.md_to_md_shared_reserved_size =
				contract.md_to_md_shared_reserved_size,
			.raw_image_size = md_image.info.raw_image_size,
		}, &loaded_layout, plan);
	if (ret)
		goto out;
	/* Firmware acquisition can sleep: prove reset-held state again after it. */
	ret = verify_cold(verify_context);
	if (ret)
		goto out;

	memory = ioremap_wc(layout->md_base, layout->md_size);
	if (!memory) {
		ret = -ENOMEM;
		goto out;
	}
	/* Vendor first boot clears MD memory before copying the split images. */
	memset_io(memory, 0, layout->md_size);
	memcpy_toio(memory, md->data + md_image.payload_offset,
		    md_image.payload_size);
	if (dsp.firmware)
		memcpy_toio(memory + md_image.info.dsp_offset,
			dsp.firmware->data + dsp.payload_offset, dsp.payload_size);
	if (arm7.firmware)
		memcpy_toio(memory + md_image.info.arm7_offset,
			arm7.firmware->data + arm7.payload_offset, arm7.payload_size);
	/* Complete every firmware write before later MPU/power/vector operations. */
	wmb();
	dev_info(dev,
		 "loaded validated MD1 firmware %s (%u bytes, CHECK_HEADER v%u)%s%s\n",
		 names->md, md_image.info.raw_image_size,
		 md_image.info.header_version, dsp.firmware ? ", DSP" : "",
		 arm7.firmware ? ", ARM7" : "");
	ret = 0;
out:
	if (memory)
		iounmap(memory);
	mt6797_md1_aux_release(&arm7);
	mt6797_md1_aux_release(&dsp);
	if (md)
		release_firmware(md);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_firmware_load);
