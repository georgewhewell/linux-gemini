// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free tests for the MT6797 MD1 firmware hand-off validator. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/mediatek/mt6797-md1-firmware.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define MD1_TEST_MD_RESERVATION	0x0a100000
#define MD1_TEST_SHARED_RESERVATION	0x00600000
#define MD1_TEST_RAW_IMAGE_SIZE	0x01000000

static const struct mt6797_md1_firmware_contract mt6797_md1_test_contract = {
	.md_reserved_size = MD1_TEST_MD_RESERVATION,
	.ap_md_shared_reserved_size = MD1_TEST_SHARED_RESERVATION,
	.md_to_md_shared_reserved_size = MD1_TEST_SHARED_RESERVATION,
	.raw_image_size = MD1_TEST_RAW_IMAGE_SIZE,
};

static int mt6797_md1_test_validate(const void *header, size_t header_size,
				    struct mt6797_md1_firmware_info *info)
{
	return mt6797_md1_firmware_validate(header, header_size,
					    &mt6797_md1_test_contract, info);
}

static void mt6797_md1_test_make_header(u8 *header, u32 version, u32 size)
{
	memset(header, 0, size);
	memcpy(header, MT6797_MD1_CHECK_HEADER_MAGIC,
	       MT6797_MD1_CHECK_HEADER_MAGIC_SIZE);
	put_unaligned_le32(version, header + MT6797_MD1_CHECK_HEADER_VERSION_OFFSET);
	put_unaligned_le32(2, header + MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	put_unaligned_le32(9, header + MT6797_MD1_CHECK_HEADER_IMAGE_TYPE_OFFSET);
	memcpy(header + MT6797_MD1_CHECK_HEADER_PLATFORM_OFFSET, "MT6797E1", 8);
	header[MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET] = 1;
	put_unaligned_le32(MD1_TEST_MD_RESERVATION,
			   header + MT6797_MD1_CHECK_HEADER_MEM_SIZE_OFFSET);
	put_unaligned_le32(MD1_TEST_RAW_IMAGE_SIZE,
			   header + MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET);
	put_unaligned_le32(MT6797_MD1_DSP_ON_EMI_DISABLED,
			   header + MT6797_MD1_CHECK_HEADER_DSP_OFFSET);
	put_unaligned_le32(0, header + MT6797_MD1_CHECK_HEADER_DSP_SIZE_OFFSET);
	put_unaligned_le32(1, header + MT6797_MD1_CHECK_HEADER_REGION_COUNT_OFFSET);
	put_unaligned_le32(0, header + MT6797_MD1_CHECK_HEADER_REGION_OFFSET);
	put_unaligned_le32(0x10000, header +
			   MT6797_MD1_CHECK_HEADER_REGION_OFFSET + sizeof(u32));
	if (version >= 5) {
		put_unaligned_le32(0x200000,
				   header + MT6797_MD1_CHECK_HEADER_ARM7_OFFSET);
		put_unaligned_le32(0x100000,
				   header + MT6797_MD1_CHECK_HEADER_ARM7_SIZE_OFFSET);
	}
	if (version == 6) {
		put_unaligned_le32(0x100000,
				   header + MT6797_MD1_CHECK_HEADER_V6_AP_SMEM_OFFSET);
		put_unaligned_le32(0x200000, header +
				   MT6797_MD1_CHECK_HEADER_V6_MD_TO_MD_SMEM_OFFSET);
	}
	put_unaligned_le32(size, header + size - sizeof(u32));
}

static void mt6797_md1_test_valid_v6(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	KUNIT_ASSERT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), 0);
	KUNIT_EXPECT_EQ(test, info.header_version, 6U);
	KUNIT_EXPECT_EQ(test, info.raw_image_size, MD1_TEST_RAW_IMAGE_SIZE);
	KUNIT_EXPECT_STREQ(test, info.platform, "MT6797E1");
	KUNIT_EXPECT_FALSE(test, info.dsp_on_emi);
	KUNIT_EXPECT_TRUE(test, info.has_arm7);
	KUNIT_EXPECT_TRUE(test, info.has_v6_shared_memory);
}

