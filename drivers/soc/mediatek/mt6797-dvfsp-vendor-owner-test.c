// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free KUnit coverage for the cooperative vendor owner adapter. */

#include <kunit/test.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-owner.h>
#include <linux/soc/mediatek/mt6797-dvfsp-vendor-coordinator.h>

static const struct mt6797_dvfsp_vendor_source_registration
mt6797_dvfsp_vendor_test_source_registration;
 
struct mt6797_dvfsp_vendor_test_devices {
	struct device clock_backend;
	struct device bigidvfs_backend;
	struct device thermal;
	struct device calibration_source;
};

struct mt6797_dvfsp_vendor_test_registration {
	const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops;
	void *bridge_context;
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops;
	void *runtime_context;
	u64 owner_handle;
	u64 transition_handle;
	unsigned int register_calls;
	unsigned int unregister_calls;
	unsigned int runtime_event_calls;
};

static unsigned int mt6797_dvfsp_vendor_test_callbacks;

struct mt6797_dvfsp_vendor_test_integration {
	const void *bridge_ops;
	void *bridge_context;
	const void *runtime_ops;
	void *runtime_context;
	const void *lifecycle_ops;
	void *lifecycle_context;
	u64 owner_handle;
	u64 transition_handle;
	bool bound;
	bool mainline_registered;
	unsigned int bind_calls;
	unsigned int unbind_calls;
	unsigned int register_calls;
	unsigned int unregister_calls;
};

static void mt6797_dvfsp_vendor_test_release(struct device *device)
{
}

static void mt6797_dvfsp_vendor_test_init_device(struct device *device)
{
	device_initialize(device);
	device->release = mt6797_dvfsp_vendor_test_release;
}

static void mt6797_dvfsp_vendor_test_init_devices(
	struct mt6797_dvfsp_vendor_test_devices *devices)
{
	mt6797_dvfsp_vendor_test_init_device(&devices->clock_backend);
	mt6797_dvfsp_vendor_test_init_device(&devices->bigidvfs_backend);
	mt6797_dvfsp_vendor_test_init_device(&devices->thermal);
	mt6797_dvfsp_vendor_test_init_device(&devices->calibration_source);
}

static void mt6797_dvfsp_vendor_test_put_devices(
	struct mt6797_dvfsp_vendor_test_devices *devices)
{
	put_device(&devices->calibration_source);
	put_device(&devices->thermal);
	put_device(&devices->bigidvfs_backend);
	put_device(&devices->clock_backend);
}

static void mt6797_dvfsp_vendor_test_tuple(
	struct mt6797_dvfsp_vendor_test_devices *devices,
	struct mt6797_dvfsp_state_source_devices *tuple)
{
	tuple->clock_backend = &devices->clock_backend;
	tuple->bigidvfs_backend = &devices->bigidvfs_backend;
	tuple->thermal = &devices->thermal;
	tuple->calibration_source = &devices->calibration_source;
}

static int mt6797_dvfsp_vendor_test_identity_epoch(
	void *context, const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *epoch)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_identity_handle(
	void *context, const struct mt6797_dvfsp_ptp_identity *identity,
	u64 *handle)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_identity_provenance_ops
mt6797_dvfsp_vendor_test_identity_ops = {
	.abi = MT6797_DVFSP_IDENTITY_SOURCE_ABI,
	.read_table_epoch = mt6797_dvfsp_vendor_test_identity_epoch,
	.read_calibration_handle = mt6797_dvfsp_vendor_test_identity_handle,
};

static int mt6797_dvfsp_vendor_test_ppm_lock(void *context)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static void mt6797_dvfsp_vendor_test_ppm_unlock(void *context)
{
	mt6797_dvfsp_vendor_test_callbacks++;
}

static int mt6797_dvfsp_vendor_test_ppm_clusters(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_ppm_snapshot *snapshot)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_ppm_cci(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u32 frequency_khz[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES])
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_ppm_limits(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u32 limit_khz[MT6797_DVFSP_PPM_POLICY_BANK_COUNT]
			[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES])
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_ppm_epoch(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	u64 *epoch)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_ppm_source_ops
mt6797_dvfsp_vendor_test_ppm_ops = {
	.abi = MT6797_DVFSP_PPM_SOURCE_ADAPTER_ABI,
	.lock = mt6797_dvfsp_vendor_test_ppm_lock,
	.unlock = mt6797_dvfsp_vendor_test_ppm_unlock,
	.read_ppm_clusters = mt6797_dvfsp_vendor_test_ppm_clusters,
	.read_cci_frequency = mt6797_dvfsp_vendor_test_ppm_cci,
	.read_policy_limits = mt6797_dvfsp_vendor_test_ppm_limits,
	.read_table_epoch = mt6797_dvfsp_vendor_test_ppm_epoch,
};

