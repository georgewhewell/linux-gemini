// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free MT6797 MD1 remap precondition classifier.
 *
 * Public LK writes MD1 bank4 and then sets MD1_LOCK:
 *   lk/platform/mt6797/ccci_lk_load_img_plat.c:161-229, 813-817
 * Its 4G branch uses physical low 32 bits; its other branch subtracts
 * 0x40000000 at :182-202. Both source-selected candidates are compared.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/soc/mediatek/mt6797-md1-remap-attest.h>
#include <linux/string.h>

int mt6797_md1_remap_attest_plan_validate(
	const struct mt6797_md1_mpu_remap_plan *plan)
{
	if (plan && plan->lk_reported_load_complete &&
		plan->firmware.header_version >= 4 &&
		plan->firmware.header_version <= 6 &&
		plan->firmware.bind_sys_id == 1 &&
		plan->firmware.region_count >= MT6797_MD1_MPU_PLAN_HEADER_ENTRIES &&
		plan->firmware.region_count <= MT6797_MD1_CHECK_HEADER_REGION_MAX &&
		plan->firmware.raw_image_size && plan->firmware.md_memory_size &&
		plan->firmware.raw_image_size <= plan->firmware.md_memory_size)
		return 0;

	return -ENODATA;
}
EXPORT_SYMBOL_GPL(mt6797_md1_remap_attest_plan_validate);

int mt6797_md1_remap_attest_plan_snapshot(
	const struct mt6797_md1_mpu_remap_plan *plan,
	const struct mt6797_md1_remap_snapshot *snapshot,
	struct mt6797_md1_remap_attestation *attestation)
{
	struct mt6797_md1_remap_attestation result;
	const struct mt6797_md1_remap_values *values;

	if (!snapshot || !attestation)
		return -EINVAL;
	memset(attestation, 0, sizeof(*attestation));
	if (mt6797_md1_remap_attest_plan_validate(plan))
		return -ENODATA;
	if (!(snapshot->lock & MT6797_MD1_REMAP_LOCK_MD1))
		return -EUCLEAN;

	values = &plan->remap[MT6797_MD1_REMAP_PHYS32];
	memset(&result, 0, sizeof(result));
	result.observed = *snapshot;
	result.lk_lock_observed = true;
	result.secure_mpu_unobservable = true;
	if (snapshot->bank4_map0 == values->bank4_map0 &&
	    snapshot->bank4_map1 == values->bank4_map1) {
		result.mode = MT6797_MD1_REMAP_ATTEST_PHYS32;
		*attestation = result;
		return 0;
	}
	values = &plan->remap[MT6797_MD1_REMAP_KERNEL_EMI_40000000];
	if (snapshot->bank4_map0 != values->bank4_map0 ||
	    snapshot->bank4_map1 != values->bank4_map1)
		return -EUCLEAN;

	result.mode = MT6797_MD1_REMAP_ATTEST_KERNEL_EMI_40000000;
	*attestation = result;
	return 0;
}
