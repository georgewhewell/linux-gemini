// SPDX-License-Identifier: GPL-2.0-only
/*
 * Validate provider-owned MT6797 PPM policy rows.
 *
 * This object contains no vendor transport and performs no I/O. A real
 * provider must copy all four banks, including CCI, while holding the PPM
 * policy lock and the external clock/rail transition lock.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm-policy.h>

static int mt6797_dvfsp_ppm_policy_bank_check(
	const struct mt6797_dvfsp_ppm_policy_snapshot *policy,
	u32 bank)
{
	u32 previous_frequency = ~0U;
	u32 row;

	for (row = 0; row < MT6797_DVFSP_PPM_POLICY_TABLE_ENTRIES; row++) {
		if (!policy->frequency_khz[bank][row] ||
		    policy->frequency_khz[bank][row] >= previous_frequency ||
		    !policy->limit_khz[bank][row])
			return -EPROTO;
		previous_frequency = policy->frequency_khz[bank][row];
	}

	return 0;
}

int mt6797_dvfsp_ppm_policy_validate(
	const struct mt6797_dvfsp_ppm_policy_snapshot *policy)
{
	u32 bank;
	int ret;

	if (!policy || policy->abi != MT6797_DVFSP_PPM_POLICY_ABI ||
	    policy->reserved || !policy->table_epoch)
		return -EPROTO;

	for (bank = 0; bank < MT6797_DVFSP_PPM_POLICY_BANK_COUNT; bank++) {
		ret = mt6797_dvfsp_ppm_policy_bank_check(policy, bank);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_dvfsp_ppm_policy_validate);

MODULE_DESCRIPTION("MT6797 provider-owned PPM policy contract");
MODULE_LICENSE("GPL");