static void mt6797_md1_test_rejects_non_md1_header(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V34_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 4, sizeof(header));
	header[MT6797_MD1_CHECK_HEADER_BIND_SYS_OFFSET] = 3;
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -EXDEV);
}

static void mt6797_md1_test_rejects_wrong_wire_size(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header,
			MT6797_MD1_CHECK_HEADER_V5_SIZE, &info), -EMSGSIZE);
}

static void mt6797_md1_test_rejects_unsupported_version(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	put_unaligned_le32(7, header + MT6797_MD1_CHECK_HEADER_VERSION_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info),
		-EOPNOTSUPP);
}

static void mt6797_md1_test_rejects_bad_trailing_size(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	put_unaligned_le32(sizeof(header) - 4,
			   header + sizeof(header) - sizeof(u32));
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -EBADMSG);
}

static void mt6797_md1_test_rejects_invalid_product_and_image_type(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	put_unaligned_le32(3, header + MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -EINVAL);
	put_unaligned_le32(2, header + MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	put_unaligned_le32(23, header + MT6797_MD1_CHECK_HEADER_IMAGE_TYPE_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -EINVAL);
}

static void mt6797_md1_test_rejects_oversized_shared_memory(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V6_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 6, sizeof(header));
	put_unaligned_le32(MD1_TEST_SHARED_RESERVATION + 1,
			   header + MT6797_MD1_CHECK_HEADER_V6_AP_SMEM_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -ERANGE);
}

static void mt6797_md1_test_does_not_mutate_output_on_failure(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V34_SIZE];
	struct mt6797_md1_firmware_info info;
	struct mt6797_md1_firmware_info original;

	mt6797_md1_test_make_header(header, 4, sizeof(header));
	memset(&info, 0xa5, sizeof(info));
	original = info;
	put_unaligned_le32(3, header + MT6797_MD1_CHECK_HEADER_PRODUCT_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(&info, &original, sizeof(info)), 0);
}

static void mt6797_md1_test_rejects_overflowed_region(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V34_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 4, sizeof(header));
	put_unaligned_le32(0xfffffff0,
			   header + MT6797_MD1_CHECK_HEADER_REGION_OFFSET);
	put_unaligned_le32(0x40, header +
			   MT6797_MD1_CHECK_HEADER_REGION_OFFSET + sizeof(u32));
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -ERANGE);
}

static void mt6797_md1_test_rejects_unbound_image_size(struct kunit *test)
{
	u8 header[MT6797_MD1_CHECK_HEADER_V34_SIZE];
	struct mt6797_md1_firmware_info info;

	mt6797_md1_test_make_header(header, 4, sizeof(header));
	put_unaligned_le32(MD1_TEST_RAW_IMAGE_SIZE - 4,
			   header + MT6797_MD1_CHECK_HEADER_IMAGE_SIZE_OFFSET);
	KUNIT_EXPECT_EQ(test,
		mt6797_md1_test_validate(header, sizeof(header), &info), -ERANGE);
}

static struct kunit_case mt6797_md1_firmware_test_cases[] = {
	KUNIT_CASE(mt6797_md1_test_valid_v6),
	KUNIT_CASE(mt6797_md1_test_rejects_non_md1_header),
	KUNIT_CASE(mt6797_md1_test_rejects_wrong_wire_size),
	KUNIT_CASE(mt6797_md1_test_rejects_unsupported_version),
	KUNIT_CASE(mt6797_md1_test_rejects_bad_trailing_size),
	KUNIT_CASE(mt6797_md1_test_rejects_invalid_product_and_image_type),
	KUNIT_CASE(mt6797_md1_test_rejects_oversized_shared_memory),
	KUNIT_CASE(mt6797_md1_test_does_not_mutate_output_on_failure),
	KUNIT_CASE(mt6797_md1_test_rejects_overflowed_region),
	KUNIT_CASE(mt6797_md1_test_rejects_unbound_image_size),
	{}
};

static struct kunit_suite mt6797_md1_firmware_test_suite = {
	.name = "mt6797-md1-firmware",
	.test_cases = mt6797_md1_firmware_test_cases,
};

kunit_test_suite(mt6797_md1_firmware_test_suite);

MODULE_LICENSE("GPL");