static int mt6797_dvfsp_vendor_test_live_generation(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm, u64 *generation,
	u64 *owner_handle, u64 *transition_handle,
	struct mt6797_dvfsp_state_provenance *provenance)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_live_cluster(
	void *context, unsigned int cluster,
	const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_live_source_cluster *state)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_live_source_ops
mt6797_dvfsp_vendor_test_live_ops = {
	.abi = MT6797_DVFSP_LIVE_SOURCE_ADAPTER_ABI,
	.read_generation = mt6797_dvfsp_vendor_test_live_generation,
	.read_cluster = mt6797_dvfsp_vendor_test_live_cluster,
};

static int mt6797_dvfsp_vendor_test_fill_calibration(
	void *context, const struct mt6797_dvfsp_ptp_handoff *handoff,
	const struct mt6797_dvfsp_ptp_state *ptp_state,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy,
	const struct mt6797_eem_readback *readback,
	const struct mt6797_dvfsp_state_owner_source_identity *identity,
	struct mt6797_eem_calibration_input *input)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static void mt6797_dvfsp_vendor_test_invalidate(
	void *context, enum mt6797_dvfsp_state_invalidation_reason reason)
{
	mt6797_dvfsp_vendor_test_callbacks++;
}

static const struct mt6797_dvfsp_calibrated_provider_source_ops
mt6797_dvfsp_vendor_test_provider_ops = {
	.abi = MT6797_DVFSP_CALIBRATED_SOURCE_ABI,
	.fill_calibration = mt6797_dvfsp_vendor_test_fill_calibration,
	.invalidate = mt6797_dvfsp_vendor_test_invalidate,
};

static int mt6797_dvfsp_vendor_test_writer_provenance(
	void *context, struct mt6797_dvfsp_state_provenance *provenance)
{
	mt6797_dvfsp_vendor_test_callbacks++;
	return -EOPNOTSUPP;
}

static const struct mt6797_dvfsp_vendor_writer_ops
mt6797_dvfsp_vendor_test_writer_ops = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_ABI,
	.read_provenance = mt6797_dvfsp_vendor_test_writer_provenance,
};

static int mt6797_dvfsp_vendor_test_runtime_event(
	void *context, unsigned int type, unsigned int cpu)
{
	struct mt6797_dvfsp_vendor_test_registration *state = context;

	if (!state || type >= MT6797_DVFSP_VENDOR_RUNTIME_EVENT_COUNT ||
	    (cpu != MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE && cpu >= 10))
		return -EINVAL;
	state->runtime_event_calls++;
	return 0;
}

static const struct mt6797_dvfsp_vendor_runtime_ops
mt6797_dvfsp_vendor_test_runtime_ops = {
	.abi = MT6797_DVFSP_VENDOR_RUNTIME_EVENT_ABI,
	.event = mt6797_dvfsp_vendor_test_runtime_event,
};

static int mt6797_dvfsp_vendor_test_register_owner(
	void *context, const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops,
	void *bridge_context,
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
	void *runtime_context, u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_registration *state = context;

	if (!state || !ops || ops->abi != MT6797_DVFSP_VENDOR_WRITER_BRIDGE_ABI ||
	    !runtime_ops ||
	    runtime_ops->abi != MT6797_DVFSP_VENDOR_RUNTIME_EVENT_ABI ||
	    !runtime_ops->event)
		return -EINVAL;
	state->ops = ops;
	state->bridge_context = bridge_context;
	state->runtime_ops = runtime_ops;
	state->runtime_context = runtime_context;
	state->owner_handle = owner_handle;
	state->transition_handle = transition_handle;
	state->register_calls++;
	return 0;
}

