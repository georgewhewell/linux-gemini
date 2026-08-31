// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware-neutral MT6797 CONSYS shared-EMI layout and one-way owner.
 *
 * The source contract is vendor/gemini-linux-kernel-3.18:
 * drivers/misc/mediatek/connectivity/common/common_main/mt6797/
 * mtk_wcn_consys_hw.c:1098-1141.  This file intentionally has no ioremap,
 * memset_io, readl, writel, or Device Tree caller.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/soc/mediatek/mt6797-consys-emi.h>

static bool mt6797_consys_emi_add(u64 base, u64 offset, u64 *result)
{
	return !check_add_overflow(base, offset, result);
}

static bool mt6797_consys_emi_reservation_valid(phys_addr_t start,
						resource_size_t size)
{
	u64 end;

	if (size != MT6797_CONSYS_SHARED_MEM_SIZE ||
	    start & (MT6797_CONSYS_SHARED_MEM_SIZE - 1))
		return false;
	if (!mt6797_consys_emi_add((u64)start, (u64)size, &end))
		return false;

	return start >= MT6797_CONSYS_SHARED_MEM_MIN &&
		end <= MT6797_CONSYS_SHARED_MEM_END;
}

int mt6797_consys_emi_layout_build(
		phys_addr_t reservation_start,
		resource_size_t reservation_size,
		struct mt6797_consys_emi_layout *layout)
{
	u64 protection_start;
	u64 protection_end;
	u64 clear_end;

	if (!layout || !mt6797_consys_emi_reservation_valid(reservation_start,
							  reservation_size))
		return -EINVAL;

	if (!mt6797_consys_emi_add((u64)reservation_start,
					   MT6797_CONSYS_EMI_PROTECTION_OFFSET,
					   &protection_start) ||
	    !mt6797_consys_emi_add(protection_start,
					   MT6797_CONSYS_EMI_PROTECTION_SIZE - 1,
					   &protection_end) ||
	    !mt6797_consys_emi_add(protection_start,
					   MT6797_CONSYS_EMI_CLEAR_SIZE - 1,
					   &clear_end))
		return -EOVERFLOW;

	/* The clear window is deliberately checked against the protected half. */
	if (clear_end > protection_end)
		return -EOVERFLOW;

	layout->reservation_start = reservation_start;
	layout->reservation_size = reservation_size;
	layout->protection_start = (phys_addr_t)protection_start;
	layout->protection_end = (phys_addr_t)protection_end;
	layout->clear_start = (phys_addr_t)protection_start;
	layout->clear_size = MT6797_CONSYS_EMI_CLEAR_SIZE;
	layout->protection_region = MT6797_CONSYS_EMI_MPU_REGION;
	layout->remap_value =
		(((u64)reservation_start & MT6797_CONSYS_EMI_REMAP_MASK) >> 20) |
		MT6797_CONSYS_EMI_REMAP_ENABLE;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_layout_build);

static bool mt6797_consys_emi_ops_valid(
		const struct mt6797_consys_emi_ops *ops)
{
	return ops && ops->abi == MT6797_CONSYS_EMI_ABI && ops->protect &&
		ops->remap && ops->map_clear;
}

static bool mt6797_consys_emi_layout_equal(
		const struct mt6797_consys_emi_layout *left,
		const struct mt6797_consys_emi_layout *right)
{
	return left->reservation_start == right->reservation_start &&
		left->reservation_size == right->reservation_size &&
		left->protection_start == right->protection_start &&
		left->protection_end == right->protection_end &&
		left->clear_start == right->clear_start &&
		left->clear_size == right->clear_size &&
		left->protection_region == right->protection_region &&
		left->remap_value == right->remap_value;
}

int mt6797_consys_emi_init(struct mt6797_consys_emi_owner *owner,
			   const struct mt6797_consys_emi_ops *ops, void *context)
{
	if (!owner || !mt6797_consys_emi_ops_valid(ops))
		return -EINVAL;

	owner->ops = ops;
	owner->context = context;
	owner->layout = (struct mt6797_consys_emi_layout){ };
	owner->state = MT6797_CONSYS_EMI_IDLE;
	mutex_init(&owner->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_init);

int mt6797_consys_emi_start(struct mt6797_consys_emi_owner *owner,
			    const struct mt6797_consys_emi_layout *layout,
			    bool experiment_gate)
{
	struct mt6797_consys_emi_layout derived;
	int ret;

	if (!owner || !layout)
		return -EINVAL;
	if (!experiment_gate)
		return -EPERM;

	mutex_lock(&owner->lock);
	if (!owner->ops) {
		ret = -EINVAL;
	} else if (owner->state == MT6797_CONSYS_EMI_ERROR) {
		ret = -EIO;
	} else if (owner->state != MT6797_CONSYS_EMI_IDLE) {
		ret = -EBUSY;
	} else if (mt6797_consys_emi_layout_build(layout->reservation_start,
						 layout->reservation_size, &derived) ||
		   !mt6797_consys_emi_layout_equal(layout, &derived)) {
		/* Do not let a caller smuggle un-derived addresses to a backend. */
		ret = -EINVAL;
	} else {
		owner->layout = *layout;
		ret = owner->ops->protect(owner->context, &owner->layout);
		if (!ret)
			ret = owner->ops->remap(owner->context, &owner->layout);
		if (!ret)
			ret = owner->ops->map_clear(owner->context, &owner->layout);
		if (ret)
			owner->state = MT6797_CONSYS_EMI_ERROR;
		else
			owner->state = MT6797_CONSYS_EMI_ACTIVE;
	}
	mutex_unlock(&owner->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_start);

int mt6797_consys_emi_stop(struct mt6797_consys_emi_owner *owner)
{
	int ret;

	if (!owner)
		return -EINVAL;

	mutex_lock(&owner->lock);
	ret = owner->state == MT6797_CONSYS_EMI_ERROR ? -EIO :
		(owner->state == MT6797_CONSYS_EMI_ACTIVE ? -EOPNOTSUPP : -EINVAL);
	mutex_unlock(&owner->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_stop);

enum mt6797_consys_emi_state
mt6797_consys_emi_state(struct mt6797_consys_emi_owner *owner)
{
	enum mt6797_consys_emi_state state;

	if (!owner)
		return MT6797_CONSYS_EMI_ERROR;
	mutex_lock(&owner->lock);
	state = owner->state;
	mutex_unlock(&owner->lock);
	return state;
}
EXPORT_SYMBOL_GPL(mt6797_consys_emi_state);

MODULE_DESCRIPTION("Dormant MT6797 CONSYS shared-EMI owner");
MODULE_LICENSE("GPL");
