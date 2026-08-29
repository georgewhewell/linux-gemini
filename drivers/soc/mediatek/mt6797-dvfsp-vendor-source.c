// SPDX-License-Identifier: GPL-2.0-only
/*
 * Validate source-backed snapshots supplied by the external vendor adapter.
 * This object copies no vendor source and performs no hardware operation.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-source.h>

static int mt6797_dvfsp_vendor_source_ops_check(
	const struct mt6797_dvfsp_vendor_source_ops *ops)
{
	if (!ops || ops->abi != MT6797_DVFSP_VENDOR_SOURCE_ABI ||
		!ops->snapshot || !ops->read_identity ||
		!ops->read_identity_observation || !ops->read_provenance)
		return -EINVAL;
	return 0;
}

static int mt6797_dvfsp_vendor_source_table_check(
	const struct mt6797_dvfsp_vendor_source_snapshot *snapshot)
{
	unsigned int i;

	if (!snapshot || snapshot->abi != MT6797_DVFSP_VENDOR_SOURCE_ABI ||
		snapshot->source_mask != MT6797_DVFSP_VENDOR_SOURCE_ALL ||
		snapshot->ppm_cluster_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER ||
		snapshot->cpufreq_domain_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN ||
		snapshot->cspm_cluster_count !=
			MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER + 1 ||
		snapshot->eem_detector_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_DETECTOR ||
		snapshot->policy_row_count != MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_ROW ||
		!snapshot->generation || !snapshot->owner_handle ||
		!snapshot->transition_handle)
		return -EPROTO;

	for (i = 0; i < snapshot->policy_row_count; i++) {
		const struct mt6797_dvfsp_vendor_source_policy_row *row =
			&snapshot->policy[i];

		if (row->state >= MT6797_DVFSP_VENDOR_SOURCE_MAX_POLICY_STATE ||
			row->cluster >= MT6797_DVFSP_VENDOR_SOURCE_MAX_CLUSTER ||
			row->min_cpufreq_idx < -1 ||
			row->max_cpufreq_idx < -1 ||
			row->min_cpufreq_idx >=
			MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP ||
			row->max_cpufreq_idx >=
			MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP ||
			row->min_cpu_core > row->max_cpu_core)
			return -EPROTO;
	}

	for (i = 0; i < snapshot->ppm_cluster_count; i++) {
		const struct mt6797_dvfsp_vendor_source_ppm_cluster *cluster =
			&snapshot->ppm[i];
		unsigned int row;

		if (!cluster->opp_count ||
			cluster->opp_count > MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP)
			return -EPROTO;
		for (row = 0; row < cluster->opp_count; row++)
			if (!cluster->frequency_khz[row])
				return -EPROTO;
	}

	for (i = 0; i < snapshot->cpufreq_domain_count; i++) {
		const struct mt6797_dvfsp_vendor_source_cpufreq_domain *domain =
			&snapshot->cpufreq[i];
		unsigned int row;

		if (domain->id >= MT6797_DVFSP_VENDOR_SOURCE_MAX_DOMAIN ||
			domain->clock_owner_kind !=
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
			 domain->vsram_mode != MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS) ||
			(domain->vsram_mode == MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC &&
			 (domain->vsram_base_10uv !=
			  MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BASE_10UV ||
			  domain->vsram_step_10uv !=
			  MT6797_DVFSP_VENDOR_SOURCE_VSRAM_STEP_10UV ||
			  domain->vsram_code_offset !=
			  MT6797_DVFSP_VENDOR_SOURCE_VSRAM_CODE_OFFSET ||
			  domain->vsram_code_mask != 0xf ||
			  domain->vsram_special_count !=
			  MT6797_DVFSP_VENDOR_SOURCE_MAX_VSRAM_SPECIAL)) ||
			(domain->vsram_mode == MT6797_DVFSP_VENDOR_SOURCE_VSRAM_BIGIDVFS &&
			 (domain->vsram_base_10uv || domain->vsram_step_10uv ||
			  domain->vsram_code_offset || domain->vsram_code_mask ||
			  domain->vsram_special_count)) ||
			!domain->opp_count ||
			domain->opp_count > MT6797_DVFSP_VENDOR_SOURCE_MAX_OPP ||
			!domain->current_freq_khz || !domain->current_volt ||
			!domain->current_vsram ||
			domain->current_volt > U32_MAX / 10U ||
			domain->current_vsram > U32_MAX / 10U)
			return -EPROTO;
		for (row = 0; row < domain->opp_count; row++)
			if (!domain->frequency_khz[row] || !domain->voltage[row] ||
				!domain->voltage_original[row] ||
				domain->voltage[row] > U32_MAX / 10U ||
				domain->voltage_original[row] > U32_MAX / 10U)
				return -EPROTO;
		if (domain->vsram_mode == MT6797_DVFSP_VENDOR_SOURCE_VSRAM_PMIC &&
			(domain->vsram_special_code[0] != 0 ||
			 domain->vsram_special_code[1] != 1 ||
			 domain->vsram_special_code[2] != 2 ||
			 domain->vsram_special_10uv[0] != 105000 ||
			 domain->vsram_special_10uv[1] != 60000 ||
			 domain->vsram_special_10uv[2] != 70000))
			return -EPROTO;
	}

	for (i = 0; i < snapshot->cspm_cluster_count; i++) {
		const struct mt6797_dvfsp_vendor_source_cspm_cluster *cluster =
			&snapshot->cspm[i];

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
	}

	for (i = 0; i < snapshot->eem_detector_count; i++) {
		const struct mt6797_dvfsp_vendor_source_eem_detector *det =
			&snapshot->eem[i];

		if (det->num_freq > MT6797_DVFSP_VENDOR_SOURCE_MAX_FREQ)
			return -EPROTO;
	}

	return 0;
}

int mt6797_dvfsp_vendor_source_init(
	struct mt6797_dvfsp_vendor_source *source,
	const struct mt6797_dvfsp_vendor_source_ops *ops, void *context,
	u64 owner_handle, u64 transition_handle)
{
	if (!source || source->initialized || !context || !owner_handle ||
		!transition_handle || mt6797_dvfsp_vendor_source_ops_check(ops))
		return -EINVAL;

	memset(source, 0, sizeof(*source));
	source->ops = ops;
	source->context = context;
	source->owner_handle = owner_handle;
	source->transition_handle = transition_handle;
	source->initialized = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_init);

int mt6797_dvfsp_vendor_source_snapshot(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_snapshot *snapshot)
{
	int ret;

	if (!source || !source->initialized || source->invalidated || !snapshot)
		return -ENODEV;
	ret = source->ops->snapshot(source->context, snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_vendor_source_table_check(snapshot);
	if (ret)
		return ret;
	if (snapshot->owner_handle != source->owner_handle ||
		snapshot->transition_handle != source->transition_handle)
		return -ESTALE;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_snapshot);

int mt6797_dvfsp_vendor_source_identity(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_identity *identity)
{
	int ret;

	if (!source || !source->initialized || source->invalidated || !identity)
		return -ENODEV;
	memset(identity, 0, sizeof(*identity));
	ret = source->ops->read_identity(source->context, identity);
	if (ret)
		return ret;
	if (identity->abi != MT6797_DVFSP_VENDOR_SOURCE_ABI ||
		!identity->variant_id || !identity->table_epoch ||
		!identity->calibration_handle)
		return -EPROTO;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_identity);

int mt6797_dvfsp_vendor_source_identity_observation(
	struct mt6797_dvfsp_vendor_source *source,
	struct mt6797_dvfsp_vendor_source_identity_observation *observation)
{
	int ret;

	if (!source || !source->initialized || source->invalidated ||
		!observation)
		return -ENODEV;
	memset(observation, 0, sizeof(*observation));
	ret = source->ops->read_identity_observation(source->context,
		observation);
	if (ret)
		return ret;
	if (observation->abi !=
		MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_OBSERVATION_ABI ||
		observation->source_mask !=
		MT6797_DVFSP_VENDOR_SOURCE_IDENTITY_ALL ||
		observation->eem_ate_version > 0xf ||
		observation->cpu_bin_level > 0xf)
		return -EPROTO;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_identity_observation);

int mt6797_dvfsp_vendor_source_provenance(
	struct mt6797_dvfsp_vendor_source *source,
	const struct mt6797_dvfsp_vendor_source_snapshot *snapshot,
	struct mt6797_dvfsp_vendor_source_provenance *provenance)
{
	int ret;

	if (!source || !source->initialized || source->invalidated ||
		!snapshot || !provenance)
		return -ENODEV;
	ret = mt6797_dvfsp_vendor_source_table_check(snapshot);
	if (ret)
		return ret;
	memset(provenance, 0, sizeof(*provenance));
	ret = source->ops->read_provenance(source->context, snapshot,
		provenance);
	if (ret)
		return ret;
	if (provenance->abi !=
		MT6797_DVFSP_VENDOR_SOURCE_PROVENANCE_ABI ||
		!provenance->variant_id || !provenance->table_epoch ||
		!provenance->calibration_handle ||
		provenance->source_generation != snapshot->generation ||
		provenance->owner_handle != snapshot->owner_handle ||
		provenance->transition_handle != snapshot->transition_handle)
		return -EPROTO;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_provenance);

void mt6797_dvfsp_vendor_source_invalidate(
	struct mt6797_dvfsp_vendor_source *source)
{
	if (source)
		source->invalidated = true;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_invalidate);

void mt6797_dvfsp_vendor_source_exit(
	struct mt6797_dvfsp_vendor_source *source)
{
	if (source)
		memset(source, 0, sizeof(*source));
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_vendor_source_exit);

MODULE_DESCRIPTION("MT6797 cross-tree vendor source observation");
MODULE_LICENSE("GPL");
