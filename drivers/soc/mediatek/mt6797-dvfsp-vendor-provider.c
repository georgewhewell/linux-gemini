// SPDX-License-Identifier: GPL-2.0-only
/*
 * Map the bounded vendor observation ABI to an explicit provider review view.
 * This file performs no I/O and never registers a provider or owner.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-provider.h>

static int mt6797_dvfsp_vendor_provider_source_check(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	const struct mt6797_dvfsp_vendor_source_identity *identity,
	const struct mt6797_dvfsp_vendor_source_provenance *provenance,
	u64 owner_handle, u64 transition_handle)
{
	if (!source || !identity || source->abi !=
	    MT6797_DVFSP_VENDOR_SOURCE_ABI || source->source_mask !=
	    MT6797_DVFSP_VENDOR_SOURCE_ALL ||
	    source->ppm_cluster_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER ||
	    source->cpufreq_domain_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN ||
	    source->cspm_cluster_count !=
	    MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER + 1 ||
	    source->eem_detector_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR ||
	    !source->generation || source->owner_handle != owner_handle ||
	    source->transition_handle != transition_handle || !owner_handle ||
	    !transition_handle || identity->abi != MT6797_DVFSP_VENDOR_SOURCE_ABI ||
	    !identity->variant_id || !identity->table_epoch ||
	    !identity->calibration_handle || !provenance ||
	    provenance->abi != MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI ||
	    provenance->variant_id != identity->variant_id ||
	    provenance->table_epoch != identity->table_epoch ||
	    provenance->calibration_handle !=
		identity->calibration_handle ||
	    provenance->source_generation != source->generation ||
	    provenance->owner_handle != source->owner_handle ||
	    provenance->transition_handle != source->transition_handle)
		return -EPROTO;

	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_ppm(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	bool seen[MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER] = { 0 };
	u32 i;

	for (i = 0; i < source->ppm_cluster_count; i++) {
		const struct mt6797_dvfsp_vendor_source_ppm_cluster *cluster =
			&source->ppm[i];

		if (cluster->cluster_id >=
		    MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER ||
		    seen[cluster->cluster_id])
			return -EPROTO;
		seen[cluster->cluster_id] = true;
		mapped->ppm[cluster->cluster_id] = *cluster;
		mapped->ppm_source_index[cluster->cluster_id] = i;
	}

	for (i = 0; i < MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER; i++)
		if (!seen[i])
			return -EPROTO;

	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_cpufreq(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	bool seen[MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN] = { 0 };
	u32 i;

	for (i = 0; i < source->cpufreq_domain_count; i++) {
		const struct mt6797_dvfsp_vendor_source_cpufreq_domain *domain =
			&source->cpufreq[i];

		if (domain->id >= MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN ||
		    seen[domain->id])
			return -EPROTO;
		seen[domain->id] = true;
		mapped->cpufreq[domain->id] = *domain;
		mapped->cpufreq_source_index[domain->id] = i;
	}

	for (i = 0; i < MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN; i++)
		if (!seen[i])
			return -EPROTO;

	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_cspm(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	u32 i;

	/* The vendor record is already fixed LL/L/B/CCI order; do not infer IDs. */
	for (i = 0; i < source->cspm_cluster_count; i++) {
		const struct mt6797_dvfsp_vendor_source_cspm_cluster *cluster =
			&source->cspm[i];

		if (cluster->is_on > 1 || !cluster->volt || !cluster->vsram)
			return -EPROTO;
		if (i < MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER &&
		    (cluster->ceiling >= MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP ||
		     cluster->floor >= MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP ||
		     cluster->floor > cluster->ceiling))
			return -EPROTO;
		if (i == MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER &&
		    (cluster->ceiling || cluster->floor))
			return -EPROTO;
		mapped->cspm[i] = *cluster;
	}

	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_eem(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	static const u32 provider_ids[4] = { 0, 3, 4, 5 };
	bool seen[MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR] = { 0 };
	u32 i;

	for (i = 0; i < source->eem_detector_count; i++) {
		const struct mt6797_dvfsp_vendor_source_eem_detector *detector =
			&source->eem[i];

		if (detector->id >= MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR ||
		    seen[detector->id])
			return -EPROTO;
		seen[detector->id] = true;
		mapped->eem[detector->id] = *detector;
		mapped->eem_source_index[detector->id] = i;
	}

	for (i = 0; i < MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR; i++)
		if (!seen[i])
			return -EPROTO;
	for (i = 0; i < ARRAY_SIZE(provider_ids); i++) {
		if (!mapped->eem[provider_ids[i]].num_freq ||
		    mapped->eem[provider_ids[i]].num_freq >
			MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ)
			return -EPROTO;
		mapped->eem_provider_detector[i] = provider_ids[i];
	}

	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_policy(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	bool seen[MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW] = { 0 };
	u32 i;

	for (i = 0; i < source->policy_row_count; i++) {
		const struct mt6797_dvfsp_vendor_source_policy_row *row =
			&source->policy[i];
		u32 index;

		if (row->state >= MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_STATE ||
			row->cluster >= MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER)
			return -EPROTO;
		index = row->state * MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER +
			row->cluster;
		if (seen[index])
			return -EPROTO;
		seen[index] = true;
		mapped->policy[index] = *row;
		mapped->policy_source_index[index] = i;
	}
	for (i = 0; i < MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW; i++)
		if (!seen[i])
			return -EPROTO;
	mapped->policy_row_count = source->policy_row_count;
	return 0;
}

static int mt6797_dvfsp_vendor_provider_map_clock_rail(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	u32 i, j;

	for (i = 0; i < source->cpufreq_domain_count; i++) {
		const struct mt6797_dvfsp_vendor_source_cpufreq_domain *domain =
			&source->cpufreq[i];
		struct mt6797_dvfsp_vendor_source_cpufreq_domain *ordered =
			&mapped->cpufreq[domain->id];

		if (domain->clock_owner_kind !=
			MT6797_DVFSP_VENDOR_SOURCE_CLOCK_OWNER_CPU_DVFS ||
			domain->clock_owner_id != domain->id ||
			domain->clock_mux >= MT6797_DVFSP_VENDOR_SOURCE_MAX_CLOCK_MUX ||
			domain->clock_valid != 1 ||
			domain->voltage_unit !=
			MT6797_DVFSP_VENDOR_SOURCE_VOLTAGE_UNIT_10UV ||
			domain->vproc_mode != MT6797_DVFSP_VENDOR_SOURCE_VPROC_EXTBUCK ||
			domain->vproc_base_10uv !=
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_BASE_10UV ||
			domain->vproc_step_10uv !=
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_STEP_10UV ||
			domain->vproc_code_mask !=
			MT6797_DVFSP_VENDOR_SOURCE_VPROC_CODE_MASK ||
			(domain->vsram_mode != MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC &&
			 domain->vsram_mode != MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS))
			return -EPROTO;
		if (domain->vsram_mode == MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC &&
			(domain->vsram_base_10uv !=
			 MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BASE_10UV ||
			 domain->vsram_step_10uv !=
			 MT6797_DVFSP_VENDOR_SOURCE_VSRAM_STEP_10UV ||
			 domain->vsram_code_offset !=
			 MT6797_DVFSP_VENDOR_SOURCE_VSRAM_CODE_OFFSET ||
			 domain->vsram_code_mask != 0xf ||
			 domain->vsram_special_count !=
			 MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL))
			return -EPROTO;
		if (domain->vsram_mode == MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS &&
			(domain->vsram_base_10uv || domain->vsram_step_10uv ||
			 domain->vsram_code_offset || domain->vsram_code_mask ||
			 domain->vsram_special_count))
			return -EPROTO;
		if (domain->current_volt > U32_MAX / 10U ||
			domain->current_vsram > U32_MAX / 10U)
			return -EOVERFLOW;
		ordered->clock_owner_kind = domain->clock_owner_kind;
		ordered->clock_owner_id = domain->clock_owner_id;
		ordered->clock_mux = domain->clock_mux;
		ordered->clock_valid = domain->clock_valid;
		for (j = 0; j < domain->opp_count; j++) {
			if (domain->voltage[j] > U32_MAX / 10U ||
				domain->voltage_original[j] > U32_MAX / 10U)
				return -EOVERFLOW;
			mapped->cpufreq_voltage_uv[domain->id][j] =
				domain->voltage[j] * 10U;
			mapped->cpufreq_voltage_original_uv[domain->id][j] =
				domain->voltage_original[j] * 10U;
		}
		mapped->cpufreq_current_volt_uv[domain->id] =
			domain->current_volt * 10U;
		mapped->cpufreq_current_vsram_uv[domain->id] =
			domain->current_vsram * 10U;
	}
	return 0;
}


static int mt6797_dvfsp_vendor_provider_map_eem_uv(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	u32 i, j;

	for (i = 0; i < source->eem_detector_count; i++) {
		const struct mt6797_dvfsp_vendor_source_eem_detector *detector =
			&source->eem[i];

		if (detector->voltage_unit !=
			MT6797_DVFSP_VENDOR_SOURCE_EEM_VOLTAGE_UNIT_10UV ||
			!detector->eem_step || !detector->pmic_step ||
			detector->num_freq >
			MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ)
			return -EPROTO;

		mapped->eem_voltage_count[i] = detector->num_freq;
		for (j = 0; j < detector->num_freq; j++) {
			u64 voltage_10uv = (u64)detector->pmic_base +
				(u64)detector->volt_tbl_pmic[j] *
				detector->pmic_step;

			if (!voltage_10uv || voltage_10uv > U32_MAX / 10U)
				return -EOVERFLOW;
			mapped->eem_voltage_uv[i][j] =
				(u32)(voltage_10uv * 10U);
		}
	}

	return 0;
}

int mt6797_dvfsp_vendor_provider_map(
	const struct mt6797_dvfsp_vendor_source_snapshot *source,
	const struct mt6797_dvfsp_vendor_source_identity *identity,
	const struct mt6797_dvfsp_vendor_source_provenance *provenance,
	u64 owner_handle, u64 transition_handle,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	int ret;

	if (!mapped)
		return -EINVAL;
	ret = mt6797_dvfsp_vendor_provider_source_check(source, identity,
			provenance, owner_handle, transition_handle);
	if (ret)
		return ret;
	memset(mapped, 0, sizeof(*mapped));
	mapped->abi = MT6797_DVFSP_VENDOR_PROVIDER_ABI;
	mapped->mapped_mask = MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_ALL &
		~MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_EEM_UV;
	mapped->mapped_mask &=
		~(MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_POLICY_ROWS |
		  MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_CLOCK |
		  MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_RAIL);
	mapped->unavailable_mask =
		MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_ALL;
	mapped->ppm_table_type = source->ppm_table_type;
	mapped->generation = source->generation;
	mapped->owner_handle = source->owner_handle;
	mapped->transition_handle = source->transition_handle;
	mapped->identity = *identity;
	mapped->provenance = *provenance;

	ret = mt6797_dvfsp_vendor_provider_map_ppm(source, mapped);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_provider_map_policy(source, mapped);
	if (ret)
		return ret;
	mapped->mapped_mask |= MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_POLICY_ROWS;
	mapped->unavailable_mask &=
		~MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_POLICY_ROWS;
	ret = mt6797_dvfsp_vendor_provider_map_cpufreq(source, mapped);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_provider_map_clock_rail(source, mapped);
	if (ret)
		return ret;
	mapped->mapped_mask |= MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_CLOCK |
		MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_RAIL;
	mapped->unavailable_mask &=
		~(MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_CLOCK |
		  MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_RAIL);
	ret = mt6797_dvfsp_vendor_provider_map_cspm(source, mapped);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_provider_map_eem(source, mapped);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_provider_map_eem_uv(source, mapped);
	if (ret)
		return ret;
	mapped->mapped_mask |= MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_EEM_UV;
	mapped->unavailable_mask &=
		~MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_EEM_UV;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_provider_map);

static bool mt6797_dvfsp_vendor_provider_state_provenance_equal(
	const struct mt6797_dvfsp_state_provenance *left,
	const struct mt6797_dvfsp_state_provenance *right)
{
	return left->abi == right->abi &&
		left->source_mask == right->source_mask &&
		left->variant_id == right->variant_id &&
		left->table_epoch == right->table_epoch &&
		left->calibration_handle == right->calibration_handle;
}

static bool mt6797_dvfsp_vendor_provider_provenance_matches(
	const struct mt6797_dvfsp_vendor_provider_snapshot *mapped,
	const struct mt6797_dvfsp_state_provenance *provenance)
{
	return provenance->variant_id == mapped->identity.variant_id &&
		provenance->variant_id == mapped->provenance.variant_id &&
		provenance->table_epoch == mapped->identity.table_epoch &&
		provenance->table_epoch == mapped->provenance.table_epoch &&
		provenance->calibration_handle ==
			mapped->identity.calibration_handle &&
		provenance->calibration_handle ==
			mapped->provenance.calibration_handle;
}

int mt6797_dvfsp_vendor_provider_match_state(
	const struct mt6797_dvfsp_vendor_provider_snapshot *mapped,
	const struct mt6797_dvfsp_state_snapshot *snapshot,
	const struct mt6797_dvfsp_state_owner_identity *identity)
{
	if (!mapped || !snapshot || !identity)
		return -EINVAL;
	if (mapped->abi != MT6797_DVFSP_VENDOR_PROVIDER_ABI ||
	    mapped->mapped_mask != MT6797_DVFSP_VENDOR_PROVIDER_MAPPED_ALL ||
	    mapped->unavailable_mask !=
		MT6797_DVFSP_VENDOR_PROVIDER_UNAVAILABLE_REGISTRATION ||
	    !mapped->generation || !mapped->owner_handle ||
	    !mapped->transition_handle ||
	    mapped->identity.abi != MT6797_DVFSP_VENDOR_SOURCE_ABI ||
	    !mapped->identity.variant_id ||
	    !mapped->identity.table_epoch ||
	    !mapped->identity.calibration_handle ||
	    mapped->provenance.abi !=
		MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI ||
	    !mapped->provenance.variant_id ||
	    !mapped->provenance.table_epoch ||
	    !mapped->provenance.calibration_handle ||
	    snapshot->abi != MT6797_DVFSP_STATE_OWNER_ABI ||
	    !snapshot->generation || !snapshot->owner_handle ||
	    !snapshot->transition_handle || !snapshot->cluster_mask ||
	    snapshot->cluster_mask & ~MT6797_DVFSP_STATE_CLUSTER_MASK ||
	    snapshot->provenance.abi !=
		MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    snapshot->provenance.source_mask !=
		MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL ||
	    identity->abi != MT6797_DVFSP_STATE_OWNER_IDENTITY_ABI ||
	    identity->resource_mask !=
		MT6797_DVFSP_STATE_OWNER_RESOURCE_ALL ||
	    identity->cpu_pll_backend !=
		MT6797_DVFSP_STATE_OWNER_BACKEND_MCUMIXED_DVFSP ||
	    identity->big_cluster_backend !=
		MT6797_DVFSP_STATE_OWNER_BACKEND_BIGIDVFS_SMCCC ||
	    identity->reserved || !identity->owner_handle ||
	    !identity->transition_handle ||
	    identity->provenance.abi !=
		MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    identity->provenance.source_mask !=
		MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL)
		return -EPROTO;
	if (mapped->generation != snapshot->generation ||
	    mapped->provenance.source_generation != mapped->generation ||
	    mapped->owner_handle != snapshot->owner_handle ||
	    mapped->owner_handle != identity->owner_handle ||
	    mapped->owner_handle != mapped->provenance.owner_handle ||
	    mapped->transition_handle != snapshot->transition_handle ||
	    mapped->transition_handle != identity->transition_handle ||
	    mapped->transition_handle !=
		mapped->provenance.transition_handle ||
	    !mt6797_dvfsp_vendor_provider_state_provenance_equal(
		&snapshot->provenance, &identity->provenance) ||
	    !mt6797_dvfsp_vendor_provider_provenance_matches(mapped,
		&snapshot->provenance))
		return -EAGAIN;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_provider_match_state);

int mt6797_dvfsp_vendor_provider_bridge_init(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge,
	struct mt6797_dvfsp_calibrated_provider *provider,
	struct mt6797_dvfsp_vendor_source *source,
	u64 owner_handle, u64 transition_handle)
{
	if (!bridge || bridge->initialized || !provider ||
	    !provider->initialized || !source || !source->initialized ||
	    !owner_handle || !transition_handle ||
	    source->owner_handle != owner_handle ||
	    source->transition_handle != transition_handle)
		return -EINVAL;

	memset(bridge, 0, sizeof(*bridge));
	bridge->provider = provider;
	bridge->source = source;
	bridge->owner_handle = owner_handle;
	bridge->transition_handle = transition_handle;
	bridge->initialized = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_provider_bridge_init);

int mt6797_dvfsp_vendor_provider_bridge_snapshot(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge,
	struct mt6797_dvfsp_vendor_provider_snapshot *mapped)
{
	struct mt6797_dvfsp_vendor_source_snapshot *source;
	struct mt6797_dvfsp_vendor_source_identity identity;
	struct mt6797_dvfsp_vendor_source_provenance provenance;
	int ret;

	if (!bridge || !bridge->initialized || !bridge->provider ||
	    !bridge->provider->initialized || !bridge->source || !mapped)
		return -ENODEV;
	source = kzalloc_obj(*source);
	if (!source)
		return -ENOMEM;
	ret = mt6797_dvfsp_vendor_source_snapshot(bridge->source, source);
	if (ret)
		goto out_free;
	memset(&identity, 0, sizeof(identity));
	ret = mt6797_dvfsp_vendor_source_identity(bridge->source, &identity);
	if (ret)
		goto out_free;
	memset(&provenance, 0, sizeof(provenance));
	ret = mt6797_dvfsp_vendor_source_provenance(bridge->source,
		source, &provenance);
	if (ret)
		goto out_free;
	ret = mt6797_dvfsp_vendor_provider_map(source, &identity,
		&provenance, bridge->owner_handle,
		bridge->transition_handle, mapped);

out_free:
	kfree(source);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_provider_bridge_snapshot);

void mt6797_dvfsp_vendor_provider_bridge_exit(
	struct mt6797_dvfsp_vendor_provider_bridge *bridge)
{
	if (bridge)
		memset(bridge, 0, sizeof(*bridge));
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_provider_bridge_exit);

MODULE_DESCRIPTION("MT6797 vendor source provider review bridge");
MODULE_LICENSE("GPL");
