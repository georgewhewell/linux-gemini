// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free KUnit coverage for the dormant calibrated-provider bridge.
 */

#include <kunit/test.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-calibrated-provider.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-snapshot.h>

struct mt6797_dvfsp_provider_test_devices {
	struct device clock_backend;
	struct device bigidvfs_backend;
	struct device thermal;
	struct device calibration_source;
};

static void mt6797_dvfsp_provider_test_release(struct device *device)
{
}

static void mt6797_dvfsp_provider_test_init_device(struct device *device)
{
	device_initialize(device);
	device->release = mt6797_dvfsp_provider_test_release;
}

static void mt6797_dvfsp_provider_test_init_devices(
	struct mt6797_dvfsp_provider_test_devices *devices)
{
	mt6797_dvfsp_provider_test_init_device(&devices->clock_backend);
	mt6797_dvfsp_provider_test_init_device(&devices->bigidvfs_backend);
	mt6797_dvfsp_provider_test_init_device(&devices->thermal);
	mt6797_dvfsp_provider_test_init_device(&devices->calibration_source);
}

static void mt6797_dvfsp_provider_test_put_devices(
	struct mt6797_dvfsp_provider_test_devices *devices)
{
	put_device(&devices->calibration_source);
	put_device(&devices->thermal);
	put_device(&devices->bigidvfs_backend);
	put_device(&devices->clock_backend);
}

static void mt6797_dvfsp_provider_test_tuple(
	struct mt6797_dvfsp_provider_test_devices *devices,
	struct mt6797_dvfsp_state_source_devices *tuple)
{
	tuple->clock_backend = &devices->clock_backend;
	tuple->bigidvfs_backend = &devices->bigidvfs_backend;
	tuple->thermal = &devices->thermal;
	tuple->calibration_source = &devices->calibration_source;
}

static int mt6797_dvfsp_provider_test_identity_epoch(
	void *context, const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *epoch)
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_provider_test_identity_handle(
	void *context, const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *handle)
{
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_identity_provenance_ops
mt6797_dvfsp_provider_test_identity_ops = {
	.abi = MT6797_DVFSP_IDENTITY_SOURCE_ABI,
	.read_table_epoch = mt6797_dvfsp_provider_test_identity_epoch,
	.read_calibration_handle = mt6797_dvfsp_provider_test_identity_handle,
};

static int mt6797_dvfsp_provider_test_ppm_lock(void *context)
{
	return -EOPNOTSUPP;
}

static void mt6797_dvfsp_provider_test_ppm_unlock(void *context)
{
}

static int mt6797_dvfsp_provider_test_ppm_clusters(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_ppm_snapshot *snapshot)
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_provider_test_ppm_cci(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u32 frequency_khz[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES])
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_provider_test_ppm_limits(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u32 limit_khz[MT6797_DVFSP_PPM_POLICY_BANK_COUNT]
				[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES])
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_provider_test_ppm_epoch(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u64 *epoch)
{
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_ppm_source_ops
mt6797_dvfsp_provider_test_ppm_ops = {
	.abi = MT6797_DVFSP_PPM_SOURCE_ADAPTER_ABI,
	.lock = mt6797_dvfsp_provider_test_ppm_lock,
	.unlock = mt6797_dvfsp_provider_test_ppm_unlock,
	.read_ppm_clusters = mt6797_dvfsp_provider_test_ppm_clusters,
	.read_cci_frequency = mt6797_dvfsp_provider_test_ppm_cci,
	.read_policy_limits = mt6797_dvfsp_provider_test_ppm_limits,
	.read_table_epoch = mt6797_dvfsp_provider_test_ppm_epoch,
};

static int mt6797_dvfsp_provider_test_live_generation(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm, u64 *generation,
	u64 *owner_handle, u64 *transition_handle,
	struct mt6797_dvfsp_state_provenance *provenance)
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_provider_test_live_cluster(
	void *context, unsigned int cluster,
	const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_live_source_cluster *state)
{
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_live_source_ops
mt6797_dvfsp_provider_test_live_ops = {
	.abi = MT6797_DVFSP_LIVE_SOURCE_ADAPTER_ABI,
	.read_generation = mt6797_dvfsp_provider_test_live_generation,
	.read_cluster = mt6797_dvfsp_provider_test_live_cluster,
};

static int mt6797_dvfsp_provider_test_fill_calibration(
	void *context, const struct mt6797_dvfsp_ptp_handoff *handoff,
	const struct mt6797_dvfsp_ptp_state *ptp_state,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy,
	const struct mt6797_eem_readback *readback,
	const struct mt6797_dvfsp_state_owner_source_identity *identity,
	struct mt6797_eem_calibration_input *input)
{
	return -EOPNOTSUPP;
}

static void mt6797_dvfsp_provider_test_invalidate(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
}

static const struct mt6797_dvfsp_calibrated_provider_source_ops
mt6797_dvfsp_provider_test_source_ops = {
	.abi = MT6797_DVFSP_CALIBRATED_SOURCE_ABI,
	.fill_calibration = mt6797_dvfsp_provider_test_fill_calibration,
	.invalidate = mt6797_dvfsp_provider_test_invalidate,
};

static void mt6797_dvfsp_provider_rejects_incomplete_source(
	struct kunit *test)
{
	struct mt6797_dvfsp_calibrated_provider provider = { };
	struct mt6797_dvfsp_resource_owner owner = { };
	struct mt6797_dvfsp_provider_test_devices *devices;
	struct mt6797_dvfsp_state_source_devices tuple = { };

	devices = kunit_kzalloc(test, sizeof(*devices), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, devices);
	mt6797_dvfsp_provider_test_init_devices(devices);
	mt6797_dvfsp_provider_test_tuple(devices, &tuple);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&owner, 0x67970010), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&owner, &tuple), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_calibrated_provider_bind_sources(
			&provider, &owner, NULL, NULL, NULL, NULL, NULL, 1, 2),
		-EINVAL);
	KUNIT_EXPECT_FALSE(test, owner.source_bound);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_resource_owner_detach(&owner), 0);
	mt6797_dvfsp_provider_test_put_devices(devices);
}

