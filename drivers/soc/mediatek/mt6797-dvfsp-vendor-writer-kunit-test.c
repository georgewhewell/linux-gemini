// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free KUnit coverage for the cooperative MT6797 writer owner. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/irqflags.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer.h>

static int mt6797_dvfsp_vendor_writer_test_provenance(
	void *context, struct mt6797_dvfsp_state_provenance *provenance)
{
	unsigned int *calls = context;

	(*calls)++;
	memset(provenance, 0, sizeof(*provenance));
	provenance->abi = MT6797_DVFSP_STATE_PROVENANCE_ABI;
	provenance->source_mask = MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL;
	provenance->variant_id = 1;
	provenance->table_epoch = 2;
	provenance->calibration_handle = 3;
	return 0;
}

static const struct mt6797_dvfsp_vendor_writer_ops
mt6797_dvfsp_vendor_writer_test_ops = {
	.abi = MT6797_DVFSP_VENDOR_WRITER_ABI,
	.read_provenance = mt6797_dvfsp_vendor_writer_test_provenance,
};

struct mt6797_dvfsp_vendor_writer_test_update {
	struct mutex *lock;
	int result;
	u64 generation;
	unsigned int calls;
	bool owner_locked;
};

static int mt6797_dvfsp_vendor_writer_test_update(void *context,
		u64 generation)
{
	struct mt6797_dvfsp_vendor_writer_test_update *state = context;

	state->calls++;
	state->generation = generation;
	state->owner_locked = mutex_is_locked(state->lock);
	return state->result;
}

static void mt6797_dvfsp_vendor_writer_rejects_incomplete(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_writer writer = { };
	struct mutex lock;
	u64 shared_generation = 1;

	mutex_init(&lock);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock, &shared_generation,
			NULL, NULL, 1, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock, &shared_generation,
			&mt6797_dvfsp_vendor_writer_test_ops, NULL, 0, 2), -EINVAL);
	KUNIT_EXPECT_FALSE(test, writer.initialized);
}

static void mt6797_dvfsp_vendor_writer_serializes_generation(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_writer writer = { };
	struct mt6797_dvfsp_vendor_writer_ops bad_ops =
		mt6797_dvfsp_vendor_writer_test_ops;
	struct mt6797_dvfsp_state_provenance provenance = { };
	struct mt6797_dvfsp_clock_state clock = { };
	struct mt6797_dvfsp_calibration_state calibration = { };
	struct mt6797_dvfsp_cspm_state cspm = { };
	struct mt6797_dvfsp_ppm_snapshot ppm = { };
	struct mutex lock;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	unsigned int calls = 0;
	u64 shared_generation = 1;

	mutex_init(&lock);
	bad_ops.abi = 99;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock, &shared_generation,
			&bad_ops, &calls, 1, 2), -EINVAL);
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock, &shared_generation,
			&mt6797_dvfsp_vendor_writer_test_ops, &calls, 1, 2), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_read_generation(&writer, &clock,
			&calibration, &cspm, &ppm, &generation, &owner_handle,
			&transition_handle, &provenance), 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)1);
	local_irq_disable();
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&writer, &generation),
		-EWOULDBLOCK);
	local_irq_enable();
	KUNIT_EXPECT_EQ(test, owner_handle, (u64)1);
	KUNIT_EXPECT_EQ(test, transition_handle, (u64)2);
	KUNIT_EXPECT_EQ(test, calls, 1U);

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&writer, &generation), 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)1);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&writer, &generation), -EALREADY);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_commit(&writer), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_read_generation(&writer, &clock,
			&calibration, &cspm, &ppm, &generation, &owner_handle,
			&transition_handle, &provenance), 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)2);

	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&writer, &generation), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_abort(&writer), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_read_generation(&writer, &clock,
			&calibration, &cspm, &ppm, &generation, &owner_handle,
			&transition_handle, &provenance), 0);
	KUNIT_EXPECT_EQ(test, generation, (u64)2);

	shared_generation = ~0ULL;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_begin(&writer, &generation),
		-EOVERFLOW);
	KUNIT_EXPECT_FALSE(test, writer.update_held);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_commit(&writer), -EINVAL);
	KUNIT_EXPECT_EQ(test, shared_generation, ~0ULL);
}

