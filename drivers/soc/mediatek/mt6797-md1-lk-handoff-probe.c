// SPDX-License-Identifier: GPL-2.0-only
/*
 * Separately-gated, read-only MT6797 MD1 LK hand-off probe.
 *
 * The MT6797 3.18 CCCI implementation consumes ccci,modem_info_v2 from
 * /chosen, maps the reported tag table, then obtains hdr_count, hdr_tbl_inf,
 * smem_layout, md1_chk and md1img from it:
 *
 * drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:295-355, 639-750,
 * 755-786, 845-889
 *
 * This clean-room probe is intentionally narrower. It maps only the LK table
 * after independently proving its address belongs to a no-map CCCI carve-out.
 * It reads and validates metadata, immediately unmaps it, and stops before
 * modem partition I/O, EMI MPU/remap, clocks, power, reset, CCIF, CLDMA, DMA,
 * interrupts, shared-memory writes, SIM, RF, or a modem boot request.
 */

#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/memremap.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6797-md1-firmware.h>
#include <linux/soc/mediatek/mt6797-md1-lk-handoff.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/unaligned.h>

#define MT6797_MD1_LK_HDR_COUNT_SIZE		4
#define MT6797_MD1_LK_MAX_MODEM_RECORDS	4

/*
 * LK rewrites Gemini's boot DT /reserved-memory before Linux sees it.  The
 * archived hardware FDT has exactly these three CCCI mblocks.  They are not
 * interchangeable with the build-time reserve-memory-ccci_* nodes: MD1 is a
 * sub-range of mblock-6, while mblock-7 is a distinct LK CCCI lease.
 */
#define MT6797_MD1_LIVE_CCCI_COMPAT		"mediatek,ccci"
#define MT6797_MD1_LIVE_SMEM_NODE		"mblock-5-ccci"
#define MT6797_MD1_LIVE_MD1_NODE		"mblock-6-ccci"
#define MT6797_MD1_LIVE_MD3_NODE		"mblock-7-ccci"
#define MT6797_MD1_LIVE_SMEM_BASE		0x88000000ULL
#define MT6797_MD1_LIVE_SMEM_SIZE		0x00600000ULL
#define MT6797_MD1_LK_IMAGE_BASE		0xb4000000ULL
#define MT6797_MD1_LK_IMAGE_SIZE		0x08000000ULL
#define MT6797_MD1_LIVE_MD1_BLOCK_BASE	0xb4000000ULL
#define MT6797_MD1_LIVE_MD1_BLOCK_SIZE	0x0a000000ULL
#define MT6797_MD1_LIVE_MD3_BLOCK_BASE	0xbe000000ULL
#define MT6797_MD1_LIVE_MD3_BLOCK_SIZE	0x00c00000ULL

static bool enable_experimental_md1_lk_handoff_probe;
module_param_named(enable_experimental_md1_lk_handoff_probe,
			   enable_experimental_md1_lk_handoff_probe, bool, 0400);
MODULE_PARM_DESC(enable_experimental_md1_lk_handoff_probe,
	"allow the explicitly gated, read-only MT6797 MD1 LK hand-off probe");

static bool mt6797_md1_lk_handoff_image_loaded_attested;
static DEFINE_MUTEX(mt6797_md1_lk_handoff_plan_lock);
static struct mt6797_md1_mpu_remap_plan mt6797_md1_lk_handoff_plan;
static bool mt6797_md1_lk_handoff_plan_attested;

bool mt6797_md1_lk_handoff_image_loaded(void)
{
	return READ_ONCE(mt6797_md1_lk_handoff_image_loaded_attested);
}
EXPORT_SYMBOL_GPL(mt6797_md1_lk_handoff_image_loaded);

