// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adapt explicit live clock/rail callbacks to the dormant state-source seam.
 *
 * No I/O occurs here. The caller owns the outer transition lock and supplies
 * samples that were captured under the same owner generation.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-live-source.h>

static const u32 mt6797_dvfsp_live_ppm_cluster[] = {
	MT6797_DVFSP_PPM_CLUSTER_LL,
	MT6797_DVFSP_PPM_CLUSTER_L,
	MT6797_DVFSP_PPM_CLUSTER_B,
};

static int mt6797_dvfsp_live_source_check(
	const struct mt6797_dvfsp_live_source *source)
{
	if (!source || !source->ops ||
	    source->ops->abi != MT6797_DVFSP_LIVE_SOURCE_ADAPTER_ABI ||
	    !source->ops->read_generation || !source->ops->read_cluster)
		return -EINVAL;

	return 0;
}

static int mt6797_dvfsp_live_frequency_check(
	const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	unsigned int cluster, u32 frequency_khz)
{
	unsigned int row;

	if (!frequency_khz ||
	    frequency_khz != clock->cluster[cluster].frequency_khz)
		return -EAGAIN;

	if (cluster == MT6797_DVFSP_PPM_CLUSTER_COUNT)
		return 0;

	for (row = 0; row < MT6797_DVFSP_PPM_TABLE_ENTRIES; row++)
		if (ppm->cluster[mt6797_dvfsp_live_ppm_cluster[cluster]]
			    .frequency_khz[row] == frequency_khz)
			return 0;

	return -EAGAIN;
}

static int mt6797_dvfsp_live_policy_check(
	const struct mt6797_dvfsp_calibration_state *calibration,
	unsigned int cluster, u32 frequency_khz, u32 ceiling_khz)
{
	const struct mt6797_dvfsp_calibration_cluster *table;
	unsigned int row;

	if (!calibration || cluster >= MT6797_DVFSP_STATE_CLUSTER_COUNT ||
	    !frequency_khz || !ceiling_khz)
		return -EINVAL;
	table = &calibration->cluster[cluster];
	if (!table->table_count ||
	    table->table_count > MT6797_DVFSP_CALIBRATION_TABLE_MAX)
		return -EPROTO;

	for (row = 0; row < table->table_count; row++) {
		const struct mt6797_dvfsp_calibration_table_entry *entry =
			&table->table[row];

		if (entry->frequency_khz != frequency_khz)
			continue;
		if (!entry->ppm_limit_khz ||
		    ceiling_khz > entry->ppm_limit_khz)
			return -EAGAIN;
		if (cluster == MT6797_DVFSP_STATE_CLUSTER_CCI &&
		    frequency_khz > entry->ppm_limit_khz)
			return -EAGAIN;
		return 0;
	}

	return -EAGAIN;
}

static int mt6797_dvfsp_live_cluster_check(
	const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm, unsigned int cluster,
	struct mt6797_dvfsp_live_source_cluster *state)
{
	u32 vproc_uv;
	u32 vsram_uv;
	int ret;

	if (!state || cluster >= MT6797_DVFSP_LIVE_SOURCE_CLUSTER_COUNT ||
	    state->state_flags &
	    ~(MT6797_DVFSP_STATE_FLAG_PRESENT | MT6797_DVFSP_STATE_FLAG_ON) ||
	    !(state->state_flags & MT6797_DVFSP_STATE_FLAG_PRESENT) ||
	    !state->floor_khz || !state->ceiling_khz ||
	    state->floor_khz > state->ceiling_khz ||
	    state->frequency_khz < state->floor_khz ||
    state->frequency_khz > state->ceiling_khz)
		return -EPROTO;

	ret = mt6797_dvfsp_live_frequency_check(clock, ppm, cluster,
						state->frequency_khz);
	if (ret)
		return ret;

	ret = mt6797_dvfsp_live_policy_check(calibration, cluster,
						state->frequency_khz, state->ceiling_khz);
	if (ret)
		return ret;

	if (state->vproc_code != cspm->cluster[cluster].voltage_code ||
	    state->vsram_code != cspm->cluster[cluster].vsram_code)
		return -EAGAIN;
	ret = mt6797_dvfsp_cspm_vproc_code_to_uv(state->vproc_code,
						&vproc_uv);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_cspm_vsram_code_to_uv(state->vsram_code,
						&vsram_uv);
	if (ret)
		return ret;

	if (cluster < MT6797_DVFSP_PPM_CLUSTER_COUNT &&
	    calibration->cluster[cluster].table_count == 0)
		return -EPROTO;

	state->vproc_uv = vproc_uv;
	state->vsram_uv = vsram_uv;
	return 0;
}

int mt6797_dvfsp_live_source_fill(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_state_source_live *live)
{
	struct mt6797_dvfsp_live_source *source = context;
	struct mt6797_dvfsp_live_source_cluster state;
	u64 generation;
	u64 owner_handle;
	u64 transition_handle;
	unsigned int cluster;
	int ret;

	if (!clock || !calibration || !cspm || !ppm || !live)
		return -EINVAL;
	ret = mt6797_dvfsp_live_source_check(source);
	if (ret)
		return ret;

	memset(live, 0, sizeof(*live));
	memset(&state, 0, sizeof(state));
	ret = source->ops->read_generation(source->context, clock,
						calibration, cspm, ppm, &generation,
						&owner_handle, &transition_handle,
						&live->provenance);
	if (ret)
		return ret;
	if (!generation || !owner_handle || !transition_handle ||
	    memcmp(&live->provenance, &calibration->provenance,
		   sizeof(live->provenance)) ||
	    generation != calibration->source_generation)
		return -EAGAIN;

	live->generation = generation;
	live->owner_handle = owner_handle;
	live->transition_handle = transition_handle;
	for (cluster = 0; cluster < MT6797_DVFSP_LIVE_SOURCE_CLUSTER_COUNT;
	     cluster++) {
		memset(&state, 0, sizeof(state));
		ret = source->ops->read_cluster(source->context, cluster, clock,
						calibration, cspm, ppm, &state);
		if (ret)
			return ret;
		ret = mt6797_dvfsp_live_cluster_check(clock, calibration, cspm,
						ppm, cluster, &state);
		if (ret)
			return ret;
		live->state_flags[cluster] = state.state_flags;
		live->opp_id[cluster] = state.opp_id;
		live->frequency_khz[cluster] = state.frequency_khz;
		live->voltage_uv[cluster] = state.vproc_uv;
		live->vsram_uv[cluster] = state.vsram_uv;
		live->floor_khz[cluster] = state.floor_khz;
		live->ceiling_khz[cluster] = state.ceiling_khz;
		live->clock_state[cluster] = state.clock_state;
		live->rail_state[cluster] = state.rail_state;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_live_source_fill);

MODULE_DESCRIPTION("MT6797 live clock and rail source adapter");
MODULE_LICENSE("GPL");
