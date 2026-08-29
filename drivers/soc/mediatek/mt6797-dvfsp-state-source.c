// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compose the validated MT6797 protected-state source pipeline.
 *
 * No callback is registered here. A real owner supplies the callbacks and
 * holds its transition lock around this complete sequence. This object only
 * orders raw readback, pure conversion, and final snapshot assembly.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-state-source.h>

static int mt6797_dvfsp_state_source_check(
	const struct mt6797_dvfsp_state_source_ops *ops)
{
	if (!ops || ops->abi != MT6797_DVFSP_STATE_SOURCE_ABI ||
	    !ops->read_clock || !ops->read_big || !ops->read_eem ||
	    !ops->read_ptp ||
	    !ops->read_ppm ||
	    !ops->fill_calibration || !ops->fill_live)
		return -EINVAL;

	return 0;
}

static int mt6797_dvfsp_state_source_ppm_calibration_match(
	const struct mt6797_dvfsp_ppm_snapshot *ppm,
	const struct mt6797_dvfsp_calibration_state *calibration)
{
	static const u32 cluster_map[MT6797_DVFSP_PPM_CLUSTER_COUNT] = {
		MT6797_DVFSP_STATE_CLUSTER_LL,
		MT6797_DVFSP_STATE_CLUSTER_L,
		MT6797_DVFSP_STATE_CLUSTER_B,
	};
	unsigned int ppm_cluster;

	if (!ppm || !calibration || calibration->abi !=
	    MT6797_DVFSP_CALIBRATION_STATE_ABI)
		return -EINVAL;

	for (ppm_cluster = 0;
	     ppm_cluster < MT6797_DVFSP_PPM_CLUSTER_COUNT; ppm_cluster++) {
		const struct mt6797_dvfsp_calibration_cluster *cluster =
			&calibration->cluster[cluster_map[ppm_cluster]];
		unsigned int row;

		if (cluster->table_count != MT6797_DVFSP_PPM_TABLE_ENTRIES)
			return -EPROTO;
		for (row = 0; row < MT6797_DVFSP_PPM_TABLE_ENTRIES; row++) {
			const struct mt6797_dvfsp_calibration_table_entry *entry =
				&cluster->table[MT6797_DVFSP_PPM_TABLE_ENTRIES - 1 - row];

			if (entry->frequency_khz !=
			    ppm->cluster[ppm_cluster].frequency_khz[row])
				return -EAGAIN;
		}
	}

	return 0;
}

int mt6797_dvfsp_state_source_snapshot(
	const struct mt6797_dvfsp_state_source_ops *ops, void *context,
	struct mt6797_dvfsp_state_snapshot *snapshot)
{
	struct mt6797_dvfsp_clock_readback clock_readback;
	struct mt6797_bigidvfs_readback big_readback;
	struct mt6797_eem_readback eem_readback;
	struct mt6797_dvfsp_cspm_state cspm_state;
	struct mt6797_dvfsp_ptp_handoff ptp_handoff;
	struct mt6797_dvfsp_ptp_state ptp_state;
	struct mt6797_dvfsp_ppm_snapshot ppm_snapshot;
	struct mt6797_dvfsp_ppm_policy_snapshot ppm_policy;
	struct mt6797_dvfsp_clock_state clock_state;
	struct mt6797_eem_calibration_input calibration_input;
	struct mt6797_dvfsp_calibration_state calibration_state;
	struct mt6797_dvfsp_state_source_live live;
	struct mt6797_dvfsp_state_snapshot_input input;
	int ret;

	if (!snapshot)
		return -EINVAL;
	ret = mt6797_dvfsp_state_source_check(ops);
	if (ret)
		return ret;

	memset(&clock_readback, 0, sizeof(clock_readback));
	ret = ops->read_clock(context, &clock_readback);
	if (ret)
		return ret;

	memset(&cspm_state, 0, sizeof(cspm_state));
	ret = mt6797_dvfsp_cspm_state_decode(&clock_readback, &cspm_state);
	if (ret)
		return ret;

	memset(&big_readback, 0, sizeof(big_readback));
	ret = ops->read_big(context, &big_readback);
	if (ret)
		return ret;

	ret = mt6797_dvfsp_clock_state_decode(&clock_readback, &big_readback,
						      &clock_state);
	if (ret)
		return ret;

	memset(&eem_readback, 0, sizeof(eem_readback));
	ret = ops->read_eem(context, &eem_readback);
	if (ret)
		return ret;

