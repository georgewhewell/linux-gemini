// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-free MT6797 MD1 secure EMI-MPU transaction core.
 *
 * Exact vendor order: initial header region 0 / EMI 11, shared EMI 7, then
 * after reset-held power header regions 1..4 / EMI 12,13,10,14.  Region 13
 * uses the vendor temporary 0x0016db44 permission in the latter phase.
 *
 * There is deliberately no GET/readback or inverse callback.  The vendor
 * secure call can have changed a region before reporting failure, so every
 * nonzero SET result makes this object permanently fatal.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6797-md1-bringup-fence.h>
#include <linux/soc/mediatek/mt6797-md1-emi-mpu.h>
#include <linux/string.h>

static const u8 mt6797_md1_emi_mpu_regions[] = { 11, 12, 13, 10, 14, 7 };

static bool mt6797_md1_emi_mpu_plan_valid(
	const struct mt6797_md1_mpu_remap_plan *plan)
{
	unsigned int i;

	if (!plan || !plan->lk_reported_load_complete ||
	    plan->firmware.header_version < 4 ||
	    plan->firmware.header_version > 6 ||
	    plan->firmware.bind_sys_id != 1 ||
	    plan->firmware.region_count < MT6797_MD1_MPU_PLAN_HEADER_ENTRIES ||
	    plan->firmware.region_count > MT6797_MD1_CHECK_HEADER_REGION_MAX ||
	    !plan->firmware.raw_image_size || !plan->firmware.md_memory_size ||
	    plan->firmware.raw_image_size > plan->firmware.md_memory_size)
		return false;

	for (i = 0; i < MT6797_MD1_MPU_PLAN_ENTRIES; i++) {
		const struct mt6797_md1_mpu_plan_entry *entry = &plan->mpu[i];
		u8 header_region = i == MT6797_MD1_MPU_SHARED_ENTRY ? U8_MAX : i;

		if (entry->emi_mpu_region != mt6797_md1_emi_mpu_regions[i] ||
		    entry->check_header_region != header_region ||
		    !entry->start || entry->start > entry->end ||
		    (entry->start & (SZ_64K - 1)) ||
		    (entry->end & (SZ_64K - 1)) != SZ_64K - 1 ||
		    entry->access_permission & ~MT6797_MD1_EMI_MPU_PERMISSION_MASK)
			return false;
	}
	return true;
}

static bool mt6797_md1_emi_mpu_entries_valid(
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	const struct mt6797_md1_mpu_plan_entry *expected;
	unsigned int i;

	if (!mt6797_md1_emi_mpu_plan_valid(plan) || !entries)
		return false;
	if (phase == MT6797_MD1_EMI_MPU_INITIAL) {
		if (count != MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES)
			return false;
		for (i = 0; i < count; i++) {
			expected = i ? &plan->mpu[MT6797_MD1_MPU_SHARED_ENTRY] :
				&plan->mpu[0];
			if (memcmp(&entries[i], expected, sizeof(*entries)))
				return false;
		}
		return true;
	}
	if (phase != MT6797_MD1_EMI_MPU_STAGE1 ||
	    count != MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES)
		return false;
	for (i = 0; i < count; i++) {
		struct mt6797_md1_mpu_plan_entry temporary = plan->mpu[i + 1];

		if (i == 1)
			temporary.access_permission =
				MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION;
		if (memcmp(&entries[i], &temporary, sizeof(temporary)))
			return false;
	}
	return true;
}

/*
 * Value-only plan match for one secure-SET tuple.  This is not skip
 * authority: the platform provider separately requires the attested
 * LK-locked observation before it may bypass the secure call.
 */
