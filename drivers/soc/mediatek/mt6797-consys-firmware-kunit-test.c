// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coverage for MT6797 CONSYS firmware metadata validation. */

#include <linux/errno.h>
#include <linux/string.h>
#include <kunit/test.h>

#include <linux/soc/mediatek/mt6797-consys-firmware.h>

static const u8 mt6797_consys_test_address[] = { 0xde, 0xad, 0xbe, 0xef };

#define MT6797_CONSYS_TEST_PATCH_SIZE	(MT6797_CONSYS_PATCH_HEADER_SIZE + 1)

static void mt6797_consys_test_patch_data(u8 patch[MT6797_CONSYS_TEST_PATCH_SIZE])
{
	memcpy(patch, "20180307b001005", 15);
	patch[15] = '\0';
	memcpy(patch + 16, "GEMI", 4);
	patch[20] = 0;
	patch[21] = 0x8a;
	patch[22] = 0;
	patch[23] = 0x8a;
	patch[24] = 0xfa;
	patch[25] = 0;
	patch[26] = 0;
	patch[27] = 0;
	patch[28] = 'X';
}

static void mt6797_consys_test_manifest(
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE],
	struct mt6797_consys_patch_image patches[2],
		struct mt6797_consys_firmware_manifest *manifest)
{
	mt6797_consys_test_patch_data(patch_data);
	patches[0].blob.name = "ROMv3_patch_1_1_hdr.bin";
	patches[0].blob.data = patch_data;
	patches[0].blob.size = MT6797_CONSYS_TEST_PATCH_SIZE;
	patches[0].load_address = mt6797_consys_test_address;
	patches[1] = patches[0];
	patches[1].blob.name = "ROMv3_patch_1_0_hdr.bin";

	*manifest = (struct mt6797_consys_firmware_manifest) {
		.patches = patches,
		.patch_count = 2,
	};
}

static void mt6797_consys_test_accepts_observed_shape(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_manifest manifest;
	struct mt6797_consys_patch_info info;

	mt6797_consys_test_manifest(patch_data, patches, &manifest);
	KUNIT_ASSERT_EQ(test, mt6797_consys_firmware_manifest_validate(&manifest),
			0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_patch_validate(&patches[0], &info), 0);
	KUNIT_EXPECT_MEMEQ(test, info.build_time, "20180307b001005\0", 16);
	KUNIT_EXPECT_MEMEQ(test, info.platform, "GEMI", 4);
	KUNIT_EXPECT_EQ(test, info.hw_version, (u16)0x8a00);
	KUNIT_EXPECT_EQ(test, info.sw_version, (u16)0x8a00);
	KUNIT_EXPECT_EQ(test, info.patch_version, 0xfaU);
	KUNIT_EXPECT_EQ(test, info.body_size, (size_t)1);
	KUNIT_EXPECT_MEMEQ(test, info.load_address, mt6797_consys_test_address,
				  sizeof(info.load_address));
}

static void mt6797_consys_test_rejects_missing_address_and_short_patch(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_manifest manifest;

	mt6797_consys_test_manifest(patch_data, patches, &manifest);
	patches[0].load_address = NULL;
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_validate(&manifest),
			-ENODATA);

	patches[0].load_address = mt6797_consys_test_address;
	patches[0].blob.size = MT6797_CONSYS_PATCH_HEADER_SIZE;
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_validate(&manifest),
			-EMSGSIZE);
}

static void mt6797_consys_test_rejects_ambiguous_or_noncanonical_manifest(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_manifest manifest;

	mt6797_consys_test_manifest(patch_data, patches, &manifest);
	patches[1].blob.name = patches[0].blob.name;
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_validate(&manifest),
			-EEXIST);

	mt6797_consys_test_manifest(patch_data, patches, &manifest);
	patches[0].blob.name = "vendor/ROMv3_patch_1_1_hdr.bin";
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_validate(&manifest),
			-EINVAL);

}

static struct kunit_case mt6797_consys_firmware_test_cases[] = {
	KUNIT_CASE(mt6797_consys_test_accepts_observed_shape),
	KUNIT_CASE(mt6797_consys_test_rejects_missing_address_and_short_patch),
	KUNIT_CASE(mt6797_consys_test_rejects_ambiguous_or_noncanonical_manifest),
	{ }
};

static struct kunit_suite mt6797_consys_firmware_test_suite = {
	.name = "mt6797-consys-firmware",
	.test_cases = mt6797_consys_firmware_test_cases,
};

kunit_test_suite(mt6797_consys_firmware_test_suite);

MODULE_LICENSE("GPL");
