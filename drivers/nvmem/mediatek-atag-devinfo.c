// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 LK devinfo thermal calibration provider.
 *
 * The retained MT6797 LK appends an opaque, little-endian 103-word property
 * at /chosen/atag,devinfo.  Only the three words consumed by the MT6797
 * thermal extractor are retained here; the efuse MMIO block is never mapped.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define MT6797_ATAG_DEVINFO_TAG		0x41000804
#define MT6797_ATAG_HEADER_WORDS	2
#define MT6797_ATAG_PAYLOAD_WORDS	100
#define MT6797_ATAG_TRAILER_WORDS	1
#define MT6797_ATAG_TOTAL_WORDS		(MT6797_ATAG_HEADER_WORDS + \
					 MT6797_ATAG_PAYLOAD_WORDS + \
					 MT6797_ATAG_TRAILER_WORDS)
#define MT6797_THERMAL_CELL_BYTES	(3 * sizeof(u32))

struct mt6797_atag_devinfo {
	u8 calibration[MT6797_THERMAL_CELL_BYTES];
};

static const unsigned int mt6797_thermal_word_order[] = { 32, 31, 33 };

static u32 mt6797_atag_word(const u8 *property, unsigned int index)
{
	return get_unaligned_le32(property + index * sizeof(u32));
}

static int mt6797_atag_parse(struct mt6797_atag_devinfo *data)
{
	struct device_node *chosen;
	const u8 *property;
	int length;
	int i;
	int ret = 0;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;

	property = of_get_property(chosen, "atag,devinfo", &length);
	if (!property) {
		ret = -ENODEV;
		goto out;
	}

	if (length != MT6797_ATAG_TOTAL_WORDS * sizeof(u32) ||
	    mt6797_atag_word(property, 0) != MT6797_ATAG_TOTAL_WORDS ||
	    mt6797_atag_word(property, 1) != MT6797_ATAG_DEVINFO_TAG ||
	    mt6797_atag_word(property, MT6797_ATAG_HEADER_WORDS +
					 MT6797_ATAG_PAYLOAD_WORDS) !=
					MT6797_ATAG_PAYLOAD_WORDS) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(mt6797_thermal_word_order); i++) {
		u32 value = mt6797_atag_word(property,
					MT6797_ATAG_HEADER_WORDS +
					mt6797_thermal_word_order[i]);

		put_unaligned_le32(value, &data->calibration[i * sizeof(u32)]);
	}

out:
	of_node_put(chosen);
	return ret;
}

static int mt6797_atag_read(void *context, unsigned int offset,
				void *value, size_t bytes)
{
	struct mt6797_atag_devinfo *data = context;

	if (offset > sizeof(data->calibration) ||
	    bytes > sizeof(data->calibration) - offset)
		return -EINVAL;

	memcpy(value, data->calibration + offset, bytes);
	return 0;
}

static int mt6797_atag_devinfo_probe(struct platform_device *pdev)
{
	struct mt6797_atag_devinfo *data;
	struct nvmem_config config = {
		.dev = &pdev->dev,
		.name = "mt6797-atag-calibration",
		.owner = THIS_MODULE,
		.add_legacy_fixed_of_cells = true,
		.read_only = true,
		.root_only = true,
		.reg_read = mt6797_atag_read,
		.size = MT6797_THERMAL_CELL_BYTES,
		.word_size = sizeof(u32),
		.stride = sizeof(u32),
	};
	struct nvmem_device *nvmem;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = mt6797_atag_parse(data);
	if (ret)
		return ret;

	config.priv = data;
	nvmem = devm_nvmem_register(&pdev->dev, &config);
	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id mt6797_atag_devinfo_of_match[] = {
	{ .compatible = "mediatek,mt6797-atag-devinfo" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_atag_devinfo_of_match);

static struct platform_driver mt6797_atag_devinfo_driver = {
	.probe = mt6797_atag_devinfo_probe,
	.driver = {
		.name = "mediatek-mt6797-atag-devinfo",
		.of_match_table = mt6797_atag_devinfo_of_match,
	},
};
module_platform_driver(mt6797_atag_devinfo_driver);

MODULE_DESCRIPTION("MediaTek MT6797 LK devinfo calibration provider");
MODULE_LICENSE("GPL");