	memset(&ptp_handoff, 0, sizeof(ptp_handoff));
	ret = ops->read_ptp(context, &ptp_handoff);
	if (ret)
		return ret;

	ret = mt6797_dvfsp_ptp_decode(&ptp_handoff, &ptp_state);
	if (ret)
		return ret;

	memset(&ppm_snapshot, 0, sizeof(ppm_snapshot));
	ret = ops->read_ppm(context, &ptp_state, &ppm_snapshot);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_ppm_snapshot_validate(&ppm_snapshot);
	if (ret)
		return ret;

	memset(&calibration_input, 0, sizeof(calibration_input));
	memset(&ppm_policy, 0, sizeof(ppm_policy));
	ret = ops->fill_calibration(context, &ptp_handoff, &ptp_state,
					    &ppm_snapshot,
					    &ppm_policy,
					    &eem_readback,
					    &calibration_input);
	if (ret)
		return ret;
	calibration_input.readback = &eem_readback;
	calibration_input.ptp_state = &ptp_state;
	calibration_input.ppm_snapshot = &ppm_snapshot;
	ret = mt6797_dvfsp_ppm_policy_validate(&ppm_policy);
	if (ret)
		return ret;
	calibration_input.ppm_policy = &ppm_policy;
	ret = mt6797_eem_calibration_build(&calibration_input,
					   &calibration_state);
	if (ret)
		return ret;
	ret = mt6797_dvfsp_state_source_ppm_calibration_match(
		&ppm_snapshot, &calibration_state);
	if (ret)
		return ret;

	memset(&live, 0, sizeof(live));
	ret = ops->fill_live(context, &clock_state, &calibration_state,
				    &cspm_state,
				    &ppm_snapshot, &live);
	if (ret)
		return ret;
	if (!calibration_state.source_generation ||
	    live.generation != calibration_state.source_generation)
		return -EAGAIN;
	if (!live.cspm_sample_generation ||
	    live.cspm_sample_generation != cspm_state.sample_generation)
		return -EAGAIN;
	if (ppm_snapshot.table_epoch != calibration_state.provenance.table_epoch ||
	    ppm_snapshot.table_epoch != live.provenance.table_epoch)
		return -EAGAIN;
	{
		unsigned int cluster;

		for (cluster = 0;
		     cluster < MT6797_DVFSP_PPM_CLUSTER_COUNT; cluster++) {
			u32 floor_khz;
			u32 ceiling_khz;
			unsigned int row;
			bool found = false;

			ret = mt6797_dvfsp_ppm_snapshot_current_limit(
				&ppm_snapshot, cluster, &floor_khz, &ceiling_khz);
			if (ret)
				return ret;
			if (!live.frequency_khz[cluster])
				return -EPROTO;
			for (row = 0;
			     row < MT6797_DVFSP_PPM_TABLE_ENTRIES; row++) {
				if (ppm_snapshot.cluster[cluster].frequency_khz[row] ==
				    live.frequency_khz[cluster]) {
					found = true;
					break;
				}
			}
			if (!found)
				return -EAGAIN;
			live.floor_khz[cluster] = floor_khz;
			live.ceiling_khz[cluster] = ceiling_khz;
		}
	}

	memset(&input, 0, sizeof(input));
	input.abi = MT6797_DVFSP_STATE_SNAPSHOT_ABI;
	input.generation = live.generation;
	input.owner_handle = live.owner_handle;
	input.transition_handle = live.transition_handle;
	input.clock = &clock_state;
	input.calibration = &calibration_state;
	input.provenance = live.provenance;
	memcpy(input.state_flags, live.state_flags, sizeof(input.state_flags));
	memcpy(input.opp_id, live.opp_id, sizeof(input.opp_id));
	memcpy(input.frequency_khz, live.frequency_khz,
	       sizeof(input.frequency_khz));
	memcpy(input.voltage_uv, live.voltage_uv, sizeof(input.voltage_uv));
	memcpy(input.vsram_uv, live.vsram_uv, sizeof(input.vsram_uv));
	memcpy(input.ceiling_khz, live.ceiling_khz,
	       sizeof(input.ceiling_khz));
	memcpy(input.floor_khz, live.floor_khz, sizeof(input.floor_khz));
	memcpy(input.clock_state, live.clock_state, sizeof(input.clock_state));
	memcpy(input.rail_state, live.rail_state, sizeof(input.rail_state));

	return mt6797_dvfsp_state_snapshot_assemble(&input, snapshot);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_state_source_snapshot);

MODULE_DESCRIPTION("MT6797 protected state source adapter");
MODULE_LICENSE("GPL");
