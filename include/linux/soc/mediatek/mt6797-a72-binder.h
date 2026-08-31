/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_BINDER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_BINDER_H

#include <linux/bits.h>
#include <linux/cpuhotplug.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

typedef int (*mt6797_a72_cpu_boot_fn)(unsigned int cpu);

#define MT6797_A72_BINDER_DIAGNOSTIC_ABI 2U

#define MT6797_A72_BINDER_SRAM_MATCH_ABI		BIT(0)
#define MT6797_A72_BINDER_SRAM_MATCH_ATTEMPTED	BIT(1)
#define MT6797_A72_BINDER_SRAM_MATCH_COMPLETED	BIT(2)
#define MT6797_A72_BINDER_SRAM_MATCH_VOLTAGE	BIT(3)
#define MT6797_A72_BINDER_SRAM_MATCH_SELECTOR_FIRST	BIT(4)
#define MT6797_A72_BINDER_SRAM_MATCH_SELECTOR_SECOND	BIT(5)
#define MT6797_A72_BINDER_SRAM_MATCH_ATTEMPT_ID	BIT(6)
#define MT6797_A72_BINDER_SRAM_MATCH_COOKIE	BIT(7)
#define MT6797_A72_BINDER_SRAM_MATCH_ERROR		BIT(8)
#define MT6797_A72_BINDER_SRAM_MATCH_EFFECT		BIT(9)
#define MT6797_A72_BINDER_SRAM_MATCH_VERIFIED	BIT(10)
#define MT6797_A72_BINDER_SRAM_MATCH_SEALED		BIT(11)
#define MT6797_A72_BINDER_SRAM_REQUIRED_MASK		GENMASK(11, 0)

struct mt6797_a72_binder_diagnostic {
	u32 abi;
	u32 lifecycle;
	u32 terminal;
	u32 last_stage;
	s32 stage_errno;
	s32 rollback_errno;
	s32 checkpoint_errno;
	u32 attempted;
	u32 watchdog_armed;
	u32 watchdog_released;
	s32 watchdog_release_ret;
	u64 watchdog_release_identity;
	u32 p27_owned;
	u32 rollback_mask;
	u32 retained_mask;
	u32 p27_acquire_operation;
	s32 p27_acquire_error;
	u32 p27_acquire_attempted;
	u32 p27_acquire_completed;
	u32 p27_acquire_spm_before;
	u32 p27_acquire_spm_after;
	u32 p27_acquire_bpll;
	u32 p27_acquire_owned;
	u32 p27_acquire_sealed;
	u32 p27_release_operation;
	s32 p27_release_error;
	u32 p27_release_attempted;
	u32 p27_release_completed;
	u32 p27_release_spm_before;
	u32 p27_release_spm_after;
	u32 p27_release_bpll;
	u32 p27_release_owned;
	u32 p27_release_sealed;
	u32 p28_begin_attempted;
	s32 p28_begin_ret;
	u32 p28_begun;
	u32 sram_returned;
	s32 sram_ret;
	u32 sram_match_mask;
	u32 sram_required_mask;
	u32 p28_complete_attempted;
	s32 p28_complete_ret;
	u32 sram_abi;
	u32 sram_attempted;
	u32 sram_completed;
	u32 sram_requested_mv_x100;
	u32 sram_selector_first;
	u32 sram_calibration_first;
	u32 sram_selector_second;
	u32 sram_calibration_second;
	u64 sram_attempt_id;
	u64 sram_cookie;
	s32 sram_error;
	u32 sram_effect_attempted;
	u32 sram_verified;
	u32 sram_sealed;
};

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_DEFAULT_OFF_BINDER)
bool mt6797_a72_binder_available(void);
int
mt6797_a72_binder_diagnostic_snapshot(struct mt6797_a72_binder_diagnostic *snapshot);
int mt6797_a72_binder_preflight(unsigned int cpu, enum cpuhp_state target);
int mt6797_a72_binder_validate(unsigned int cpu, int tasks_frozen,
			       enum cpuhp_state target);
int mt6797_a72_binder_cpu_boot(unsigned int cpu,
			       mt6797_a72_cpu_boot_fn cpu_boot);
int mt6797_a72_binder_secondary_complete(unsigned int cpu);
int mt6797_a72_binder_complete(unsigned int cpu, enum cpuhp_state target);
int mt6797_a72_binder_failure(unsigned int cpu, int error,
			      bool *publish_p32);
#else
static inline bool mt6797_a72_binder_available(void)
{
	return false;
}

static inline int
mt6797_a72_binder_diagnostic_snapshot(struct mt6797_a72_binder_diagnostic *snapshot)
{
	if (snapshot)
		*snapshot = (struct mt6797_a72_binder_diagnostic){};
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_binder_preflight(unsigned int cpu, enum cpuhp_state target)
{
	(void)cpu;
	(void)target;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_binder_validate(unsigned int cpu, int tasks_frozen,
			   enum cpuhp_state target)
{
	(void)cpu;
	(void)tasks_frozen;
	(void)target;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_binder_cpu_boot(unsigned int cpu,
			   mt6797_a72_cpu_boot_fn cpu_boot)
{
	(void)cpu;
	(void)cpu_boot;
	return -EOPNOTSUPP;
}

static inline int mt6797_a72_binder_secondary_complete(unsigned int cpu)
{
	(void)cpu;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_binder_complete(unsigned int cpu, enum cpuhp_state target)
{
	(void)cpu;
	(void)target;
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_binder_failure(unsigned int cpu, int error, bool *publish_p32)
{
	(void)cpu;
	(void)error;
	if (publish_p32)
		*publish_p32 = false;
	return -EOPNOTSUPP;
}
#endif

#endif /* __LINUX_SOC_MEDIATEK_MT6797_A72_BINDER_H */