static int mt6797_dvfsp_vendor_test_unregister_owner(
	void *context, const struct mt6797_dvfsp_vendor_writer_bridge_ops *ops,
	void *bridge_context,
	const struct mt6797_dvfsp_vendor_runtime_ops *runtime_ops,
	void *runtime_context, u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_registration *state = context;

	if (!state || state->ops != ops || state->bridge_context != bridge_context ||
	    state->runtime_ops != runtime_ops ||
	    state->runtime_context != runtime_context ||
	    state->owner_handle != owner_handle ||
	    state->transition_handle != transition_handle)
		return -EPROTO;
	state->unregister_calls++;
	return 0;
}

static const struct mt6797_dvfsp_vendor_writer_registration_ops
mt6797_dvfsp_vendor_test_registration_ops = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_REGISTRATION_ABI,
	.register_owner = mt6797_dvfsp_vendor_test_register_owner,
	.unregister_owner = mt6797_dvfsp_vendor_test_unregister_owner,
};

static int mt6797_dvfsp_vendor_test_integration_register_mainline(
	void *context, const void *bridge_ops, void *bridge_context,
	const void *runtime_ops, void *runtime_context,
	u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_integration *state = context;

	if (!state || !state->bound || state->mainline_registered ||
	    state->bridge_ops != bridge_ops ||
	    state->bridge_context != bridge_context ||
	    state->runtime_ops != runtime_ops ||
	    state->runtime_context != runtime_context ||
	    state->owner_handle != owner_handle ||
	    state->transition_handle != transition_handle)
		return -EPROTO;
	state->mainline_registered = true;
	state->register_calls++;
	return 0;
}

static int mt6797_dvfsp_vendor_test_integration_unregister_mainline(
	void *context, const void *bridge_ops, void *bridge_context,
	const void *runtime_ops, void *runtime_context,
	u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_integration *state = context;

	if (!state || !state->bound || !state->mainline_registered ||
	    state->bridge_ops != bridge_ops ||
	    state->bridge_context != bridge_context ||
	    state->runtime_ops != runtime_ops ||
	    state->runtime_context != runtime_context ||
	    state->owner_handle != owner_handle ||
	    state->transition_handle != transition_handle)
		return -EPROTO;
	state->mainline_registered = false;
	state->unregister_calls++;
	return 0;
}

static int mt6797_dvfsp_vendor_test_integration_bind(
	void *context, const void *bridge_ops, void *bridge_context,
	const void *runtime_ops, void *runtime_context,
	const void *lifecycle_ops, void *lifecycle_context,
	u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_integration *state = context;
	const struct mt6797_dvfsp_vendor_owner_lifecycle_ops *lifecycle =
		lifecycle_ops;
	int ret;

	if (!state || state->bound || !bridge_ops || !bridge_context ||
	    !runtime_ops || !runtime_context || !lifecycle ||
	    lifecycle->abi != MT6797_DVFSP_VENDOR_OWNER_LIFECYCLE_ABI ||
	    !lifecycle->register_writer || !lifecycle->unregister_writer ||
	    !lifecycle_context || !owner_handle || !transition_handle)
		return -EINVAL;
	state->bridge_ops = bridge_ops;
	state->bridge_context = bridge_context;
	state->runtime_ops = runtime_ops;
	state->runtime_context = runtime_context;
	state->lifecycle_ops = lifecycle_ops;
	state->lifecycle_context = lifecycle_context;
	state->owner_handle = owner_handle;
	state->transition_handle = transition_handle;
	state->bound = true;
	state->bind_calls++;
	ret = lifecycle->register_writer(lifecycle_context);
	if (ret)
		state->bound = false;
	return ret;
}

static int mt6797_dvfsp_vendor_test_integration_unbind(
	void *context, const void *bridge_ops, void *bridge_context,
	const void *runtime_ops, void *runtime_context,
	const void *lifecycle_ops, void *lifecycle_context,
	u64 owner_handle, u64 transition_handle)
{
	struct mt6797_dvfsp_vendor_test_integration *state = context;

	if (!state || !state->bound || state->mainline_registered ||
	    state->bridge_ops != bridge_ops ||
	    state->bridge_context != bridge_context ||
	    state->runtime_ops != runtime_ops ||
	    state->runtime_context != runtime_context ||
	    state->lifecycle_ops != lifecycle_ops ||
	    state->lifecycle_context != lifecycle_context ||
	    state->owner_handle != owner_handle ||
	    state->transition_handle != transition_handle)
		return -EPROTO;
	state->bound = false;
	state->unbind_calls++;
	return 0;
}

