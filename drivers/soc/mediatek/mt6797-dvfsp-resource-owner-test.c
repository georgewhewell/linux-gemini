// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free KUnit coverage for the MT6797 DVFSP resource owner.
 *
 * This fixture uses only initialized struct device objects so the owner
 * lifetime and arbitration contract can be tested without a platform probe.
 */

#include <kunit/test.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-resource-owner.h>

struct mt6797_dvfsp_owner_test_devices {
	struct device clock_backend;
	struct device bigidvfs_backend;
	struct device thermal;
	struct device calibration_source;
};

static void mt6797_dvfsp_owner_test_release(struct device *device)
{
}

static void mt6797_dvfsp_owner_test_device_init(struct device *device)
{
	device_initialize(device);
	device->release = mt6797_dvfsp_owner_test_release;
}

static void mt6797_dvfsp_owner_test_devices_init(
	struct mt6797_dvfsp_owner_test_devices *devices)
{
	mt6797_dvfsp_owner_test_device_init(&devices->clock_backend);
	mt6797_dvfsp_owner_test_device_init(&devices->bigidvfs_backend);
	mt6797_dvfsp_owner_test_device_init(&devices->thermal);
	mt6797_dvfsp_owner_test_device_init(&devices->calibration_source);
}

static void mt6797_dvfsp_owner_test_devices_put(
	struct mt6797_dvfsp_owner_test_devices *devices)
{
	put_device(&devices->calibration_source);
	put_device(&devices->thermal);
	put_device(&devices->bigidvfs_backend);
	put_device(&devices->clock_backend);
}

static void mt6797_dvfsp_owner_test_tuple(
	struct mt6797_dvfsp_owner_test_devices *devices,
	struct mt6797_dvfsp_state_source_devices *tuple)
{
	tuple->clock_backend = &devices->clock_backend;
	tuple->bigidvfs_backend = &devices->bigidvfs_backend;
	tuple->thermal = &devices->thermal;
	tuple->calibration_source = &devices->calibration_source;
}

static void mt6797_dvfsp_owner_rejects_invalid_lifecycle(struct kunit *test)
{
	struct mt6797_dvfsp_resource_owner owner = { };
	struct mt6797_dvfsp_state_owner_arbitration_ops ops = { };
	u64 generation = 0;

	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_init(NULL, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_init(&owner, 0), -EINVAL);
	KUNIT_ASSERT_EQ(test,
			mt6797_dvfsp_resource_owner_init(&owner, 0x67970001), 0);
	KUNIT_EXPECT_TRUE(test, owner.writes_disabled);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_advance_generation(&owner),
			-ENODEV);
	KUNIT_ASSERT_EQ(test,
			mt6797_dvfsp_resource_owner_arbitration_ops_init(&owner,
									&ops), 0);
	KUNIT_EXPECT_EQ(test, ops.lock(&owner), -ENODEV);
	KUNIT_EXPECT_EQ(test,
			ops.read_generation(&owner, &generation), -ENODEV);
}

static void mt6797_dvfsp_owner_serializes_generation(struct kunit *test)
{
	struct mt6797_dvfsp_resource_owner owner = { };
	struct mt6797_dvfsp_owner_test_devices *devices;
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_state_owner_arbitration_ops ops = { };
	u64 generation = 0;
	int ret;

	devices = kunit_kzalloc(test, sizeof(*devices), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, devices);
	mt6797_dvfsp_owner_test_devices_init(devices);
	mt6797_dvfsp_owner_test_tuple(devices, &tuple);
	ret = mt6797_dvfsp_resource_owner_init(&owner, 0x67970002);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_devices;

	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_attach(&owner, NULL),
			-ENODEV);
	ret = mt6797_dvfsp_resource_owner_attach(&owner, &tuple);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_devices;
	KUNIT_EXPECT_EQ(test, owner.generation, (u64)1);
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_attach(&owner, &tuple),
			-EBUSY);

	ret = mt6797_dvfsp_resource_owner_arbitration_ops_init(&owner, &ops);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_detach;

	ret = ops.lock(&owner);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_detach;
	ret = ops.read_generation(&owner, &generation);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)1);
	if (!ret) {
		ret = mt6797_dvfsp_resource_owner_advance_generation(&owner);
		KUNIT_EXPECT_EQ(test, ret, 0);
		ret = ops.read_generation(&owner, &generation);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, generation, (u64)2);
	}
	ops.unlock(&owner);

out_detach:
	if (owner.attached)
		KUNIT_EXPECT_EQ(test,
				mt6797_dvfsp_resource_owner_detach(&owner), 0);
out_devices:
	mt6797_dvfsp_owner_test_devices_put(devices);
}

static void mt6797_dvfsp_owner_binding_blocks_detach(struct kunit *test)
{
	struct mt6797_dvfsp_resource_owner owner = { };
	struct mt6797_dvfsp_owner_test_devices *devices;
	struct mt6797_dvfsp_state_source_devices tuple = { };
	int ret;

	devices = kunit_kzalloc(test, sizeof(*devices), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, devices);
	mt6797_dvfsp_owner_test_devices_init(devices);
	mt6797_dvfsp_owner_test_tuple(devices, &tuple);
	ret = mt6797_dvfsp_resource_owner_init(&owner, 0x67970003);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_dvfsp_resource_owner_attach(&owner, &tuple);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = mt6797_dvfsp_resource_owner_bind_source(&owner);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test,
				mt6797_dvfsp_resource_owner_bind_source(&owner),
				-EBUSY);
		KUNIT_EXPECT_EQ(test,
				mt6797_dvfsp_resource_owner_detach(&owner), -EBUSY);
		KUNIT_EXPECT_EQ(test,
				mt6797_dvfsp_resource_owner_unbind_source(&owner), 0);
	}
	KUNIT_EXPECT_EQ(test,
			mt6797_dvfsp_resource_owner_detach(&owner), 0);
	mt6797_dvfsp_owner_test_devices_put(devices);
}

static struct kunit_case mt6797_dvfsp_owner_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_owner_rejects_invalid_lifecycle),
	KUNIT_CASE(mt6797_dvfsp_owner_serializes_generation),
	KUNIT_CASE(mt6797_dvfsp_owner_binding_blocks_detach),
	{ }
};

static struct kunit_suite mt6797_dvfsp_owner_test_suite = {
	.name = "mt6797-dvfsp-resource-owner",
	.test_cases = mt6797_dvfsp_owner_test_cases,
};

kunit_test_suite(mt6797_dvfsp_owner_test_suite);

MODULE_LICENSE("GPL");
