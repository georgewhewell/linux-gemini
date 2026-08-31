// SPDX-License-Identifier: GPL-2.0-only
/* Hardware-free mapping and lifecycle tests for the vendor provider bridge. */

#include <linux/errno.h>
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-provider.h>

static struct mt6797_dvfsp_vendor_source_snapshot
	mt6797_dvfsp_vendor_provider_test_snapshot;
static struct mt6797_dvfsp_vendor_source_identity
	mt6797_dvfsp_vendor_provider_test_identity;
static struct mt6797_dvfsp_vendor_source_provenance
	mt6797_dvfsp_vendor_provider_test_provenance;

static int mt6797_dvfsp_vendor_provider_test_read_snapshot(
	void *context, struct mt6797_dvfsp_vendor_source_snapshot *snapshot)
{
	*snapshot = mt6797_dvfsp_vendor_provider_test_snapshot;
	return 0;
}

static int mt6797_dvfsp_vendor_provider_test_read_identity(
	void *context, struct mt6797_dvfsp_vendor_source_identity *identity)
{
	*identity = mt6797_dvfsp_vendor_provider_test_identity;
	return 0;
}

static int mt6797_dvfsp_vendor_provider_test_read_observation(
	void *context,
	struct mt6797_dvfsp_vendor_source_identity_observation *observation)
{
	memset(observation, 0, sizeof(*observation));
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

static int mt6797_dvfsp_vendor_provider_test_read_provenance(
	void *context,
	const struct mt6797_dvfsp_vendor_source_snapshot *snapshot,
	struct mt6797_dvfsp_vendor_source_provenance *provenance)
{
	*provenance = mt6797_dvfsp_vendor_provider_test_provenance;
	return 0;
}

static const struct mt6797_dvfsp_vendor_source_ops
mt6797_dvfsp_vendor_provider_test_source_ops = {
	.abi = MT6797_DVFSP_VENDOR_SOURCE_ABI,
	.snapshot = mt6797_dvfsp_vendor_provider_test_read_snapshot,
	.read_identity = mt6797_dvfsp_vendor_provider_test_read_identity,
	.read_identity_observation =
		mt6797_dvfsp_vendor_provider_test_read_observation,
	.read_provenance =
		mt6797_dvfsp_vendor_provider_test_read_provenance,
};

static void mt6797_dvfsp_vendor_provider_test_fill(void)
{
	struct mt6797_dvfsp_vendor_source_snapshot *snapshot =
		&mt6797_dvfsp_vendor_provider_test_snapshot;
	u32 i;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->abi = MT6797_DVFSP_VENDOR_SOURCE_ABI;
	snapshot->source_mask = MT6797_DVFSP_VENDOR_SOURCE_ALL;
	snapshot->ppm_table_type = 1;
	snapshot->ppm_cluster_count =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER;
	snapshot->cpufreq_domain_count =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN;
	snapshot->cspm_cluster_count =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER + 1;
	snapshot->eem_detector_count =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR;
	snapshot->policy_row_count =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW;
	for (i = 0; i < snapshot->policy_row_count; i++) {
		snapshot->policy[i].state =
			i / MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER;
		snapshot->policy[i].cluster =
			i % MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER;
		snapshot->policy[i].max_cpufreq_idx = 15;
		snapshot->policy[i].max_cpu_core = 4;
	}
	snapshot->generation = 9;
	snapshot->owner_handle = 0x67970023;
	snapshot->transition_handle = 0x67970022;
	for (i = 0; i < snapshot->ppm_cluster_count; i++) {
		struct mt6797_dvfsp_vendor_source_ppm_cluster *cluster =
			&snapshot->ppm[i];
		cluster->cluster_id = 2 - i;
		cluster->core_num = i + 1;
		cluster->cpu_id = i * 4;
		cluster->opp_count = 2;
		cluster->frequency_khz[0] = 100000 + i * 10000;
		cluster->frequency_khz[1] = 50000 + i * 10000;
	}
	for (i = 0; i < snapshot->cpufreq_domain_count; i++) {
		struct mt6797_dvfsp_vendor_source_cpufreq_domain *domain =
			&snapshot->cpufreq[i];
		domain->id = 3 - i;
		domain->opp_count = 2;
		domain->current_freq_khz = 100000 + i;
		domain->current_volt = 800000 + i;
		domain->current_vsram = 1000000 + i;
		domain->frequency_khz[0] = 100000 + i;
		domain->frequency_khz[1] = 50000 + i;
		domain->voltage[0] = 800000 + i;
		domain->voltage[1] = 810000 + i;
		domain->voltage_original[0] = 800000 + i;
		domain->voltage_original[1] = 810000 + i;
		domain->clock_owner_kind =
			MT6797_DVFSP_VENDOR_SOURCE_CLOCK_OWNER_CPU_DVFS;
		domain->clock_owner_id = domain->id;
		domain->clock_mux = i;
		domain->clock_valid = 1;
		domain->voltage_unit =
			MT6797_DVFSP_VENDOR_SOURCE_VOLTAGE_UNIT_10UV;
		domain->vproc_mode =
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_EXTBUCK;
		domain->vproc_base_10uv =
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_BASE_10UV;
		domain->vproc_step_10uv =
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_STEP_10UV;
		domain->vproc_code_mask =
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_CODE_MASK;
		if (domain->id == 2) {
			domain->vsram_mode =
				MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS;
		} else {
			domain->vsram_mode =
				MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC;
			domain->vsram_base_10uv =
				MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BASE_10UV;
			domain->vsram_step_10uv =
				MT6797_DVFSP_VENDOR_SOURCE_VSRAM_STEP_10UV;
			domain->vsram_code_offset =
				MT6797_DVFSP_VENDOR_SOURCE_VSRAM_CODE_OFFSET;
			domain->vsram_code_mask = 0xf;
			domain->vsram_special_count =
				MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL;
			domain->vsram_special_code[0] = 0;
			domain->vsram_special_code[1] = 1;
			domain->vsram_special_code[2] = 2;
			domain->vsram_special_10uv[0] = 105000;
			domain->vsram_special_10uv[1] = 60000;
			domain->vsram_special_10uv[2] = 70000;
		}
	}
	for (i = 0; i < snapshot->cspm_cluster_count; i++) {
		snapshot->cspm[i].firmware_opp = i;
		snapshot->cspm[i].opp = i + 1;
		snapshot->cspm[i].volt = 10 + i;
		snapshot->cspm[i].vsram = 20 + i;
		if (i < MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER) {
			snapshot->cspm[i].floor = 0;
			snapshot->cspm[i].ceiling = 1;
		}
	}
	for (i = 0; i < snapshot->eem_detector_count; i++) {
		snapshot->eem[i].id = i;
		snapshot->eem[i].num_freq = 2;
		snapshot->eem[i].frequency_percent[0] = 100;
		snapshot->eem[i].frequency_percent[1] = 50;
		snapshot->eem[i].volt_tbl[0] = 10 + i;
		snapshot->eem[i].volt_tbl[1] = 11 + i;
		snapshot->eem[i].volt_tbl_pmic[0] = 70 + i;
		snapshot->eem[i].volt_tbl_pmic[1] = 71 + i;
		snapshot->eem[i].voltage_unit =
			MT6797_DVFSP_VENDOR_SOURCE_EEM_VOLTAGE_UNIT_10UV;
		snapshot->eem[i].eem_v_base = 70000;
		snapshot->eem[i].eem_step = 625;
		snapshot->eem[i].pmic_base = 30000;
		snapshot->eem[i].pmic_step = 1000;
	}
	memset(&mt6797_dvfsp_vendor_provider_test_identity, 0,
	       sizeof(mt6797_dvfsp_vendor_provider_test_identity));
	mt6797_dvfsp_vendor_provider_test_identity.abi =
		MT6797_DVFSP_VENDOR_SOURCE_ABI;
	mt6797_dvfsp_vendor_provider_test_identity.variant_id = 7;
	mt6797_dvfsp_vendor_provider_test_identity.table_epoch = 0x100000002ULL;
	mt6797_dvfsp_vendor_provider_test_identity.calibration_handle =
		0x6797abcdULL;
	memset(&mt6797_dvfsp_vendor_provider_test_provenance, 0,
	       sizeof(mt6797_dvfsp_vendor_provider_test_provenance));
	mt6797_dvfsp_vendor_provider_test_provenance.abi =
		MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI;
	mt6797_dvfsp_vendor_provider_test_provenance.variant_id = 7;
	mt6797_dvfsp_vendor_provider_test_provenance.table_epoch =
		0x100000002ULL;
	mt6797_dvfsp_vendor_provider_test_provenance.calibration_handle =
		0x6797abcdULL;
	mt6797_dvfsp_vendor_provider_test_provenance.source_generation = 9;
	mt6797_dvfsp_vendor_provider_test_provenance.owner_handle =
		0x67970023;
	mt6797_dvfsp_vendor_provider_test_provenance.transition_handle =
		0x67970022;
}

static void mt6797_dvfsp_vendor_provider_maps_fields(struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;

	mt6797_dvfsp_vendor_provider_test_fill();
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), 0);
	KUNIT_EXPECT_EQ(test, mapped.abi,
		(u32)MT6797_DVFSP_VENDOR_PROVIDER_ABI);
	KUNIT_EXPECT_EQ(test, mapped.mapped_mask,
		(u32)MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_ALL);
	KUNIT_EXPECT_EQ(test, mapped.unavailable_mask,
		(u32)(MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_ALL &
		~(MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_EEM_UV |
		  MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_POLICY_ROWS |
		  MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_CLOCK |
		  MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_RAIL)));
	KUNIT_EXPECT_EQ(test, mapped.ppm[0].cluster_id, 0U);
	KUNIT_EXPECT_EQ(test, mapped.ppm[2].cluster_id, 2U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq[0].id, 0U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq[3].id, 3U);
	KUNIT_EXPECT_EQ(test, mapped.policy_row_count,
		(u32)MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW);
	KUNIT_EXPECT_EQ(test, mapped.policy[0].state, 0U);
	KUNIT_EXPECT_EQ(test, mapped.policy[0].cluster, 0U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq[0].clock_owner_id, 0U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq[0].clock_mux, 3U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq_voltage_uv[0][0], 8000030U);
	KUNIT_EXPECT_EQ(test, mapped.cpufreq_current_vsram_uv[0], 10000030U);
	KUNIT_EXPECT_EQ(test, mapped.cspm[3].floor, 0U);
	KUNIT_EXPECT_EQ(test, mapped.eem_provider_detector[0], 0U);
	KUNIT_EXPECT_EQ(test, mapped.eem_provider_detector[1], 3U);
	KUNIT_EXPECT_EQ(test, mapped.eem_provider_detector[2], 4U);
	KUNIT_EXPECT_EQ(test, mapped.eem_provider_detector[3], 5U);
	KUNIT_EXPECT_EQ(test, mapped.eem_voltage_count[0], 2U);
	KUNIT_EXPECT_EQ(test, mapped.eem_voltage_uv[0][0], 1000000U);
	KUNIT_EXPECT_EQ(test, mapped.identity.table_epoch, 0x100000002ULL);
	KUNIT_EXPECT_EQ(test, mapped.identity.calibration_handle,
		0x6797abcdULL);
	KUNIT_EXPECT_EQ(test, mapped.provenance.source_generation, 9ULL);
	KUNIT_EXPECT_EQ(test, mapped.provenance.table_epoch,
		0x100000002ULL);
	KUNIT_EXPECT_EQ(test, mapped.provenance.calibration_handle,
		0x6797abcdULL);
}