static const struct mt6797_dvfsp_vendor_integration_ops
mt6797_dvfsp_vendor_test_integration_ops = {
	.abi = MT6797_DVFSP_VENDOR_INTEGRATION_ABI,
	.bind_lifecycle = mt6797_dvfsp_vendor_test_integration_bind,
	.unbind_lifecycle = mt6797_dvfsp_vendor_test_integration_unbind,
	.register_mainline_owner =
		mt6797_dvfsp_vendor_test_integration_register_mainline,
	.unregister_mainline_owner =
		mt6797_dvfsp_vendor_test_integration_unregister_mainline,
	.source_registration = &mt6797_dvfsp_vendor_test_source_registration,
};
static int mt6797_dvfsp_vendor_test_source_snapshot(
	void *context, struct mt6797_dvfsp_vendor_source_snapshot *snapshot)
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_source_identity(
	void *context, struct mt6797_dvfsp_vendor_source_identity *identity)
{
	return -EOPNOTSUPP;
}

static int mt6797_dvfsp_vendor_test_source_identity_observation(
	void *context,
	struct mt6797_dvfsp_vendor_source_identity_observation *observation)
{
	if (!observation)
		return -EINVAL;
	observation->abi =
		MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_OBSERVATION_ABI;
	observation->source_mask = MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_ALL;
	observation->function_word = 0x22;
	observation->date_word = 0x70;
	observation->cpu_bin_level = 1;
	observation->eem_ate_version = 6;
	observation->ppm_table_type = 1;
	return 0;
}

static int mt6797_dvfsp_vendor_test_source_provenance(
	void *context,
	const struct mt6797_dvfsp_vendor_source_snapshot *snapshot,
	struct mt6797_dvfsp_vendor_source_provenance *provenance)
{
	if (!snapshot || !provenance)
		return -EINVAL;
	memset(provenance, 0, sizeof(*provenance));
	provenance->abi = MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI;
	provenance->variant_id = 1;
	provenance->table_epoch = 2;
	provenance->calibration_handle = 3;
	provenance->source_generation = snapshot->generation;
	provenance->owner_handle = snapshot->owner_handle;
	provenance->transition_handle = snapshot->transition_handle;
	return 0;
}

static const struct mt6797_dvfsp_vendor_source_ops
mt6797_dvfsp_vendor_test_source_ops = {
	.abi = MT6797_DVFSP_VENDOR_SOURCE_ABI,
	.snapshot = mt6797_dvfsp_vendor_test_source_snapshot,
	.read_identity = mt6797_dvfsp_vendor_test_source_identity,
	.read_identity_observation =
		mt6797_dvfsp_vendor_test_source_identity_observation,
	.read_provenance = mt6797_dvfsp_vendor_test_source_provenance,
};

static unsigned int mt6797_dvfsp_vendor_test_source_context;
static const struct mt6797_dvfsp_vendor_source_registration
mt6797_dvfsp_vendor_test_source_registration = {
	.ops = &mt6797_dvfsp_vendor_test_source_ops,
	.context = &mt6797_dvfsp_vendor_test_source_context,
};

static const struct mt6797_dvfsp_vendor_owner_ops
mt6797_dvfsp_vendor_test_owner_ops = {
	.abi = MT6797_DVFSP_VENDOR_OWNER_ABI,
	.identity_ops = &mt6797_dvfsp_vendor_test_identity_ops,
	.ppm_ops = &mt6797_dvfsp_vendor_test_ppm_ops,
	.live_ops = &mt6797_dvfsp_vendor_test_live_ops,
	.provider_ops = &mt6797_dvfsp_vendor_test_provider_ops,
	.writer_ops = &mt6797_dvfsp_vendor_test_writer_ops,
};

static void mt6797_dvfsp_vendor_owner_rejects_incomplete(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_owner owner = { };
	struct mt6797_dvfsp_resource_owner resource_owner = { };
	struct mt6797_dvfsp_vendor_owner_ops ops = { .abi =
		MT6797_DVFSP_VENDOR_OWNER_ABI };

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&resource_owner, 0x67970020), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind(&owner, &resource_owner, &ops,
			0x67970021, 0x67970020), -EINVAL);
	KUNIT_EXPECT_FALSE(test, owner.bound);
}

