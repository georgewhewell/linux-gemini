// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 MD1 RMPU/remap diagnostic plan.
 *
 * The MT6797 vendor CCCI implementation describes MD1 header regions
 * 0..4 as EMI regions 11, 12, 13, 10 and 14, and the AP/MD shared-memory
 * region as EMI region 7.  Its staged first pass temporarily uses a special
 * region-2 permission, then the second pass applies the check-header value.
 * This library records the complete target only.  The separate fence applies
 * its initial and stage-one subsets in vendor order.  It intentionally performs no
 * MMIO and cannot be used to release or start MD1.
 *
 * Source contract:
 *   gemini-linux-kernel-3.18/drivers/misc/mediatek/eccci/mt6797/
 *     ccci_platform.c:71-79, 127-178, 380-415, 487-497, 507-614, 666-838
 *   gemini-linux-kernel-3.18/drivers/misc/mediatek/include/mt-plat/
 *     mt6797/include/mach/emi_mpu.h:103-109, 278-283
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define MT6797_MD1_MPU_GRANULE				0x10000ULL
#define MT6797_MD1_KERNEL_EMI_BASE			0x40000000ULL

enum mt6797_md1_emi_permission {
	MT6797_MD1_EMI_NO_PROTECTION = 0,
	MT6797_MD1_EMI_SEC_R_NSEC_R = 4,
	MT6797_MD1_EMI_FORBIDDEN = 5,
};

static const u8 mt6797_md1_header_to_emi_region[] = {
	11, 12, 13, 10, 14,
};

/* D0(AP) through D7(MDHW), copied from vendor MPU_ATTR_DEFAULT. */
static const u8 mt6797_md1_default_permissions[][8] = {
	/* check-header region 0 -> EMI region 11 */
	{ 4, 4, 5, 5, 5, 5, 5, 4 },
	/* check-header region 1 -> EMI region 12 */
	{ 4, 0, 5, 5, 5, 5, 5, 4 },
	/* check-header region 2 -> EMI region 13 */
	{ 4, 4, 5, 5, 5, 5, 5, 0 },
	/* check-header region 3 -> EMI region 10 */
	{ 4, 0, 5, 5, 5, 5, 5, 0 },
	/* check-header region 4 -> EMI region 14 */
	{ 4, 0, 5, 5, 5, 5, 5, 4 },
};

static u32 mt6797_md1_pack_permissions(const u8 domains[8])
{
	return ((u32)domains[7] << 21) | ((u32)domains[6] << 18) |
		((u32)domains[5] << 15) | ((u32)domains[4] << 12) |
		((u32)domains[3] << 9) | ((u32)domains[2] << 6) |
		((u32)domains[1] << 3) | domains[0];
}

static u32 mt6797_md1_header_permission(const u8 *header, u8 region)
{
	u8 domains[8];
	u32 value;

	memcpy(domains, mt6797_md1_default_permissions[region], sizeof(domains));

	/*
	 * The vendor parser takes only AP(D0), MD1(D1), and MDHW(D7) from
	 * domain_attr[0], [1], and [2], respectively.  Any value above
	 * FORBIDDEN falls back to the corresponding default.
	 */
	value = (get_unaligned_le32(header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET) >>
		 (region * 4)) & 0xf;
	if (value <= MT6797_MD1_EMI_FORBIDDEN)
		domains[0] = value;
	value = (get_unaligned_le32(header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 4) >>
		 (region * 4)) & 0xf;
	if (value <= MT6797_MD1_EMI_FORBIDDEN)
		domains[1] = value;
	value = (get_unaligned_le32(header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 8) >>
		 (region * 4)) & 0xf;
	if (value <= MT6797_MD1_EMI_FORBIDDEN)
		domains[7] = value;

	return mt6797_md1_pack_permissions(domains);
}

static int mt6797_md1_plan_region(u64 base, u32 limit, u32 offset, u32 size,
				  struct mt6797_md1_mpu_plan_entry *entry)
{
	u32 end_offset;
	u64 start, end, rounded_end;

	/* The vendor subtracts one after rounding; never model an empty range. */
	if (!size)
		return -ENODATA;
	if (check_add_overflow(offset, size, &end_offset) || end_offset > limit)
		return -ERANGE;
	if (check_add_overflow(base, (u64)offset, &start) ||
	    check_add_overflow(start, (u64)size, &end) ||
	    check_add_overflow(end, MT6797_MD1_MPU_GRANULE - 1, &rounded_end))
		return -EOVERFLOW;

	entry->start = start;
	entry->end = round_down(rounded_end, MT6797_MD1_MPU_GRANULE) - 1;
	return 0;
}