static void mt6797_dvfsp_vendor_provider_matches_state_identity(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped;
	struct mt6797_dvfsp_state_snapshot snapshot = {
		.abi = MT6797_DVFSP_STATE_OWNER_ABI,
		.cluster_mask = MT6797_DVFSP_STATE_CLUSTER_MASK,
		.generation = 9,
		.owner_handle = 0x67970023,
		.transition_handle = 0x67970022,
		.provenance = {
			.abi = MT6797_DVFSP_STATE_PROVENANCE_ABI,
			.source_mask =
				MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL,
			.variant_id = 7,
			.table_epoch = 0x100000002ULL,
			.calibration_handle = 0x6797abcdULL,
		},
	};
	struct mt6797_dvfsp_state_owner_identity identity = {
		.abi = MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI,
		.resource_mask = MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL,
		.cpu_pll_backend =
			MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP,
		.big_cluster_backend =
			MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC,
		.owner_handle = 0x67970023,
		.transition_handle = 0x67970022,
	};

	mapped = kunit_kzalloc(test, sizeof(*mapped), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mapped);
	mt6797_dvfsp_vendor_provider_test_fill();
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, mapped), 0);
	identity.provenance = snapshot.provenance;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_match_state(
		mapped, &snapshot, &identity), 0);
	snapshot.generation++;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_match_state(
		mapped, &snapshot, &identity), -EAGAIN);
	snapshot.generation--;
	identity.provenance.table_epoch++;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_match_state(
		mapped, &snapshot, &identity), -EAGAIN);
}