static void mt6797_dvfsp_vendor_owner_binds_and_unbinds(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_owner owner = { };
	struct mt6797_dvfsp_resource_owner resource_owner = { };
	struct mt6797_dvfsp_vendor_test_devices devices = { };
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_state_owner_ops state_ops = { };
	struct mt6797_dvfsp_vendor_test_registration registration = { };
	struct mt6797_dvfsp_vendor_owner_ops owner_ops =
		mt6797_dvfsp_vendor_test_owner_ops;
	u64 generation;

	mt6797_dvfsp_vendor_test_callbacks = 0;
	mt6797_dvfsp_vendor_test_init_devices(&devices);
	mt6797_dvfsp_vendor_test_tuple(&devices, &tuple);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&resource_owner, 0x67970022), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&resource_owner, &tuple), 0);
	owner_ops.writer_registration_ops =
		&mt6797_dvfsp_vendor_test_registration_ops;
	owner_ops.writer_registration_context = &registration;
	owner_ops.writer_runtime_ops = &mt6797_dvfsp_vendor_test_runtime_ops;
	owner_ops.writer_runtime_context = &registration;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind(&owner, &resource_owner,
			&owner_ops, 0x67970023,
			0x67970022), 0);
	KUNIT_EXPECT_TRUE(test, owner.bound);
	KUNIT_EXPECT_TRUE(test, owner.provider.initialized);
	KUNIT_EXPECT_TRUE(test, owner.writer.initialized);
	KUNIT_EXPECT_PTR_EQ(test, owner.writer.generation,
		&resource_owner.generation);
	KUNIT_EXPECT_PTR_EQ(test, owner.resource_owner, &resource_owner);
	KUNIT_EXPECT_PTR_EQ(test, owner.provider.ppm_source.ops,
			&mt6797_dvfsp_vendor_test_ppm_ops);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_owner_state_ops_init(
		&owner, &state_ops), 0);
	KUNIT_EXPECT_TRUE(test, !!state_ops.snapshot);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_test_callbacks, 0U);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_owner_register_writer(&owner), -ENODEV);
	KUNIT_EXPECT_FALSE(test, owner.writer_registered);
	KUNIT_EXPECT_EQ(test, registration.register_calls, 0U);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&owner.writer, &generation), 0);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_owner_unbind(&owner), -EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_writer_abort(&owner.writer), 0);
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_owner_unbind(&owner), 0);
	KUNIT_EXPECT_FALSE(test, owner.bound);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_test_callbacks, 1U);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_detach(&resource_owner), 0);
	mt6797_dvfsp_vendor_test_put_devices(&devices);
}