static int mt6797_md1_remap_pair(u64 des, u32 *map0, u32 *map1)
{
	u64 last;
	u32 first, second;

	if (check_add_overflow(des, MT6797_MD1_REMAP_WINDOW_SIZE - 1, &last) ||
	    last > U32_MAX)
		return -ERANGE;

	first = (((des >> 24) | 0x1) & 0xff) |
		((((des + MT6797_MD1_REMAP_SLOT_SIZE) >> 16) | BIT(8)) & 0xff00) |
		((((des + 2 * MT6797_MD1_REMAP_SLOT_SIZE) >> 8) | BIT(16)) & 0xff0000) |
		((((des + 3 * MT6797_MD1_REMAP_SLOT_SIZE) >> 0) | BIT(24)) & 0xff000000);
	second = ((((des + 4 * MT6797_MD1_REMAP_SLOT_SIZE) >> 24) | 0x1) & 0xff) |
		((((des + 5 * MT6797_MD1_REMAP_SLOT_SIZE) >> 16) | BIT(8)) & 0xff00) |
		((((des + 6 * MT6797_MD1_REMAP_SLOT_SIZE) >> 8) | BIT(16)) & 0xff0000) |
		((((des + 7 * MT6797_MD1_REMAP_SLOT_SIZE) >> 0) | BIT(24)) & 0xff000000);
	*map0 = first;
	*map1 = second;
	return 0;
}

static int mt6797_md1_remap_values(u64 md_base, u64 smem_base,
				   struct mt6797_md1_remap_values *values)
{
	int ret;

	ret = mt6797_md1_remap_pair(md_base, &values->bank0_map0,
				    &values->bank0_map1);
	if (ret)
		return ret;
	return mt6797_md1_remap_pair(smem_base, &values->bank4_map0,
				    &values->bank4_map1);
}

int mt6797_md1_mpu_remap_plan_build(const void *data, size_t header_size,
	const struct mt6797_md1_firmware_contract *contract,
	const struct mt6797_md1_mpu_remap_layout *layout,
	struct mt6797_md1_mpu_remap_plan *plan)
{
	const u8 *header = data;
	struct mt6797_md1_mpu_remap_plan result;
	u64 bank4_base;
	u8 region;
	int ret;

	if (!header || !contract || !layout || !plan || !layout->md_base ||
	    !layout->md_size || !layout->ap_md_smem_base ||
	    !layout->ap_md_smem_size || layout->md_size != contract->md_reserved_size ||
	    layout->ap_md_smem_size != contract->ap_md_shared_reserved_size ||
	    layout->md_base < MT6797_MD1_KERNEL_EMI_BASE ||
	    (layout->md_base & (MT6797_MD1_MPU_GRANULE - 1)) ||
	    (layout->ap_md_smem_base & (MT6797_MD1_MPU_GRANULE - 1)))
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	ret = mt6797_md1_firmware_validate(header, header_size, contract,
					    &result.firmware);
	if (ret)
		return ret;

	for (region = 0; region < MT6797_MD1_MPU_PLAN_HEADER_ENTRIES; region++) {
		struct mt6797_md1_mpu_plan_entry *entry = &result.mpu[region];
		u32 offset = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			region * MT6797_MD1_CHECK_HEADER_REGION_STRIDE);
		u32 size = get_unaligned_le32(header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			region * MT6797_MD1_CHECK_HEADER_REGION_STRIDE + sizeof(u32));

		ret = mt6797_md1_plan_region(layout->md_base, layout->md_size,
					     offset, size, entry);
		if (ret)
			return ret;
		entry->emi_mpu_region = mt6797_md1_header_to_emi_region[region];
		entry->check_header_region = region;
		entry->access_permission = mt6797_md1_header_permission(header, region);
	}

	ret = mt6797_md1_plan_region(layout->ap_md_smem_base,
				     layout->ap_md_smem_size, 0,
				     layout->ap_md_smem_size,
				     &result.mpu[MT6797_MD1_MPU_SHARED_ENTRY]);
	if (ret)
		return ret;
	result.mpu[MT6797_MD1_MPU_SHARED_ENTRY].emi_mpu_region = 7;
	result.mpu[MT6797_MD1_MPU_SHARED_ENTRY].check_header_region = U8_MAX;
	result.mpu[MT6797_MD1_MPU_SHARED_ENTRY].access_permission =
		mt6797_md1_pack_permissions((const u8[]) { 0, 0, 5, 0, 5, 5, 5, 5 });

	bank4_base = round_down(layout->ap_md_smem_base, MT6797_MD1_REMAP_SLOT_SIZE);
	if (bank4_base < MT6797_MD1_KERNEL_EMI_BASE)
		return -ERANGE;
	result.md_bank4_base = bank4_base;
	result.smem_offset_ap_to_md = bank4_base - MT6797_MD1_KERNEL_EMI_BASE;

	ret = mt6797_md1_remap_values(layout->md_base, bank4_base,
				       &result.remap[MT6797_MD1_REMAP_PHYS32]);
	if (ret)
		return ret;
	ret = mt6797_md1_remap_values(layout->md_base - MT6797_MD1_KERNEL_EMI_BASE,
				       bank4_base - MT6797_MD1_KERNEL_EMI_BASE,
				       &result.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000]);
	if (ret)
		return ret;

	result.lk_reported_load_complete = layout->lk_reported_load_complete;
	*plan = result;
	return 0;
}
