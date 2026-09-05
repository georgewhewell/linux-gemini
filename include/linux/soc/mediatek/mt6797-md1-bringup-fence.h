/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 source-order bring-up fence.
 *
 * This is a hardware-neutral transaction core.  It deliberately supplies no
 * MMIO, regmap, clock, reset, DMA, CCIF, CLDMA, firmware or platform-driver
 * implementation.  In particular, READY_FOR_BOOT_VECTOR is only a terminal
 * review state: this API contains no boot-vector release callback.
 *
 * Vendor order recorded here:
 *   ccci_modem.c:215-216              remap during MD layout setup
 *   ccci_platform.c:362-505           initial ROM/shared MPU, then CCIF clear
 *   cldma_platform.c:821-840          source clock, then PLL init
 *   ccci_platform.c:507-577           reset-held stage-one MPU
 *   modem_cldma.c:2156,2161-2167      power, stage-one MPU, boot release
 *   port_proxy.c:1636-1659            later stage-two MPU, after boot release
 * The boot release at the last cited line is intentionally outside this API.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_FENCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_FENCE_H

#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>
#include <linux/types.h>

#define MT6797_MD1_BRINGUP_FENCE_ABI		1

/*
 * The 3.18 MD1 PLL loops have no timeout.  This is a mainline containment
 * bound for a future backend, not a claimed vendor timeout or MMIO guarantee.
 */
#define MT6797_MD1_BRINGUP_MAX_PLL_POLLS	100

/*
 * ccci_set_mem_access_protection() applies header region 0 (EMI region 11)
 * and shared memory (EMI region 7) before CCIF.  After power-on,
 * ccci_set_mem_access_protection_1st_stage() applies header regions 1..4.
 * Its header region 2 (EMI region 13) has this temporary hard-coded
 * permission until port_proxy_md_hs1_msg_notify() performs stage two.  The
 * latter is post-release and intentionally has no representation here.
 */
#define MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES	2
#define MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES	4
#define MT6797_MD1_BRINGUP_STAGE1_REGION2_PERMISSION	0x0016db44

enum mt6797_md1_bringup_state {
	MT6797_MD1_BRINGUP_OFF,
	MT6797_MD1_BRINGUP_PREPARED,
	MT6797_MD1_BRINGUP_REMAP_APPLIED,
	MT6797_MD1_BRINGUP_INITIAL_MPU_APPLIED,
	MT6797_MD1_BRINGUP_CCIF_CLEARED,
	MT6797_MD1_BRINGUP_POWERED_RESET_HELD,
	MT6797_MD1_BRINGUP_SRC_CLK_ENABLED,
	MT6797_MD1_BRINGUP_PLL_READY,
	MT6797_MD1_BRINGUP_STAGE1_MPU_APPLIED,
	MT6797_MD1_BRINGUP_READY_FOR_BOOT_VECTOR,
	MT6797_MD1_BRINGUP_FENCED,
};

/* Values captured before or after a future CCIF clear implementation. */
struct mt6797_md1_ccif_observation {
	u32 ap_busy;
	u32 md_busy;
	u32 ap_ack;
	u32 md_ack;
	u32 ap_sram_nonzero_words;
	u32 md_sram_nonzero_words;
};

/*
 * These are independently-attested ownership facts, not observations this
 * core can manufacture.  The core refuses to call any backend unless every
 * fact is explicit.  A future MMIO owner must establish each one separately.
 */
struct mt6797_md1_bringup_authorization {
	bool lk_firmware_contract_valid;
	bool mpu_owner_and_failure_fence;
	bool remap_owner_and_failure_fence;
	bool power_reset_owner_and_failure_fence;
	bool clock_owner_and_bounded_pll;
	bool ccif_owner_and_destructive_clear_fence;
};

/*
 * Every callback is supplied by a future owner. There is intentionally no
 * boot_vector_release member. enable_srcclk occurs after reset-held power and
 * before PLL observations. power_off_reset_held must leave reset held even
 * when its first cleanup action fails; restore_remap must best-effort restore
 * the snapshot captured by apply_remap whenever a transaction fails after an
 * attempted remap.  The core permanently fences the object regardless of any
 * cleanup return value.
 */
struct mt6797_md1_bringup_ops {
	u32 abi;
	int (*apply_remap)(void *context,
			   const struct mt6797_md1_remap_values *values);
	int (*restore_remap)(void *context);
	int (*apply_initial_mpu)(void *context,
			       const struct mt6797_md1_mpu_remap_plan *plan,
			       const struct mt6797_md1_mpu_plan_entry *entries,
			       size_t count);
	/* One-way secure MPU transactions need a reset/reboot after any attempt. */
	void (*mpu_latch_fatal)(void *context);
	int (*ccif_observe)(void *context,
			    struct mt6797_md1_ccif_observation *observation);
	int (*ccif_clear)(void *context);
	int (*ccif_verify_cleared)(void *context,
			     const struct mt6797_md1_ccif_observation *before,
			     const struct mt6797_md1_ccif_observation *after);
	int (*power_on_reset_held)(void *context);
	int (*enable_srcclk)(void *context);
	int (*pll_poll)(void *context);
	int (*apply_stage1_mpu)(void *context,
				const struct mt6797_md1_mpu_remap_plan *plan,
				const struct mt6797_md1_mpu_plan_entry *entries,
				size_t count);
	int (*power_off_reset_held)(void *context);
};

struct mt6797_md1_bringup_fence {
	const struct mt6797_md1_bringup_ops *ops;
	void *context;
	struct mutex lock;
	struct mt6797_md1_mpu_remap_plan plan;
	struct mt6797_md1_mpu_plan_entry
		initial_mpu[MT6797_MD1_BRINGUP_INITIAL_MPU_ENTRIES];
	struct mt6797_md1_mpu_plan_entry
		stage1_mpu[MT6797_MD1_BRINGUP_STAGE1_MPU_ENTRIES];
	struct mt6797_md1_ccif_observation ccif_before;
	struct mt6797_md1_ccif_observation ccif_after;
	enum mt6797_md1_remap_mode remap_mode;
	enum mt6797_md1_bringup_state state;
	int last_error;
};

int mt6797_md1_bringup_fence_init(struct mt6797_md1_bringup_fence *fence,
	const struct mt6797_md1_bringup_ops *ops, void *context);
int mt6797_md1_bringup_fence_prepare(struct mt6797_md1_bringup_fence *fence,
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_remap_mode remap_mode,
	const struct mt6797_md1_bringup_authorization *authorization);
int mt6797_md1_bringup_fence_run(struct mt6797_md1_bringup_fence *fence);
enum mt6797_md1_bringup_state
mt6797_md1_bringup_fence_state(struct mt6797_md1_bringup_fence *fence);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_BRINGUP_FENCE_H */
