// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adapt explicit vendor PPM/CCI callbacks to the dormant PPM-owner seam.
 *
 * This file performs no I/O. Its source callbacks must already hold the
 * vendor PPM lock when snapshot() is called; the state owner supplies the
 * outer transition lock before invoking that lock callback.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm-source.h>

static const u32 mt6797_dvfsp_ppm_policy_cluster[] = {
	MT6797_DVFSP_PPM_CLUSTER_B,
	MT6797_DVFSP_PPM_CLUSTER_L,
	MT6797_DVFSP_PPM_CLUSTER_LL,
};

static int mt6797_dvfsp_ppm_source_check(
	const struct mt6797_dvfsp_ppm_source *source)
{
	const struct mt6797_dvfsp_ppm_source_ops *ops;

	if (!source || !source->ops)
		return -EINVAL;
	ops = source->ops;
	if (ops->abi != MT6797_DVFSP_PPM_SOURCE_ADAPTER_ABI ||
	    !ops->lock || !ops->unlock || !ops->read_ppm_clusters ||
	    !ops->read_cci_frequency || !ops->read_policy_limits ||
	    !ops->read_table_epoch)
		return -EINVAL;
	return 0;
}

static int mt6797_dvfsp_ppm_source_lock(void *context)
{
	struct mt6797_dvfsp_ppm_source *source = context;
	int ret;

	ret = mt6797_dvfsp_ppm_source_check(source);
	if (ret)
		return ret;
	return source->ops->lock(source->context);
}

static void mt6797_dvfsp_ppm_source_unlock(void *context)
{
	struct mt6797_dvfsp_ppm_source *source = context;

	if (!source || !source->ops || !source->ops->unlock)
		return;
	source->ops->unlock(source->context);
}

int mt6797_dvfsp_ppm_source_snapshot(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_ppm_snapshot *ppm,
	struct mt6797_dvfsp_ppm_policy_snapshot *policy)
{
	struct mt6797_dvfsp_ppm_source *source = context;
	u32 cci_frequency[MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES];
	u64 table_epoch;
	int ret;

	if (!ptp_state || !ppm || !policy)
		return -EINVAL;
	ret = mt6797_dvfsp_ppm_source_check(source);
	if (ret)
		return ret;

	memset(ppm, 0, sizeof(*ppm));
	memset(policy, 0, sizeof(*policy));
	memset(cci_frequency, 0, sizeof(cci_frequency));
	policy->abi = MT6797_DVFSP_PPM_POLICY_ABI;

	ret = source->ops->read_ppm_clusters(source->context, ptp_state, ppm);
	if (ret)
		return ret;
	ret = source->ops->read_cci_frequency(source->context, ptp_state,
						cci_frequency);
	if (ret)
		return ret;
	ret = source->ops->read_policy_limits(source->context, ptp_state,
						policy->limit_khz);
	if (ret)
		return ret;
	ret = source->ops->read_table_epoch(source->context, ptp_state,
						&table_epoch);
	if (ret)
		return ret;
	if (!table_epoch)
		return -EPROTO;

	policy->table_epoch = table_epoch;
	ppm->table_epoch = table_epoch;
	memcpy(policy->frequency_khz[MT6797_DVFSP_PPM_POLICY_BANK_CCI],
		cci_frequency, sizeof(cci_frequency));
	memcpy(policy->frequency_khz[MT6797_DVFSP_PPM_POLICY_BANK_BIG],
		ppm->cluster[mt6797_dvfsp_ppm_policy_cluster[0]].frequency_khz,
		sizeof(ppm->cluster[0].frequency_khz));
	memcpy(policy->frequency_khz[MT6797_DVFSP_PPM_POLICY_BANK_L],
		ppm->cluster[mt6797_dvfsp_ppm_policy_cluster[1]].frequency_khz,
		sizeof(ppm->cluster[0].frequency_khz));
	memcpy(policy->frequency_khz[MT6797_DVFSP_PPM_POLICY_BANK_2L],
		ppm->cluster[mt6797_dvfsp_ppm_policy_cluster[2]].frequency_khz,
		sizeof(ppm->cluster[0].frequency_khz));

	ret = mt6797_dvfsp_ppm_snapshot_validate(ppm);
	if (ret)
		return ret;
	return mt6797_dvfsp_ppm_policy_validate(policy);
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ppm_source_snapshot);

int mt6797_dvfsp_ppm_source_owner_ops_init(
	const struct mt6797_dvfsp_ppm_source *source,
	struct mt6797_dvfsp_ppm_owner_ops *owner_ops)
{
	if (!owner_ops || mt6797_dvfsp_ppm_source_check(source))
		return -EINVAL;

	memset(owner_ops, 0, sizeof(*owner_ops));
	owner_ops->abi = MT6797_DVFSP_PPM_OWNER_ABI;
	owner_ops->lock = mt6797_dvfsp_ppm_source_lock;
	owner_ops->unlock = mt6797_dvfsp_ppm_source_unlock;
	owner_ops->snapshot = mt6797_dvfsp_ppm_source_snapshot;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ppm_source_owner_ops_init);

MODULE_DESCRIPTION("MT6797 read-only PPM/CCI source adapter");
