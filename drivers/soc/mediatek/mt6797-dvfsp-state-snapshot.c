// SPDX-License-Identifier: GPL-2.0-only
/*
 * Assemble the source-backed MT6797 protected startup-state snapshot.
 *
 * This file deliberately has no device, clock, regulator, or secure-monitor
 * access. It only joins already validated conversion results with the live
 * fields supplied by a future owner while that owner holds its transition
 * lock.
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-state-snapshot.h>

static int mt6797_dvfsp_snapshot_provenance_check(
	const struct mt6797_dvfsp_state_provenance *provenance)
{
	if (!provenance ||
	    provenance->abi != MT6797_DVFSP_STATE_PROVENANCE_ABI ||
	    provenance->source_mask != MT6797_DVFSP_STATE_PROVENANCE_SOURCE_ALL ||
	    !provenance->table_epoch || !provenance->calibration_handle)
		return -EPROTO;

	return 0;
}

static bool mt6797_dvfsp_snapshot_provenance_equal(
	const struct mt6797_dvfsp_state_provenance *left,
	const struct mt6797_dvfsp_state_provenance *right)
{
	return left->abi == right->abi &&
		left->source_mask == right->source_mask &&
		left->variant_id == right->variant_id &&
		left->table_epoch == right->table_epoch &&
		left->calibration_handle == right->calibration_handle;
}

static int mt6797_dvfsp_snapshot_table_match(
	const struct mt6797_dvfsp_calibration_cluster *cluster,
	u32 frequency_khz,
	const struct mt6797_dvfsp_calibration_table_entry **matched_entry)
{
	unsigned int i;

	if (!cluster || !matched_entry || !frequency_khz ||
	    !cluster->table_count ||
	    cluster->table_count > MT6797_DVFSP_CALIBRATION_TABLE_MAX)
		return -EPROTO;
	*matched_entry = NULL;

	for (i = 0; i < cluster->table_count; i++) {
		const struct mt6797_dvfsp_calibration_table_entry *entry =
			&cluster->table[i];

		if (!entry->frequency_khz || !entry->vproc_uv ||
		    !entry->vsram_uv || !entry->ppm_limit_khz ||
		    entry->vsram_uv < entry->vproc_uv ||
		    entry->vsram_uv - entry->vproc_uv < 10000 ||
		    entry->vsram_uv - entry->vproc_uv > 30000)
			return -EPROTO;
		if (entry->frequency_khz == frequency_khz) {
			*matched_entry = entry;
			return 0;
		}
	}

	return -EAGAIN;
}

static int mt6797_dvfsp_snapshot_cluster_check(
	const struct mt6797_dvfsp_state_snapshot_input *input,
	unsigned int cluster)
{
	const struct mt6797_dvfsp_calibration_cluster *calibration;
	const struct mt6797_dvfsp_clock_state_cluster *clock;
	const struct mt6797_dvfsp_calibration_table_entry *matched_entry;
	int ret;

	if (input->state_flags[cluster] &
	    ~(MT6797_DVFSP_STATE_FLAG_PRESENT | MT6797_DVFSP_STATE_FLAG_ON) ||
	    !(input->state_flags[cluster] & MT6797_DVFSP_STATE_FLAG_PRESENT) ||
	    !input->frequency_khz[cluster] ||
	    !input->voltage_uv[cluster] || !input->vsram_uv[cluster] ||
	    !input->ceiling_khz[cluster] || !input->floor_khz[cluster] ||
	    input->floor_khz[cluster] > input->ceiling_khz[cluster] ||
	    input->frequency_khz[cluster] < input->floor_khz[cluster] ||
	    input->frequency_khz[cluster] > input->ceiling_khz[cluster])
		return -EPROTO;

	clock = &input->clock->cluster[cluster];
	if (clock->frequency_khz != input->frequency_khz[cluster])
		return -EAGAIN;

	calibration = &input->calibration->cluster[cluster];
	ret = mt6797_dvfsp_snapshot_table_match(calibration,
		input->frequency_khz[cluster], &matched_entry);
	if (ret)
		return ret;
	if (input->voltage_uv[cluster] != matched_entry->vproc_uv ||
	    input->vsram_uv[cluster] != matched_entry->vsram_uv)
		return -EAGAIN;

	return 0;
}

int mt6797_dvfsp_state_snapshot_assemble(
	const struct mt6797_dvfsp_state_snapshot_input *input,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	unsigned int i;
	int ret;

	if (!input || !snapshot || input->abi !=
	    MT6797_DVFSP_STATE_SNAPSHOT_ABI || input->reserved ||
	    !input->generation || !input->owner_handle ||
	    !input->transition_handle || !input->clock || !input->calibration)
		return -EINVAL;

	if (input->clock->abi != MT6797_DVFSP_CLOCK_STATE_ABI ||
	    !input->clock->clock_sample_generation ||
	    !input->clock->big_sample_generation ||
	    input->calibration->abi != MT6797_DVFSP_CALIBRATION_STATE_ABI ||
	    input->calibration->phase !=
		MT6797_DVFSP_CALIBRATION_STATE_PHASE_MON ||
	    input->calibration->bank_mask !=
		MT6797_DVFSP_CALIBRATION_STATE_BANK_ALL ||
	    !input->calibration->thermal_generation ||
	    !input->calibration->clock_owner_generation ||
	    !input->calibration->rail_owner_generation ||
	    !input->calibration->source_generation ||
	    input->generation != input->calibration->source_generation ||
	    mt6797_dvfsp_snapshot_provenance_check(&input->provenance) ||
	    !mt6797_dvfsp_snapshot_provenance_equal(&input->provenance,
		&input->calibration->provenance))
		return -EPROTO;

	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		ret = mt6797_dvfsp_snapshot_cluster_check(input, i);
		if (ret)
			return ret;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->abi = MT6797_DVFSP_STATE_OWNER_ABI;
	snapshot->cluster_mask = MT6797_DVFSP_STATE_CLUSTER_MASK;
	snapshot->generation = input->generation;
	snapshot->owner_handle = input->owner_handle;
	snapshot->transition_handle = input->transition_handle;
	snapshot->provenance = input->provenance;

	for (i = 0; i < MT6797_DVFSP_STATE_CLUSTER_COUNT; i++) {
		struct mt6797_dvfsp_state_cluster *cluster =
			&snapshot->cluster[i];

		cluster->valid_fields = MT6797_DVFSP_STATE_FIELD_ALL;
		cluster->state_flags = input->state_flags[i];
		cluster->opp_id = input->opp_id[i];
		cluster->frequency_khz = input->frequency_khz[i];
		cluster->voltage_uv = input->voltage_uv[i];
		cluster->vsram_uv = input->vsram_uv[i];
		cluster->ceiling_khz = input->ceiling_khz[i];
		cluster->floor_khz = input->floor_khz[i];
		cluster->clock_state = input->clock_state[i];
		cluster->rail_state = input->rail_state[i];
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_snapshot_assemble);

MODULE_DESCRIPTION("MT6797 protected state snapshot assembler");
MODULE_LICENSE("GPL");
