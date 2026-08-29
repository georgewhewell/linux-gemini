// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free coverage for the source-independent writer owner bridge. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer-bridge.h>

static int mt6797_dvfsp_vendor_writer_bridge_test_provenance(
	void *context, struct mt6797_dvfsp_state_provenance *provenance)
{
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_vendor_writer_ops
mt6797_dvfsp_vendor_writer_bridge_test_writer_ops = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_ABI,
	.read_provenance = mt6797_dvfsp_vendor_writer_bridge_test_provenance,
};

static void mt6797_dvfsp_vendor_writer_bridge_contract(struct kunit *test)
{
	struct mt6797_dvfsp_vendor_writer writer = { };
	struct mt6797_dvfsp_vendor_writer_bridge bridge = { };
	struct mutex lock;
	const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	u64 shared_generation = 1;

	mutex_init(&lock);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock,
			&shared_generation,
			&mt6797_dvfsp_vendor_writer_bridge_test_writer_ops,
			NULL, 0x67970031, 0x67970032), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_bridge_init(&bridge, &writer), 0);
	ops = mt6797_dvfsp_vendor_writer_bridge_ops();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ops);
	KUNIT_EXPECT_EQ(test, ops->abi,
		(u32)MT6797_DVFSP_VENDOR_WRITER_BRIDGE_ABI);
	KUNIT_ASSERT_EQ(test,
		ops->read_identity(&bridge, &owner_handle, &transition_handle), 0);
	KUNIT_EXPECT_EQ(test, owner_handle, (u64)0x67970031);
	KUNIT_EXPECT_EQ(test, transition_handle, (u64)0x67970032);

	KUNIT_ASSERT_EQ(test,
		ops->begin(&bridge, MT6797_DVFSP_VENDOR_WRITER_SITE_VOLTAGE,
			&generation), 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)1);
	KUNIT_EXPECT_TRUE(test, bridge.active);
	KUNIT_EXPECT_TRUE(test, mutex_is_locked(&lock));
	KUNIT_EXPECT_EQ(test,
		ops->commit(&bridge, MT6797_DVFSP_VENDOR_WRITER_SITE_VOLTAGE,
			generation), 0);
	KUNIT_EXPECT_FALSE(test, bridge.active);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)2);

	KUNIT_ASSERT_EQ(test,
		ops->begin(&bridge, MT6797_DVFSP_VENDOR_WRITER_SITE_PTP_TABLE,
			&generation), 0);
	KUNIT_EXPECT_EQ(test,
		ops->commit(&bridge, MT6797_DVFSP_VENDOR_WRITER_SITE_PTP_TABLE,
			generation + 1), -ESTALE);
	KUNIT_EXPECT_TRUE(test, bridge.active);
	KUNIT_EXPECT_EQ(test,
		ops->abort(&bridge, MT6797_DVFSP_VENDOR_WRITER_SITE_PTP_TABLE,
			generation + 1), -ESTALE);
	KUNIT_EXPECT_FALSE(test, bridge.active);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)2);
}

static struct kunit_case mt6797_dvfsp_vendor_writer_bridge_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_vendor_writer_bridge_contract),
	{ }
};

static struct kunit_suite mt6797_dvfsp_vendor_writer_bridge_suite = {
	.name = "mt6797-dvfsp-vendor-writer-bridge",
	.test_cases = mt6797_dvfsp_vendor_writer_bridge_cases,
};

kunit_test_suite(mt6797_dvfsp_vendor_writer_bridge_suite);

MODULE_LICENSE("GPL");