static void mt6797_dvfsp_provider_binds_and_exits(
	struct kunit *test)
{
	struct mt6797_dvfsp_calibrated_provider provider = { };
	struct mt6797_dvfsp_resource_owner owner = { };
	struct mt6797_dvfsp_provider_test_devices *devices;
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_identity_source identity = { };
	struct mt6797_dvfsp_live_source live = { };
	struct mt6797_dvfsp_ppm_source ppm = { };
	struct mt6797_dvfsp_state_owner_ops owner_ops = { };
	int ret;

	devices = kunit_kzalloc(test, sizeof(*devices), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, devices);
	mt6797_dvfsp_provider_test_init_devices(devices);
	mt6797_dvfsp_provider_test_tuple(devices, &tuple);
	identity.devices = tuple;
	identity.provenance_ops = &mt6797_dvfsp_provider_test_identity_ops;
	live.ops = &mt6797_dvfsp_provider_test_live_ops;
	ppm.ops = &mt6797_dvfsp_provider_test_ppm_ops;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&owner, 0x67970011), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&owner, &tuple), 0);
	ret = mt6797_dvfsp_calibrated_provider_bind_sources(
		&provider, &owner, &identity, &live, &ppm,
		&mt6797_dvfsp_provider_test_source_ops, NULL, 1, 2);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, provider.initialized);
	KUNIT_EXPECT_TRUE(test, owner.source_bound);
	KUNIT_EXPECT_FALSE(test, provider.arbitration.registered);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_calibrated_provider_owner_ops_init(
			&provider, &owner_ops), 0);
	KUNIT_EXPECT_TRUE(test, !!owner_ops.snapshot);
	KUNIT_EXPECT_TRUE(test, !!owner_ops.hold);
	KUNIT_EXPECT_EQ(test,
		provider.source.ppm_owner_ops->lock(
			provider.source.ppm_owner_context), -EOPNOTSUPP);
	KUNIT_EXPECT_PTR_EQ(test, provider.source.ppm_owner_context,
			&provider.ppm_source);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_calibrated_provider_exit(&provider), 0);
	KUNIT_EXPECT_FALSE(test, owner.source_bound);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_resource_owner_detach(&owner), 0);
	mt6797_dvfsp_provider_test_put_devices(devices);
}

