// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 RMPU/remap diagnostic plan. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define MD1_PLAN_MD_SIZE	0x0a000000
#define MD1_PLAN_SHARED_SIZE	0x00600000
#define MD1_PLAN_RAW_SIZE	0x00100000

static const struct mt6797_md1_firmware_contract mt6797_md1_plan_contract = {
	.md_reserved_size = MD1_PLAN_MD_SIZE,
	.ap_md_shared_reserved_size = MD1_PLAN_SHARED_SIZE,
	.md_to_md_shared_reserved_size = MD1_PLAN_SHARED_SIZE,
	.raw_image_size = MD1_PLAN_RAW_SIZE,
};

static const struct mt6797_md1_mpu_remap_layout mt6797_md1_plan_layout = {
	.md_base = 0xb4000000,
	.md_size = MD1_PLAN_MD_SIZE,
	.ap_md_smem_base = 0x88000000,
	.ap_md_smem_size = MD1_PLAN_SHARED_SIZE,
	.lk_reported_load_complete = true,
};

static void mt6797_md1_plan_make_header(u8 *header)
{
	u8 region;

	memset(header, 0, MT6797_MD1_CHECK_HEADER_V6_SIZE);
	memcpy(header, MT6797_MD1_CHECK_HEADER_MAGIC,
	       MT6797_MD1_CHECK_HEADER_MAGIC_SIZE);
	put_unaligned_le32(6, header + MT6797_MD1_CHECK_HEADER_VERSION_OFFSET);
	put_unaligned_le32(2, header + MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	put_unaligned_le32(9, header + MT6797_MD1_CHECK_HEADER_IMAGE_TYPE_OFFSET);
	header[MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET] = 1;
	put_unaligned_le32(MD1_PLAN_MD_SIZE,
			   header + MT6797_MD1_CHECK_HEADER_MEM_SIZE_OFFSET);
	put_unaligned_le32(MD1_PLAN_RAW_SIZE,
			   header + MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET);
	put_unaligned_le32(MT6797_MD1_DSP_ON_EMI_DISABLED,
			   header + MT6797_MD1_CHECK_HEADER_DSP_OFFSET);
	put_unaligned_le32(5, header + MT6797_MD1_CHECK_HEADER_REGION_COUNT_OFFSET);
	for (region = 0; region < MT6797_MD1_MPU_PLAN_HEADER_ENTRIES; region++) {
		put_unaligned_le32(region * MD1_PLAN_RAW_SIZE, header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			region * MT6797_MD1_CHECK_HEADER_REGION_STRIDE);
		put_unaligned_le32(MD1_PLAN_RAW_SIZE, header +
			MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
			region * MT6797_MD1_CHECK_HEADER_REGION_STRIDE + sizeof(u32));
	}
	/* Invalid nibbles select the vendor default-permission table. */
	put_unaligned_le32(0xffffffff,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET);
	put_unaligned_le32(0xffffffff,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 4);
	put_unaligned_le32(0xffffffff,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 8);
	put_unaligned_le32(0x00100000,
			   header + MT6797_MD1_CHECK_HEADER_V6_AP_SMEM_OFFSET);
	put_unaligned_le32(0x00200000,
			   header + MT6797_MD1_CHECK_HEADER_V6_MD_TO_MD_SMEM_OFFSET);
	put_unaligned_le32(MT6797_MD1_CHECK_HEADER_V6_SIZE,
			   header + MT6797_MD1_CHECK_HEADER_V6_SIZE - sizeof(u32));
}

static int mt6797_md1_plan_build(const void *header,
				 struct mt6797_md1_mpu_remap_plan *plan)
{
	return mt6797_md1_mpu_remap_plan_build(header,
		MT6797_MD1_CHECK_HEADER_V6_SIZE, &mt6797_md1_plan_contract,
		&mt6797_md1_plan_layout, plan);
}

static void mt6797_md1_plan_builds_final_vendor_targets(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_mpu_remap_plan plan;

	mt6797_md1_plan_make_header(header);
	/* Valid nibbles override AP, MD1 and MDHW only for header region zero. */
	put_unaligned_le32(0xfffffff1,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET);
	put_unaligned_le32(0xfffffff2,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 4);
	put_unaligned_le32(0xfffffff3,
			   header + MT6797_MD1_CHECK_HEADER_DOMAIN_ATTR_OFFSET + 8);

	KUNIT_ASSERT_EQ(test, mt6797_md1_plan_build(header, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.mpu[0].emi_mpu_region, 11);
	KUNIT_EXPECT_EQ(test, plan.mpu[1].emi_mpu_region, 12);
	KUNIT_EXPECT_EQ(test, plan.mpu[2].emi_mpu_region, 13);
	KUNIT_EXPECT_EQ(test, plan.mpu[3].emi_mpu_region, 10);
	KUNIT_EXPECT_EQ(test, plan.mpu[4].emi_mpu_region, 14);
	KUNIT_EXPECT_EQ(test, plan.mpu[0].start, 0xb4000000ULL);
	KUNIT_EXPECT_EQ(test, plan.mpu[0].end, 0xb40fffffULL);
	KUNIT_EXPECT_EQ(test, plan.mpu[0].access_permission, 0x76db51U);
	KUNIT_EXPECT_EQ(test, plan.mpu[MT6797_MD1_MPU_SHARED_ENTRY].emi_mpu_region,
			7);
	KUNIT_EXPECT_EQ(test, plan.mpu[MT6797_MD1_MPU_SHARED_ENTRY].start,
			0x88000000ULL);
	KUNIT_EXPECT_EQ(test, plan.mpu[MT6797_MD1_MPU_SHARED_ENTRY].end,
			0x885fffffULL);
	KUNIT_EXPECT_EQ(test,
		plan.mpu[MT6797_MD1_MPU_SHARED_ENTRY].access_permission, 0xb6d140U);
	KUNIT_EXPECT_EQ(test, plan.md_bank4_base, 0x88000000ULL);
	KUNIT_EXPECT_EQ(test, plan.smem_offset_ap_to_md, 0x48000000ULL);
	KUNIT_EXPECT_EQ(test, plan.remap[MT6797_MD1_REMAP_PHYS32].bank0_map0,
			0xbbb9b7b5U);
	KUNIT_EXPECT_EQ(test, plan.remap[MT6797_MD1_REMAP_PHYS32].bank0_map1,
			0xc3c1bfbdU);
	KUNIT_EXPECT_EQ(test, plan.remap[MT6797_MD1_REMAP_PHYS32].bank4_map0,
			0x8f8d8b89U);
	KUNIT_EXPECT_EQ(test, plan.remap[MT6797_MD1_REMAP_PHYS32].bank4_map1,
			0x97959391U);
	KUNIT_EXPECT_EQ(test,
		plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank0_map0,
		0x7b797775U);
	KUNIT_EXPECT_EQ(test,
		plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank0_map1,
		0x83817f7dU);
	KUNIT_EXPECT_EQ(test,
		plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank4_map0,
		0x4f4d4b49U);
	KUNIT_EXPECT_EQ(test,
		plan.remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000].bank4_map1,
		0x57555351U);
}

static void mt6797_md1_plan_rejects_empty_header_region_atomically(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_mpu_remap_plan original;

	mt6797_md1_plan_make_header(header);
	put_unaligned_le32(0, header + MT6797_MD1_CHECK_HEADER_REGION_OFFSET +
		2 * MT6797_MD1_CHECK_HEADER_REGION_STRIDE + sizeof(u32));
	memset(&plan, 0xa5, sizeof(plan));
	original = plan;
	KUNIT_EXPECT_EQ(test, mt6797_md1_plan_build(header, &plan), -ENODATA);
	KUNIT_EXPECT_EQ(test, memcmp(&plan, &original, sizeof(plan)), 0);
}

static void mt6797_md1_plan_rejects_remap_window_outside_32bit(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_mpu_remap_layout layout = mt6797_md1_plan_layout;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_mpu_remap_plan original;

	mt6797_md1_plan_make_header(header);
	layout.md_base = 0xf2000000;
	memset(&plan, 0x5a, sizeof(plan));
	original = plan;
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_mpu_remap_plan_build(header, sizeof(header),
			&mt6797_md1_plan_contract, &layout, &plan), -ERANGE);
	KUNIT_EXPECT_EQ(test, memcmp(&plan, &original, sizeof(plan)), 0);
}

static struct kunit_case mt6797_md1_mpu_remap_plan_test_cases[] = {
	KUNIT_CASE(mt6797_md1_plan_builds_final_vendor_targets),
	KUNIT_CASE(mt6797_md1_plan_rejects_empty_header_region_atomically),
	KUNIT_CASE(mt6797_md1_plan_rejects_remap_window_outside_32bit),
	{}
};

static struct kunit_suite mt6797_md1_mpu_remap_plan_test_suite = {
	.name = "mt6797-md1-mpu-remap-plan",
	.test_cases = mt6797_md1_mpu_remap_plan_test_cases,
};

kunit_test_suite(mt6797_md1_mpu_remap_plan_test_suite);

MODULE_LICENSE("GPL");