static void mt6797_dvfsp_vendor_owner_integrates_lifecycle(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_owner owner = { };
	struct mt6797_dvfsp_resource_owner resource_owner = { };
	struct mt6797_dvfsp_vendor_test_devices devices = { };
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_vendor_test_registration registration = { };
	struct mt6797_dvfsp_vendor_test_integration integration = { };
	struct mt6797_dvfsp_vendor_owner_ops owner_ops =
		mt6797_dvfsp_vendor_test_owner_ops;
	struct mt6797_dvfsp_vendor_source_identity_observation observation;
	struct mt6797_dvfsp_handoff *handoff;

	mt6797_dvfsp_vendor_test_init_devices(&devices);
	mt6797_dvfsp_vendor_test_tuple(&devices, &tuple);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&resource_owner, 0x67970030), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&resource_owner, &tuple), 0);
	owner_ops.writer_runtime_ops = &mt6797_dvfsp_vendor_test_runtime_ops;
	owner_ops.writer_runtime_context = &registration;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind(&owner, &resource_owner,
			&owner_ops, 0x67970031, 0x67970030), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind_writer_integration(&owner,
			&mt6797_dvfsp_vendor_test_integration_ops, &integration), 0);
	KUNIT_EXPECT_TRUE(test, integration.bound);
	KUNIT_EXPECT_TRUE(test, integration.mainline_registered);
	KUNIT_EXPECT_TRUE(test, owner.writer_registered);
	KUNIT_EXPECT_EQ(test, integration.bind_calls, 1U);
	KUNIT_EXPECT_EQ(test, integration.register_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, integration.bridge_ops,
		mt6797_dvfsp_vendor_writer_bridge_ops());
	KUNIT_EXPECT_PTR_EQ(test, integration.runtime_context, &owner);
	KUNIT_EXPECT_TRUE(test, owner.source.initialized);
	/* Opaque non-NULL token; the unsupported source must reject first. */
	handoff = kunit_kzalloc(test, 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, handoff);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_owner_register_state(
		&owner, handoff), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, owner.state_registered);
	KUNIT_EXPECT_PTR_EQ(test, owner.state_handoff, NULL);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_source_identity_observation(&owner.source,
			&observation), 0);
	KUNIT_EXPECT_EQ(test, observation.function_word, 0x22U);
	KUNIT_EXPECT_EQ(test, observation.date_word, 0x70U);
	KUNIT_ASSERT_EQ(test,
		((const struct mt6797_dvfsp_vendor_runtime_ops *)
		 integration.runtime_ops)->event(integration.runtime_context,
		 MT6797_DVFSP_VENDOR_RUNTIME_CPU_ONLINE,
		 MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE), 0);
	KUNIT_EXPECT_TRUE(test, owner.source.invalidated);
	KUNIT_EXPECT_EQ(test, registration.runtime_event_calls, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_owner_unbind_writer_integration(
		&owner), -EBUSY);
	KUNIT_ASSERT_EQ(test,
		((const struct mt6797_dvfsp_vendor_owner_lifecycle_ops *)
		 integration.lifecycle_ops)->unregister_writer(
		 integration.lifecycle_context), 0);
	KUNIT_EXPECT_FALSE(test, integration.mainline_registered);
	KUNIT_EXPECT_FALSE(test, owner.writer_registered);
	KUNIT_EXPECT_EQ(test, integration.unregister_calls, 1U);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_unbind_writer_integration(&owner), 0);
	KUNIT_EXPECT_FALSE(test, integration.bound);
	KUNIT_EXPECT_EQ(test, integration.unbind_calls, 1U);
	KUNIT_EXPECT_FALSE(test, owner.source.initialized);
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_owner_unbind(&owner), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_detach(&resource_owner), 0);
	mt6797_dvfsp_vendor_test_put_devices(&devices);
}

static void mt6797_dvfsp_vendor_owner_coordinator_stages_lifecycle(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_owner owner = { };
	struct mt6797_dvfsp_resource_owner resource_owner = { };
	struct mt6797_dvfsp_vendor_test_devices devices = { };
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_vendor_test_registration registration = { };
	struct mt6797_dvfsp_vendor_test_integration integration = { };
	struct mt6797_dvfsp_vendor_coordinator coordinator = { };
	struct mt6797_dvfsp_vendor_owner_ops owner_ops =
		mt6797_dvfsp_vendor_test_owner_ops;

	mt6797_dvfsp_vendor_test_init_devices(&devices);
	mt6797_dvfsp_vendor_test_tuple(&devices, &tuple);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&resource_owner, 0x67970040), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&resource_owner, &tuple), 0);
	owner_ops.writer_runtime_ops = &mt6797_dvfsp_vendor_test_runtime_ops;
	owner_ops.writer_runtime_context = &registration;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind(&owner, &resource_owner,
			&owner_ops, 0x67970041, 0x67970040), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_start(&coordinator, &owner,
			&mt6797_dvfsp_vendor_test_integration_ops, &integration), 0);
	KUNIT_EXPECT_TRUE(test,
		mt6797_dvfsp_vendor_coordinator_started(&coordinator));
	KUNIT_EXPECT_TRUE(test, integration.bound);
	KUNIT_EXPECT_TRUE(test, integration.mainline_registered);
	KUNIT_EXPECT_TRUE(test, owner.writer_registered);
	KUNIT_EXPECT_EQ(test, integration.bind_calls, 1U);
	KUNIT_EXPECT_EQ(test, integration.register_calls, 1U);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_forward_runtime_event(&coordinator,
			MT6797_DVFSP_VENDOR_RUNTIME_CPU_ONLINE,
			MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE), 0);
	KUNIT_EXPECT_TRUE(test, owner.source.invalidated);
	KUNIT_EXPECT_EQ(test, registration.runtime_event_calls, 1U);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_start(&coordinator, &owner,
			&mt6797_dvfsp_vendor_test_integration_ops, &integration), -EBUSY);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_stop(&coordinator), -EBUSY);
	KUNIT_ASSERT_EQ(test,
		((const struct mt6797_dvfsp_vendor_owner_lifecycle_ops *)
		 integration.lifecycle_ops)->unregister_writer(
		 integration.lifecycle_context), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_stop(&coordinator), 0);
	KUNIT_EXPECT_FALSE(test,
		mt6797_dvfsp_vendor_coordinator_started(&coordinator));
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_forward_runtime_event(&coordinator,
			MT6797_DVFSP_VENDOR_RUNTIME_CPU_ONLINE,
			MT6797_DVFSP_VENDOR_RUNTIME_CPU_NONE), -ENODEV);
	KUNIT_EXPECT_FALSE(test, integration.bound);
	KUNIT_EXPECT_EQ(test, integration.unbind_calls, 1U);
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_owner_unbind(&owner), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_detach(&resource_owner), 0);
	mt6797_dvfsp_vendor_test_put_devices(&devices);
}