int mt6797_md1_lk_handoff_plan_get(struct mt6797_md1_mpu_remap_plan *plan)
{
	if (!plan)
		return -EINVAL;

	mutex_lock(&mt6797_md1_lk_handoff_plan_lock);
	if (!mt6797_md1_lk_handoff_plan_attested) {
		mutex_unlock(&mt6797_md1_lk_handoff_plan_lock);
		return -ENODATA;
	}
	*plan = mt6797_md1_lk_handoff_plan;
	mutex_unlock(&mt6797_md1_lk_handoff_plan_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_lk_handoff_plan_get);

static bool mt6797_md1_phys_range_valid(u64 start, u64 size, u64 limit_start,
					u64 limit_size)
{
	u64 end, limit_end;

	return size && limit_size &&
		!check_add_overflow(start, size, &end) &&
		!check_add_overflow(limit_start, limit_size, &limit_end) &&
		start >= limit_start && end <= limit_end;
}

static bool mt6797_md1_phys_ranges_overlap(u64 first_start, u64 first_size,
					    u64 second_start, u64 second_size)
{
	u64 first_end, second_end;

	return first_size && second_size &&
		!check_add_overflow(first_start, first_size, &first_end) &&
		!check_add_overflow(second_start, second_size, &second_end) &&
		first_start < second_end && second_start < first_end;
}

struct mt6797_md1_live_ccci_mblock {
	const char *name;
	u64 base;
	u64 size;
	bool no_map;
};

static const struct mt6797_md1_live_ccci_mblock mt6797_md1_live_ccci_mblocks[] = {
	{ MT6797_MD1_LIVE_SMEM_NODE, MT6797_MD1_LIVE_SMEM_BASE,
	  MT6797_MD1_LIVE_SMEM_SIZE, true },
	{ MT6797_MD1_LIVE_MD1_NODE, MT6797_MD1_LIVE_MD1_BLOCK_BASE,
	  MT6797_MD1_LIVE_MD1_BLOCK_SIZE, false },
	{ MT6797_MD1_LIVE_MD3_NODE, MT6797_MD1_LIVE_MD3_BLOCK_BASE,
	  MT6797_MD1_LIVE_MD3_BLOCK_SIZE, false },
};

static int mt6797_md1_live_ccci_mblock_get(struct device *dev,
			const struct mt6797_md1_live_ccci_mblock *expected,
			struct device_node **result);

static bool mt6797_md1_live_ccci_compatible_exact(struct device_node *node)
{
	const char *compatible;

	return !of_property_read_string_index(node, "compatible", 0, &compatible) &&
		of_property_read_string_index(node, "compatible", 1, &compatible) &&
		!strcmp(compatible, MT6797_MD1_LIVE_CCCI_COMPAT);
}

static int mt6797_md1_require_live_ccci_table(struct device *dev, u64 start,
						       u64 size)
{
	struct device_node *node;
	struct resource resource;
	int ret;

	ret = mt6797_md1_live_ccci_mblock_get(dev,
		&mt6797_md1_live_ccci_mblocks[0], &node);
	if (ret)
		return ret;
	ret = of_address_to_resource(node, 0, &resource);
	if (!ret)
		ret = mt6797_md1_phys_range_valid(start, size, resource.start,
			resource_size(&resource)) ? 0 : -ERANGE;
	of_node_put(node);
	if (ret)
		return dev_err_probe(dev, ret,
			"LK table is outside the live mblock-5-ccci no-map lease\n");
	return 0;
}

static int mt6797_md1_live_ccci_mblock_get(struct device *dev,
			const struct mt6797_md1_live_ccci_mblock *expected,
			struct device_node **result)
{
	struct device_node *reserved, *node;
	struct resource resource;

	reserved = of_find_node_by_path("/reserved-memory");
	if (!reserved)
		return dev_err_probe(dev, -ENODEV, "missing /reserved-memory\n");
	node = of_get_child_by_name(reserved, expected->name);
	of_node_put(reserved);
	if (!node)
		return dev_err_probe(dev, -ENODEV,
			"missing LK live CCCI mblock %s\n", expected->name);
	if (strcmp(node->name, expected->name) ||
	    !mt6797_md1_live_ccci_compatible_exact(node) ||
	    of_property_read_bool(node, "no-map") != expected->no_map ||
	    of_address_to_resource(node, 0, &resource) ||
	    resource.start != expected->base ||
	    resource_size(&resource) != expected->size) {
		of_node_put(node);
		return dev_err_probe(dev, -ERANGE,
			"LK live CCCI mblock %s has invalid name/type/no-map/reg\n",
			expected->name);
	}
	*result = node;
	return 0;
}

static int mt6797_md1_validate_live_ccci_mblocks(struct device *dev,
						 const struct mt6797_md1_lk_modem_info *md1,
						 const struct mt6797_md1_lk_smem_layout *smem)
{
	struct device_node *reserved, *node, *live[ARRAY_SIZE(mt6797_md1_live_ccci_mblocks)];
	struct resource resource;
	unsigned int i, live_count = 0;
	int ret = 0;

	if (mt6797_md1_phys_ranges_overlap(md1->base_phys, md1->size,
					 smem->base_phys, smem->total_size))
		return dev_err_probe(dev, -ERANGE,
			"LK MD1 and shared-memory regions overlap\n");

	if (smem->base_phys != MT6797_MD1_LIVE_SMEM_BASE ||
	    smem->total_size != MT6797_MD1_LIVE_SMEM_SIZE ||
	    md1->base_phys != MT6797_MD1_LK_IMAGE_BASE ||
	    md1->size != MT6797_MD1_LK_IMAGE_SIZE ||
	    !mt6797_md1_phys_range_valid(md1->base_phys, md1->size,
		MT6797_MD1_LIVE_MD1_BLOCK_BASE,
		MT6797_MD1_LIVE_MD1_BLOCK_SIZE))
		return dev_err_probe(dev, -ERANGE,
			"LK ranges do not fit the authoritative live CCCI mblock contract\n");

	for (i = 0; i < ARRAY_SIZE(live); i++) {
		ret = mt6797_md1_live_ccci_mblock_get(dev,
				&mt6797_md1_live_ccci_mblocks[i], &live[i]);
		if (ret)
			goto out_put_live;
		if (i && mt6797_md1_phys_ranges_overlap(
				mt6797_md1_live_ccci_mblocks[i - 1].base,
				mt6797_md1_live_ccci_mblocks[i - 1].size,
				mt6797_md1_live_ccci_mblocks[i].base,
				mt6797_md1_live_ccci_mblocks[i].size)) {
			ret = dev_err_probe(dev, -ERANGE,
				"authoritative LK CCCI mblocks overlap\n");
			of_node_put(live[i]);
			goto out_put_live;
		}
	}

	reserved = of_find_node_by_path("/reserved-memory");
	if (!reserved) {
		ret = dev_err_probe(dev, -ENODEV, "missing /reserved-memory\n");
		goto out_put_live;
	}
	for_each_child_of_node(reserved, node) {
		if (of_device_is_compatible(node, MT6797_MD1_LIVE_CCCI_COMPAT)) {
			bool known = false;

			for (i = 0; i < ARRAY_SIZE(live); i++)
				known |= node == live[i];
			if (!known || live_count >= ARRAY_SIZE(live)) {
				ret = dev_err_probe(dev, -EEXIST,
					"ambiguous LK live CCCI mblock %pOF\n", node);
				of_node_put(node);
				goto out_put_reserved;
			}
			live_count++;
			continue;
		}
		if (of_device_is_compatible(node, "mediatek,reserve-memory-ccci_md1") ||
		    of_device_is_compatible(node, "mediatek,reserve-memory-ccci_share")) {
			ret = dev_err_probe(dev, -EEXIST,
				"ambiguous pre-LK CCCI reservation %pOF\n", node);
			of_node_put(node);
			goto out_put_reserved;
		}
		if (of_address_to_resource(node, 0, &resource))
			continue; /* Dynamic carve-outs are allocated around fixed leases. */
		if (mt6797_md1_phys_ranges_overlap(md1->base_phys, md1->size,
						 resource.start, resource_size(&resource)) ||
		    mt6797_md1_phys_ranges_overlap(smem->base_phys,
						 smem->total_size, resource.start,
						 resource_size(&resource))) {
			dev_err(dev,
				"LK CCCI lease overlaps reserved-memory node %pOF\n", node);
			ret = -ERANGE;
			of_node_put(node);
			goto out_put_reserved;
		}
	}
	if (live_count != ARRAY_SIZE(live)) {
		ret = dev_err_probe(dev, -EEXIST,
			"incomplete LK live CCCI mblock set\n");
		goto out_put_reserved;
	}
	out_put_reserved:
	of_node_put(reserved);
	out_put_live:
	while (i)
		of_node_put(live[--i]);
	return ret;
}


static int mt6797_md1_get_tag(const void *table, size_t table_size,
			      u32 tag_count, const char *name, const void **data,
			      u32 *size)
{
	struct mt6797_md1_lk_tag tag;
	int ret;

	ret = mt6797_md1_lk_tag_find_v2(table, table_size, tag_count, name, &tag);
	if (ret)
		return ret;
	*data = table + tag.data_offset;
	*size = tag.data_size;
	return 0;
}

static int mt6797_md1_lk_handoff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *chosen;
	struct mt6797_md1_lk_info lk_info;
	struct mt6797_md1_lk_modem_info md1;
	struct mt6797_md1_lk_smem_layout smem;
	enum mt6797_md1_lk_md1_load_evidence load_evidence;
	struct mt6797_md1_firmware_contract contract;
	struct mt6797_md1_firmware_info firmware;
	struct mt6797_md1_mpu_remap_layout plan_layout;
	struct mt6797_md1_mpu_remap_plan plan;
	const void *chosen_data;
	const void *tag_data;
	void *table;
	u32 raw_tag_size, tag_size, header_count, raw_image_size;
	int chosen_len;
	int ret;

	if (!READ_ONCE(enable_experimental_md1_lk_handoff_probe) ||
	    !device_property_read_bool(dev,
			"mediatek,experimental-md1-lk-handoff-probe") ||
	    !device_property_read_bool(dev, "mediatek,stage-lk-handoff-read-only"))
		return dev_err_probe(dev, -EPERM,
			"LK probe requires independent module and read-only DT gates\n");

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return dev_err_probe(dev, -ENODEV, "missing /chosen LK hand-off\n");
	chosen_data = of_get_property(chosen, "ccci,modem_info_v2", &chosen_len);
	if (!chosen_data) {
		of_node_put(chosen);
		return dev_err_probe(dev, -ENODATA,
			"missing ccci,modem_info_v2 LK hand-off\n");
	}
	ret = mt6797_md1_lk_info_parse_v2(chosen_data, chosen_len, &lk_info);
	of_node_put(chosen);
	if (ret)
		return dev_err_probe(dev, ret, "invalid ccci,modem_info_v2 hand-off\n");
	if (lk_info.error || lk_info.md_error[MT6797_MD1_LK_MD1_INDEX])
		return dev_err_probe(dev, -EIO,
			"LK reports global=%d MD1=%d; refusing metadata acquisition\n",
			lk_info.error, lk_info.md_error[MT6797_MD1_LK_MD1_INDEX]);

	ret = mt6797_md1_require_live_ccci_table(dev, lk_info.table_phys,
			lk_info.table_size);
	if (ret)
		return ret;
	table = memremap(lk_info.table_phys, lk_info.table_size, MEMREMAP_WB);
	if (!table)
		return dev_err_probe(dev, -ENOMEM, "could not map LK table read-only\n");

	ret = mt6797_md1_get_tag(table, lk_info.table_size, lk_info.tag_count,
				 "hdr_count", &tag_data, &tag_size);
	if (ret || tag_size != MT6797_MD1_LK_HDR_COUNT_SIZE) {
		ret = ret ?: -EMSGSIZE;
		dev_err(dev, "invalid hdr_count LK tag\n");
		goto out_unmap;
	}
	header_count = get_unaligned_le32(tag_data);
	if (!header_count || header_count > MT6797_MD1_LK_MAX_MODEM_RECORDS) {
		ret = -EINVAL;
		dev_err(dev, "invalid LK modem record count %u\n", header_count);
		goto out_unmap;
	}

	ret = mt6797_md1_get_tag(table, lk_info.table_size, lk_info.tag_count,
				 "hdr_tbl_inf", &tag_data, &tag_size);
	if (ret || tag_size != header_count * MT6797_MD1_LK_MODEM_INFO_SIZE) {
		ret = ret ?: -EMSGSIZE;
		dev_err(dev, "invalid hdr_tbl_inf LK tag\n");
		goto out_unmap;
	}
	ret = mt6797_md1_lk_modem_info_find(tag_data, tag_size,
					   MT6797_MD1_LK_MD1_INDEX, &md1);
	if (ret || md1.load_error) {
		ret = ret ?: -EIO;
		dev_err(dev, "MD1 memory record is absent or reports load error\n");
		goto out_unmap;
	}
	ret = mt6797_md1_lk_md1_load_evidence(&lk_info, &md1, &load_evidence);
	if (ret) {
		dev_err(dev,
			"LK MD1 image was not successfully loaded (flags=%#x evidence=%u); refusing plan\n",
			lk_info.load_flags, load_evidence);
		goto out_unmap;
	}
	ret = mt6797_md1_get_tag(table, lk_info.table_size, lk_info.tag_count,
				 "smem_layout", &tag_data, &tag_size);
	if (ret) {
		dev_err(dev, "missing smem_layout LK tag\n");
		goto out_unmap;
	}
	ret = mt6797_md1_lk_smem_layout_parse(tag_data, tag_size, &smem);
	if (ret) {
		dev_err(dev, "invalid smem_layout LK tag\n");
		goto out_unmap;
	}
	if (!mt6797_md1_phys_range_valid(lk_info.table_phys,
			lk_info.table_size, smem.base_phys, smem.total_size)) {
		ret = -ERANGE;
		dev_err(dev, "LK table is outside the reported shared-memory layout\n");
		goto out_unmap;
	}
	ret = mt6797_md1_validate_live_ccci_mblocks(dev, &md1, &smem);
	if (ret)
		goto out_unmap;

	ret = mt6797_md1_get_tag(table, lk_info.table_size, lk_info.tag_count,
				 "md1_chk", &tag_data, &tag_size);
	if (ret) {
		dev_err(dev, "missing md1_chk LK tag\n");
		goto out_unmap;
	}
	ret = mt6797_md1_get_tag(table, lk_info.table_size, lk_info.tag_count,
				 "md1img", &chosen_data, &raw_tag_size);
	if (ret || raw_tag_size != sizeof(u32)) {
		ret = ret ?: -EMSGSIZE;
		dev_err(dev, "invalid md1img LK tag\n");
		goto out_unmap;
	}
	raw_image_size = get_unaligned_le32(chosen_data);
	contract = (struct mt6797_md1_firmware_contract) {
		.md_reserved_size = md1.size,
		.ap_md_shared_reserved_size = smem.ap_md1_size,
		.md_to_md_shared_reserved_size = smem.md1_md3_size,
		.raw_image_size = raw_image_size,
	};
	ret = mt6797_md1_firmware_validate(tag_data, tag_size, &contract, &firmware);
	if (ret) {
		dev_err(dev, "md1_chk rejected against LK memory contract: %d\n", ret);
		goto out_unmap;
	}
	plan_layout = (struct mt6797_md1_mpu_remap_layout) {
		.md_base = md1.base_phys,
		.md_size = md1.size,
		.ap_md_smem_base = smem.base_phys + smem.ap_md1_offset,
		.ap_md_smem_size = smem.ap_md1_size,
		.lk_reported_load_complete = true,
	};
	ret = mt6797_md1_mpu_remap_plan_build(tag_data, tag_size, &contract,
						     &plan_layout, &plan);
	if (ret) {
		dev_err(dev,
			"source-derived MD1 RMPU/remap diagnostic plan rejected: %d; no hardware state changed\n",
			ret);
		goto out_unmap;
	}

	dev_info(dev,
		 "verified LK hand-off: table=%pa/0x%x MD1=%pa/0x%x smem=%pa/0x%x header=v%u platform=%s raw=0x%x; stopping before partition I/O, MPU/remap, power or reset\n",
		 &lk_info.table_phys, lk_info.table_size, &md1.base_phys, md1.size,
		 &smem.base_phys, smem.total_size, firmware.header_version,
		 firmware.platform, firmware.raw_image_size);
	dev_info(dev,
		 "LK MD1 load evidence: flags=%#x image-loaded=%u execution=unknown; this does not grant Linux cold-start, reset, power, remap, MPU, CCIF, clock, PLL or boot-vector ownership\n",
		 lk_info.load_flags,
		 load_evidence == MT6797_MD1_LK_MD1_LOAD_EXECUTION_UNKNOWN);
	dev_info(dev,
		 "RMPU/remap diagnostic only: LK load-complete=%u, bank4=%pa offset=0x%llx; no MPU/remap register is owned or written\n",
		 plan.lk_reported_load_complete, &plan.md_bank4_base,
		 plan.smem_offset_ap_to_md);
	dev_info(dev,
		 "RMPU final regions: 11=%pa-%pa/%06x 12=%pa-%pa/%06x 13=%pa-%pa/%06x 10=%pa-%pa/%06x 14=%pa-%pa/%06x smem7=%pa-%pa/%06x\n",
		 &plan.mpu[0].start, &plan.mpu[0].end, plan.mpu[0].access_permission,
		 &plan.mpu[1].start, &plan.mpu[1].end, plan.mpu[1].access_permission,
		 &plan.mpu[2].start, &plan.mpu[2].end, plan.mpu[2].access_permission,
		 &plan.mpu[3].start, &plan.mpu[3].end, plan.mpu[3].access_permission,
		 &plan.mpu[4].start, &plan.mpu[4].end, plan.mpu[4].access_permission,
		 &plan.mpu[5].start, &plan.mpu[5].end, plan.mpu[5].access_permission);
	dev_info(dev,
		 "RMPU/remap candidate encodings: phys32 bank0=%08x/%08x bank4=%08x/%08x; kernel-emi-40000000 bank0=%08x/%08x bank4=%08x/%08x; mode unresolved, no write\n",
		 plan.remap[MT6797_MD1_REMAP_PHYS32].bank0_map0,
		 plan.remap[MT6797_MD1_REMAP_PHYS32].bank0_map1,
		 plan.remap[MT6797_MD1_REMAP_PHYS32].bank4_map0,
		 plan.remap[MT6797_MD1_REMAP_PHYS32].bank4_map1,
		 plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank0_map0,
		 plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank0_map1,
		 plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank4_map0,
		 plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank4_map1);
	/* Publish only the already-validated diagnostic plan, never an authority. */
	mutex_lock(&mt6797_md1_lk_handoff_plan_lock);
	mt6797_md1_lk_handoff_plan = plan;
	mt6797_md1_lk_handoff_plan_attested = true;
	mutex_unlock(&mt6797_md1_lk_handoff_plan_lock);
	/* This remains only a verified image-load attestation; execution is unknown. */
	WRITE_ONCE(mt6797_md1_lk_handoff_image_loaded_attested, true);
out_unmap:
	memunmap(table);
	return ret;
}

static const struct of_device_id mt6797_md1_lk_handoff_of_match[] = {
	{ .compatible = "mediatek,mt6797-md1-lk-handoff-probe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_md1_lk_handoff_of_match);

static struct platform_driver mt6797_md1_lk_handoff_driver = {
	.probe = mt6797_md1_lk_handoff_probe,
	.driver = {
		.name = "mt6797-md1-lk-handoff-probe",
		.of_match_table = mt6797_md1_lk_handoff_of_match,
	},
};
module_platform_driver(mt6797_md1_lk_handoff_driver);

MODULE_DESCRIPTION("Fail-closed, read-only MT6797 MD1 LK hand-off probe");
MODULE_LICENSE("GPL");