static void mt6797_dvfsp_vendor_provider_rejects_identity_or_ids(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;

	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_identity.variant_id = 0;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_snapshot.ppm[0].cluster_id = 3;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
}

static void mt6797_dvfsp_vendor_provider_rejects_provenance_mismatch(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;

	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_provenance.table_epoch++;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_provenance.source_generation++;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
}

static void mt6797_dvfsp_vendor_provider_rejects_bad_clock_or_rail(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;

	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_snapshot.cpufreq[0].clock_mux =
		MT6797_DVFSP_VENDOR_SOURCE_MAX_CLOCK_MUX;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
}

static void mt6797_dvfsp_vendor_provider_rejects_bad_eem_units(
	struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;

	mt6797_dvfsp_vendor_provider_test_fill();
	mt6797_dvfsp_vendor_provider_test_snapshot.eem[0].voltage_unit = 0;
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_map(
		&mt6797_dvfsp_vendor_provider_test_snapshot,
		&mt6797_dvfsp_vendor_provider_test_identity,
		&mt6797_dvfsp_vendor_provider_test_provenance,
		0x67970023, 0x67970022, &mapped), -EPROTO);
}

static void mt6797_dvfsp_vendor_provider_bridge_lifecycle(struct kunit *test)
{
	struct mt6797_dvfsp_vendor_provider_bridge bridge;
	struct mt6797_dvfsp_vendor_provider_snapshot mapped;
	struct mt6797_dvfsp_vendor_source source;
	struct mt6797_dvfsp_calibrated_provider provider;

	mt6797_dvfsp_vendor_provider_test_fill();
	memset(&source, 0, sizeof(source));
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_source_init(&source,
		&mt6797_dvfsp_vendor_provider_test_source_ops, &source,
		0x67970023, 0x67970022), 0);
	memset(&provider, 0, sizeof(provider));
	provider.initialized = true;
	memset(&bridge, 0, sizeof(bridge));
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_provider_bridge_init(&bridge,
		&provider, &source, 0x67970023, 0x67970022), 0);
	KUNIT_ASSERT_EQ(test, mt6797_dvfsp_vendor_provider_bridge_snapshot(
		&bridge, &mapped), 0);
	KUNIT_EXPECT_EQ(test, mapped.generation, 9ULL);
	mt6797_dvfsp_vendor_source_invalidate(&source);
	KUNIT_EXPECT_EQ(test, mt6797_dvfsp_vendor_provider_bridge_snapshot(
		&bridge, &mapped), -ENODEV);
	mt6797_dvfsp_vendor_provider_bridge_exit(&bridge);
	KUNIT_EXPECT_FALSE(test, bridge.initialized);
	mt6797_dvfsp_vendor_source_exit(&source);
}

static struct kunit_case mt6797_dvfsp_vendor_provider_cases[] = {
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_maps_fields),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_matches_state_identity),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_rejects_identity_or_ids),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_rejects_provenance_mismatch),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_rejects_bad_eem_units),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_rejects_bad_clock_or_rail),
	KUNIT_CASE(mt6797_dvfsp_vendor_provider_bridge_lifecycle),
	{}
};

static struct kunit_suite mt6797_dvfsp_vendor_provider_suite = {
	.name = "mt6797-dvfsp-vendor-provider",
	.test_cases = mt6797_dvfsp_vendor_provider_cases,
};

kunit_test_suite(mt6797_dvfsp_vendor_provider_suite);
MODULE_LICENSE("GPL");