static void mt6797_dvfsp_vendor_owner_vendor_caller_lifecycle(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_owner owner = { };
	struct mt6797_dvfsp_resource_owner resource_owner = { };
	struct mt6797_dvfsp_vendor_test_devices devices = { };
	struct mt6797_dvfsp_state_source_devices tuple = { };
	struct mt6797_dvfsp_vendor_test_registration registration = { };
	struct mt6797_dvfsp_vendor_test_integration integration = { };
	struct mt6797_dvfsp_vendor_coordinator coordinator = { };
	struct mt6797_dvfsp_vendor_owner_ops owner_ops =
		mt6797_dvfsp_vendor_test_owner_ops;

	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_vendor_probe(), -ENODEV);
	mt6797_dvfsp_vendor_test_init_devices(&devices);
	mt6797_dvfsp_vendor_test_tuple(&devices, &tuple);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_init(&resource_owner, 0x67970050), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_attach(&resource_owner, &tuple), 0);
	owner_ops.writer_runtime_ops = &mt6797_dvfsp_vendor_test_runtime_ops;
	owner_ops.writer_runtime_context = &registration;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_owner_bind(&owner, &resource_owner,
			&owner_ops, 0x67970051, 0x67970050), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_bind_caller(&coordinator, &owner,
			&mt6797_dvfsp_vendor_test_integration_ops, &integration), 0);
	KUNIT_EXPECT_TRUE(test,
		mt6797_dvfsp_vendor_coordinator_caller_bound(&coordinator));
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_caller_probe(&coordinator), 0);
	KUNIT_EXPECT_TRUE(test,
		mt6797_dvfsp_vendor_coordinator_started(&coordinator));
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_caller_probe(&coordinator), -EBUSY);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_caller_remove(&coordinator), -EBUSY);
	KUNIT_ASSERT_EQ(test,
		((const struct mt6797_dvfsp_vendor_owner_lifecycle_ops *)
		 integration.lifecycle_ops)->unregister_writer(
		 integration.lifecycle_context), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_caller_remove(&coordinator), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_coordinator_unbind_caller(&coordinator), 0);
	KUNIT_EXPECT_FALSE(test,
		mt6797_dvfsp_vendor_coordinator_caller_bound(&coordinator));
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_owner_unbind(&owner), 0);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_resource_owner_detach(&resource_owner), 0);
	mt6797_dvfsp_vendor_test_put_devices(&devices);
}

static struct kunit_case mt6797_dvfsp_vendor_owner_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_vendor_owner_rejects_incomplete),
	KUNIT_CASE(mt6797_dvfsp_vendor_owner_binds_and_unbinds),
	KUNIT_CASE(mt6797_dvfsp_vendor_owner_integrates_lifecycle),
	KUNIT_CASE(mt6797_dvfsp_vendor_owner_coordinator_stages_lifecycle),
	KUNIT_CASE(mt6797_dvfsp_vendor_owner_vendor_caller_lifecycle),
	{ }
};

static struct kunit_suite mt6797_dvfsp_vendor_owner_test_suite = {
	.name = "mt6797-dvfsp-vendor-owner",
	.test_cases = mt6797_dvfsp_vendor_owner_test_cases,
};

kunit_test_suite(mt6797_dvfsp_vendor_owner_test_suite);

MODULE_LICENSE("GPL");
