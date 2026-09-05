// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral half of the MT6797 MD1 remap observer.
 *
 * It makes exactly five caller-provided reads. It has no MMIO mapping and no
 * write-capable callback. The platform adapter is separately DT+cmdline gated.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/soc/mediatek/mt6797-md1-remap-observe.h>
#include <linux/string.h>

int mt6797_md1_remap_observe(
	const struct mt6797_md1_mpu_remap_plan *plan,
	mt6797_md1_remap_read_fn read, void *context,
	struct mt6797_md1_remap_observation *observation)
{
	struct mt6797_md1_remap_observation result;
	int ret;

	if (!read || !observation)
		return -EINVAL;
	memset(observation, 0, sizeof(*observation));

	ret = mt6797_md1_remap_attest_plan_validate(plan);
	if (ret)
		return ret;

	memset(&result, 0, sizeof(result));
	result.snapshot.bank0_map0 = read(context,
		MT6797_MD1_REMAP_BANK0_MAP0_OFFSET);
	result.snapshot.bank0_map1 = read(context,
		MT6797_MD1_REMAP_BANK0_MAP1_OFFSET);
	result.snapshot.bank4_map0 = read(context,
		MT6797_MD1_REMAP_BANK4_MAP0_OFFSET);
	result.snapshot.bank4_map1 = read(context,
		MT6797_MD1_REMAP_BANK4_MAP1_OFFSET);
	result.snapshot.lock = read(context, MT6797_MD1_REMAP_LOCK_OFFSET);
	result.classification = mt6797_md1_remap_attest_plan_snapshot(plan,
		&result.snapshot, &result.attestation);
	result.observed = true;
	*observation = result;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_md1_remap_observe);