static void mt6797_dvfsp_vendor_writer_runs_prelock_chain(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_writer writer = { };
	struct mt6797_dvfsp_vendor_writer_test_update state = { };
	struct mutex lock;
	u64 shared_generation = 1;

	mutex_init(&lock);
	state.lock = &lock;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock,
			&shared_generation,
			&mt6797_dvfsp_vendor_writer_test_ops, &state, 1, 2), 0);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run(&writer,
			mt6797_dvfsp_vendor_writer_test_update, &state), 0);
	KUNIT_EXPECT_EQ(test, state.calls, 1U);
	KUNIT_EXPECT_EQ(test, state.generation, (u64)1);
	KUNIT_EXPECT_TRUE(test, state.owner_locked);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)2);
	KUNIT_EXPECT_FALSE(test, writer.update_held);

	state.result = -EIO;
	state.owner_locked = false;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run(&writer,
			mt6797_dvfsp_vendor_writer_test_update, &state), -EIO);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)2);
	KUNIT_EXPECT_FALSE(test, writer.update_held);
}

static void mt6797_dvfsp_vendor_writer_runs_named_sites(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_writer writer = { };
	struct mt6797_dvfsp_vendor_writer_test_update state = { };
	struct mt6797_dvfsp_vendor_writer_site site = {
		.abi = MT6797_DVFSP_VENDOR_WRITER_SITE_ABI,
		.update = mt6797_dvfsp_vendor_writer_test_update,
	};
	struct mutex lock;
	u64 shared_generation = 1;

	mutex_init(&lock);
	state.lock = &lock;
	site.context = &state;
	KUNIT_ASSERT_EQ(test,
		mt6797_dvfsp_vendor_writer_init(&writer, &lock,
			&shared_generation,
			&mt6797_dvfsp_vendor_writer_test_ops, &state, 1, 2), 0);

	site.id = MT6797_DVFSP_VENDOR_WRITER_SITE_PTP_TABLE;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run_site(&writer, &site), 0);
	site.id = MT6797_DVFSP_VENDOR_WRITER_SITE_VOLTAGE;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run_site(&writer, &site), 0);
	KUNIT_EXPECT_EQ(test, state.calls, 2U);
	KUNIT_EXPECT_EQ(test, state.generation, (u64)2);
	KUNIT_EXPECT_TRUE(test, state.owner_locked);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)3);

	state.result = -EIO;
	site.id = MT6797_DVFSP_VENDOR_WRITER_SITE_PPM;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run_site(&writer, &site), -EIO);
	KUNIT_EXPECT_EQ(test, state.calls, 3U);
	KUNIT_EXPECT_EQ(test, shared_generation, (u64)3);
	KUNIT_EXPECT_FALSE(test, writer.update_held);

	site.id = MT6797_DVFSP_VENDOR_WRITER_SITE_COUNT;
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run_site(&writer, &site), -EINVAL);
	KUNIT_EXPECT_EQ(test, state.calls, 3U);
	KUNIT_EXPECT_EQ(test,
		mt6797_dvfsp_vendor_writer_run_site(&writer, NULL), -EINVAL);
}

static struct kunit_case mt6797_dvfsp_vendor_writer_test_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_vendor_writer_rejects_incomplete),
	KUNIT_CASE(mt6797_dvfsp_vendor_writer_serializes_generation),
	KUNIT_CASE(mt6797_dvfsp_vendor_writer_runs_prelock_chain),
	KUNIT_CASE(mt6797_dvfsp_vendor_writer_runs_named_sites),
	{ }
};

static struct kunit_suite mt6797_dvfsp_vendor_writer_test_suite = {
	.name = "mt6797-dvfsp-vendor-writer",
	.test_cases = mt6797_dvfsp_vendor_writer_test_cases,
};

kunit_test_suite(mt6797_dvfsp_vendor_writer_test_suite);

MODULE_LICENSE("GPL");