bool mt6797_md1_emi_mpu_plan_entry_matches(
	const struct mt6797_md1_mpu_remap_plan *plan, u64 start, u64 end,
	u32 region_permission)
{
	u32 region = region_permission >> MT6797_MD1_EMI_MPU_REGION_SHIFT;
	u32 permission = region_permission & MT6797_MD1_EMI_MPU_PERMISSION_MASK;
	unsigned int i;

	if (!plan)
		return false;
	for (i = 0; i < MT6797_MD1_MPU_PLAN_ENTRIES; i++) {
		const struct mt6797_md1_mpu_plan_entry *entry = &plan->mpu[i];

		if (entry->emi_mpu_region != region)
			continue;
		return entry->start == start && entry->end == end &&
		       entry->access_permission == permission;
	}
	return false;
}

void mt6797_md1_emi_mpu_core_init(struct mt6797_md1_emi_mpu_core *core)
{
	if (!core)
		return;
	mutex_init(&core->lock);
	core->state = MT6797_MD1_EMI_MPU_IDLE;
}

int mt6797_md1_emi_mpu_core_apply(struct mt6797_md1_emi_mpu_core *core,
	mt6797_md1_emi_mpu_begin_fn begin, mt6797_md1_emi_mpu_set_fn set,
	void *context, const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count)
{
	unsigned int i;
	int ret;

	if (!core || !set)
		return -EINVAL;
	mutex_lock(&core->lock);
	if (core->state == MT6797_MD1_EMI_MPU_FATAL ||
	    core->state == MT6797_MD1_EMI_MPU_STAGE1_COMMITTED) {
		ret = -EIO;
		goto unlock;
	}
	if ((phase == MT6797_MD1_EMI_MPU_INITIAL &&
	     core->state != MT6797_MD1_EMI_MPU_IDLE) ||
	    (phase == MT6797_MD1_EMI_MPU_STAGE1 &&
	     core->state != MT6797_MD1_EMI_MPU_INITIAL_COMMITTED)) {
		ret = -EINVAL;
		goto unlock;
	}
	if (!mt6797_md1_emi_mpu_entries_valid(plan, phase, entries, count)) {
		/* Initial MPU is irreversible too; a bad continuation needs reset. */
		if (core->state != MT6797_MD1_EMI_MPU_IDLE)
			core->state = MT6797_MD1_EMI_MPU_FATAL;
		ret = -EINVAL;
		goto unlock;
	}
	if (core->state == MT6797_MD1_EMI_MPU_IDLE && begin) {
		ret = begin(context);
		if (ret)
			goto unlock;
	}
	for (i = 0; i < count; i++) {
		u32 region_permission = entries[i].access_permission |
			((u32)entries[i].emi_mpu_region <<
			 MT6797_MD1_EMI_MPU_REGION_SHIFT);

		ret = set(context, entries[i].start, entries[i].end,
			  region_permission);
		if (ret) {
			core->state = MT6797_MD1_EMI_MPU_FATAL;
			goto unlock;
		}
	}
	core->state = phase == MT6797_MD1_EMI_MPU_INITIAL ?
		MT6797_MD1_EMI_MPU_INITIAL_COMMITTED :
		MT6797_MD1_EMI_MPU_STAGE1_COMMITTED;
	ret = 0;
unlock:
	mutex_unlock(&core->lock);
	return ret;
}

void mt6797_md1_emi_mpu_core_latch_fatal(struct mt6797_md1_emi_mpu_core *core)
{
	if (!core)
		return;
	mutex_lock(&core->lock);
	if (core->state != MT6797_MD1_EMI_MPU_IDLE)
		core->state = MT6797_MD1_EMI_MPU_FATAL;
	mutex_unlock(&core->lock);
}

enum mt6797_md1_emi_mpu_state
mt6797_md1_emi_mpu_core_state(struct mt6797_md1_emi_mpu_core *core)
{
	enum mt6797_md1_emi_mpu_state state;

	if (!core)
		return MT6797_MD1_EMI_MPU_FATAL;
	mutex_lock(&core->lock);
	state = core->state;
	mutex_unlock(&core->lock);
	return state;
}
