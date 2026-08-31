/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_BINDER_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_BINDER_H

#include <linux/cpuhotplug.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

typedef int (*mt6797_a72_cpu_boot_fn)(unsigned int cpu);

#define MT6797_A72_BINDER_DIAGNOSTIC_ABI 1U

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
