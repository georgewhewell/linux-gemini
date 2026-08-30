/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware-neutral MT6797 CONSYS shared-EMI layout and one-way owner.
 *
 * The vendor source's init path at
 * drivers/misc/mediatek/connectivity/common/common_main/mt6797/
 * mtk_wcn_consys_hw.c:1098-1141 protects base + SZ_1M / 2 through
 * base + SZ_1M - 1, updates the CONSYS EMI remap value, and maps and clears
 * CONSYS_EMI_MEM_SIZE at base + SZ_1M / 2.  The vendor header at
 * .../mt6797/include/mtk_wcn_consys_hw.h:41,72,193-196 supplies the MPU
 * enable, mapping-register offset, half-megabyte offset, and 343-KiB size.
 *
 * This interface deliberately performs none of those hardware operations.
 * A future backend supplies callbacks and must provide its own MMIO/mapping
 * implementation.  The operation is one-way: the cited vendor source has no
 * inverse protection, remap, or clear path, so failed and completed runs are
 * terminal states rather than pretending that rollback exists.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_H

#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-consys.h>
#include <linux/types.h>

#define MT6797_CONSYS_EMI_ABI			1

#define MT6797_CONSYS_EMI_PROTECTION_OFFSET	0x80000ULL
#define MT6797_CONSYS_EMI_PROTECTION_SIZE	0x80000ULL
#define MT6797_CONSYS_EMI_CLEAR_SIZE		(343ULL * 1024ULL)
#define MT6797_CONSYS_EMI_MPU_REGION		19
#define MT6797_CONSYS_EMI_MAPPING_OFFSET	0x1340
#define MT6797_CONSYS_EMI_REMAP_ENABLE		0x1000
#define MT6797_CONSYS_EMI_REMAP_MASK		0xfff00000ULL

enum mt6797_consys_emi_state {
	MT6797_CONSYS_EMI_IDLE,
	MT6797_CONSYS_EMI_ACTIVE,
	MT6797_CONSYS_EMI_ERROR,
};

/* Every address is physical; no field is an ioremap'd or virtual address. */
struct mt6797_consys_emi_layout {
	phys_addr_t reservation_start;
	resource_size_t reservation_size;
	phys_addr_t protection_start;
	phys_addr_t protection_end;
	phys_addr_t clear_start;
	resource_size_t clear_size;
	u32 protection_region;
	u32 remap_value;
};

/* Backend callbacks are ordered exactly as the vendor init path. */
struct mt6797_consys_emi_ops {
	u32 abi;
	int (*protect)(void *context,
			      const struct mt6797_consys_emi_layout *layout);
	int (*remap)(void *context,
		     const struct mt6797_consys_emi_layout *layout);
	int (*map_clear)(void *context,
			 const struct mt6797_consys_emi_layout *layout);
};

struct mt6797_consys_emi_owner {
	const struct mt6797_consys_emi_ops *ops;
	void *context;
	struct mt6797_consys_emi_layout layout;
	struct mutex lock;
	enum mt6797_consys_emi_state state;
};

/* Validate the DT reservation and derive all vendor subranges safely. */
int mt6797_consys_emi_layout_build(
		phys_addr_t reservation_start,
		resource_size_t reservation_size,
		struct mt6797_consys_emi_layout *layout);

int mt6797_consys_emi_init(struct mt6797_consys_emi_owner *owner,
			   const struct mt6797_consys_emi_ops *ops, void *context);

/* @experiment_gate is independent and defaults to false at every caller. */
int mt6797_consys_emi_start(struct mt6797_consys_emi_owner *owner,
			    const struct mt6797_consys_emi_layout *layout,
			    bool experiment_gate);

/* No vendor inverse exists; this returns -EOPNOTSUPP without changing state. */
int mt6797_consys_emi_stop(struct mt6797_consys_emi_owner *owner);

enum mt6797_consys_emi_state
mt6797_consys_emi_state(struct mt6797_consys_emi_owner *owner);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_EMI_H */