static void mt6797_dvfsp_provider_preserves_wide_epoch(
	struct kunit *test)
{
	struct mt6797_dvfsp_state_provenance provenance = {
		.abi = MT6797_DVFSP_STATE_PROVENANCE_ABI,
		.table_epoch = 0x100000002ULL,
	};

	KUNIT_EXPECT_EQ(test, provenance.table_epoch, 0x100000002ULL);
	KUNIT_EXPECT_EQ(test, sizeof(provenance.table_epoch),
		sizeof(u64));
}

static void mt6797_dvfsp_provider_snapshot_keeps_attribution(
	struct kunit *test)
{
	struct mt6797_dvfsp_clock_state clock = {
		.abi = MT6797_DVFSP_CLOCK_STATE_ABI,
		.clock_sample_generation = 1,
		.big_sample_generation = 1,
	};
	struct mt6797_dvfsp_calibration_state calibration = {
		.abi = MT6797_DVFSP_CALIBRATION_STATE_ABI,
		.phase = MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON,
		.bank_mask = MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL,
		.thermal_generation = 1,
		.clock_owner_generation = 1,
		.rail_owner_generation = 1,
		.source_generation = 9,
	};
	struct mt6797_dvfsp_state_snapshot_input input = {
		.abi = MT6797_DVFSP_STATE_SNAPSHOT_ABI,
		.generation = 9,
		.owner_handle = 0x67970031,
		.transition_handle = 0x67970030,
		.clock = &clock,
		.calibration = &calibration,
		.provenance = {
			.abi = MT6797_DVFSP_STATE_PROVENANCE_ABI,
			.source_mask = MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL,
			.variant_id = 274,
			.table_epoch = 0x100000002ULL,
			.calibration_handle = 0x6797abcdULL,
		},
	};
	struct mt6797_dvfsp_state_snapshot snapshot;
	unsigned int i;

	calibration.provenance = input.provenance;
	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		struct mt6797_dvfsp_calibration_table_entry *entry =
			&calibration.cluster[i].table[0];

		calibration.cluster[i].table_count = 1;
		entry->frequency_khz = 100000 + i;
		entry->vproc_uv = 800000;
		entry->vsram_uv = 820000;
		entry->ppm_limit_khz = 200000;
		clock.cluster[i].frequency_khz = entry->frequency_khz;
		input.state_flags[i] = MT6797_DVFSP_STATE_FLAG_PRESENT |
			MT6797_DVFSP_STATE_FLAG_ON;
		input.frequency_khz[i] = entry->frequency_khz;
		input.voltage_uv[i] = entry->vproc_uv;
		input.vsram_uv[i] = entry->vsram_uv;
		input.ceiling_khz[i] = 200000;
		input.floor_khz[i] = 50000;
	}

	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_state_snapshot_assemble(
		&input, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, snapshot.generation, input.generation);
	KUNIT_EXPECT_EQ(test, snapshot.owner_handle, input.owner_handle);
	KUNIT_EXPECT_EQ(test, snapshot.transition_handle,
		input.transition_handle);
	KUNIT_EXPECT_EQ(test, snapshot.provenance.table_epoch,
		input.provenance.table_epoch);
	KUNIT_EXPECT_EQ(test, snapshot.provenance.calibration_handle,
		input.provenance.calibration_handle);
}

static struct kunit_case mt6797_dvfsp_provider_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_provider_rejects_incomplete_source),
	KUNIT_CASE(mt6797_dvfsp_provider_binds_and_exits),
	KUNIT_CASE(mt6797_dvfsp_provider_preserves_wide_epoch),
	KUNIT_CASE(mt6797_dvfsp_provider_snapshot_keeps_attribution),
	{ }
};

static struct kunit_suite mt6797_dvfsp_provider_test_suite = {
	.name = "mt6797-dvfsp-calibrated-provider",
	.test_cases = mt6797_dvfsp_provider_test_cases,
};

kunit_test_suite(mt6797_dvfsp_provider_test_suite);

MODULE_LICENSE("GPL");
