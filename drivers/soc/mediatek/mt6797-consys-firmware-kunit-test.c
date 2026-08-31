// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coverage for the MT6797 CONSYS firmware preflight gate. */

#include <linux/errno.h>
#include <linux/string.h>
#include <kunit/test.h>

#include <linux/soc/mediatek/mt6797-consys-firmware.h>

static const u8 mt6797_consys_test_config[] = "co_clock_flag=0\n";
static const u8 mt6797_consys_test_config_sha256[] = {
	0x7f, 0x3f, 0x8e, 0x28, 0x76, 0x8a, 0xe7, 0x68,
	0x18, 0xbd, 0x28, 0x57, 0x05, 0x10, 0xf9, 0x72,
	0x5b, 0xc2, 0x83, 0x52, 0xeb, 0x8a, 0x46, 0x04,
	0xcb, 0xbb, 0xb6, 0x79, 0x50, 0x07, 0x95, 0xb3,
};
static const u8 mt6797_consys_test_patch_sha256[] = {
	0xc6, 0x41, 0xe6, 0xcd, 0x90, 0x29, 0x46, 0x1f,
	0xb7, 0xbe, 0x65, 0x87, 0x6e, 0xad, 0xed, 0x73,
	0x90, 0x3d, 0xa2, 0xb2, 0x5b, 0x62, 0x1d, 0x90,
	0xe9, 0x37, 0x10, 0x7a, 0x2a, 0x2e, 0x8f, 0xe1,
};
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
		struct mt6797_consys_firmware_blob *config,
		struct mt6797_consys_firmware_manifest *manifest)
{
	*config = (struct mt6797_consys_firmware_blob) {
		.name = "WMT_SOC.cfg",
		.data = mt6797_consys_test_config,
		.size = sizeof(mt6797_consys_test_config) - 1,
		.sha256 = mt6797_consys_test_config_sha256,
	};

	mt6797_consys_test_patch_data(patch_data);
	patches[0].blob.name = "ROMv3_patch_1_1_hdr.bin";
	patches[0].blob.data = patch_data;
	patches[0].blob.size = MT6797_CONSYS_TEST_PATCH_SIZE;
	patches[0].blob.sha256 = mt6797_consys_test_patch_sha256;
	patches[0].load_address = mt6797_consys_test_address;
	patches[1] = patches[0];
	patches[1].blob.name = "ROMv3_patch_1_0_hdr.bin";

	*manifest = (struct mt6797_consys_firmware_manifest) {
		.config = config,
		.patches = patches,
		.patch_count = 2,
	};
}

static void mt6797_consys_test_accepts_observed_shape(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_blob config;
	struct mt6797_consys_firmware_manifest manifest;
	struct mt6797_consys_patch_info info;

	mt6797_consys_test_manifest(patch_data, patches, &config, &manifest);
	KUNIT_ASSERT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			0);
	KUNIT_ASSERT_EQ(test, mt6797_consys_patch_preflight(&patches[0], &info), 0);
	KUNIT_EXPECT_MEMEQ(test, info.build_time, "20180307b001005\0", 16);
	KUNIT_EXPECT_MEMEQ(test, info.platform, "GEMI", 4);
	KUNIT_EXPECT_EQ(test, info.hw_version, (u16)0x8a00);
	KUNIT_EXPECT_EQ(test, info.sw_version, (u16)0x8a00);
	KUNIT_EXPECT_EQ(test, info.patch_version, 0xfaU);
	KUNIT_EXPECT_EQ(test, info.body_size, (size_t)1);
	KUNIT_EXPECT_MEMEQ(test, info.load_address, mt6797_consys_test_address,
				  sizeof(info.load_address));
}

static void mt6797_consys_test_rejects_missing_address_and_bad_digest(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_blob config;
	struct mt6797_consys_firmware_manifest manifest;

	mt6797_consys_test_manifest(patch_data, patches, &config, &manifest);
	patches[0].load_address = NULL;
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			-ENODATA);

	patches[0].load_address = mt6797_consys_test_address;
	patch_data[28] = 'Y';
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			-EKEYREJECTED);
}

static void mt6797_consys_test_rejects_ambiguous_or_noncanonical_manifest(struct kunit *test)
{
	u8 patch_data[MT6797_CONSYS_TEST_PATCH_SIZE];
	struct mt6797_consys_patch_image patches[2] = { };
	struct mt6797_consys_firmware_blob config;
	struct mt6797_consys_firmware_manifest manifest;

	mt6797_consys_test_manifest(patch_data, patches, &config, &manifest);
	patches[1].blob.name = patches[0].blob.name;
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			-EEXIST);

	mt6797_consys_test_manifest(patch_data, patches, &config, &manifest);
	patches[0].blob.name = "vendor/ROMv3_patch_1_1_hdr.bin";
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			-EINVAL);

	mt6797_consys_test_manifest(patch_data, patches, &config, &manifest);
	config.name = "other.cfg";
	KUNIT_EXPECT_EQ(test, mt6797_consys_firmware_manifest_preflight(&manifest),
			-EPROTO);
}

static struct kunit_case mt6797_consys_firmware_test_cases[] = {
	KUNIT_CASE(mt6797_consys_test_accepts_observed_shape),
	KUNIT_CASE(mt6797_consys_test_rejects_missing_address_and_bad_digest),
	KUNIT_CASE(mt6797_consys_test_rejects_ambiguous_or_noncanonical_manifest),
	{ }
};

static struct kunit_suite mt6797_consys_firmware_test_suite = {
	.name = "mt6797-consys-firmware",
	.test_cases = mt6797_consys_firmware_test_cases,
};

kunit_test_suite(mt6797_consys_firmware_test_suite);

MODULE_LICENSE("GPL");
