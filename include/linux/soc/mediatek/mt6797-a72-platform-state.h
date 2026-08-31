/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_PLATFORM_STATE_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_PLATFORM_STATE_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

/**
 * struct mt6797_a72_platform_state - one immutable raw platform sample
 * @spm_pwr_status: general SPM power-domain status
 * @spm_pwr_status_2nd: second general SPM power-domain status
 * @spm_cpu_pwr_status: first SPM CPU power-status word
 * @spm_cpu_pwr_status_2nd: second SPM CPU power-status word
 * @spm_mp2_cpusys_pwr_con: MP2 cluster power-control word
 * @spm_mp2_cpu0_pwr_con: CPU8 power-control word
 * @spm_mp2_cpu1_pwr_con: CPU9 power-control word
 * @spm_cpu_ext_buck_iso: external CPU-buck isolation word
 * @mp2_sync_dcm: MP2 synchronous DCM word
 * @cci_mp2_port_control: MP2 CCI port-control word
 * @cci_status_before: global CCI status before the port read
 * @cci_status_after: global CCI status after the port read
 * @pwrap_reset_asserted: logical TOPRGU PWRAP reset state
 * @valid: all fields were captured in one stable bounded transaction
 *
 * This record carries raw observation only. It does not classify recovery,
 * authorize an A72 transition, or provide cross-PSCI serialization.
 */
struct mt6797_a72_platform_state {
	u32 spm_pwr_status;
	u32 spm_pwr_status_2nd;
	u32 spm_cpu_pwr_status;
	u32 spm_cpu_pwr_status_2nd;
	u32 spm_mp2_cpusys_pwr_con;
	u32 spm_mp2_cpu0_pwr_con;
	u32 spm_mp2_cpu1_pwr_con;
	u32 spm_cpu_ext_buck_iso;
	u32 mp2_sync_dcm;
	u32 cci_mp2_port_control;
	u32 cci_status_before;
	u32 cci_status_after;
	bool pwrap_reset_asserted;
	bool valid;
};

enum mt6797_a72_platform_state_movement {
	MT6797_A72_PLATFORM_MOVED_SPM_PWR_STATUS = BIT(0),
	MT6797_A72_PLATFORM_MOVED_SPM_PWR_STATUS_2ND = BIT(1),
	MT6797_A72_PLATFORM_MOVED_MP2_CPUSYS_PWR_CON = BIT(2),
	MT6797_A72_PLATFORM_MOVED_MP2_CPU0_PWR_CON = BIT(3),
	MT6797_A72_PLATFORM_MOVED_MP2_CPU1_PWR_CON = BIT(4),
	MT6797_A72_PLATFORM_MOVED_CPU_EXT_BUCK_ISO = BIT(5),
	MT6797_A72_PLATFORM_MOVED_MP2_SYNC_DCM = BIT(6),
	MT6797_A72_PLATFORM_MOVED_CCI_MP2_PORT = BIT(7),
	MT6797_A72_PLATFORM_MOVED_PWRAP_RESET = BIT(8),
	MT6797_A72_PLATFORM_MOVED_ALL = GENMASK(8, 0),
};

/**
 * struct mt6797_a72_platform_state_failure - completed refused pair
 * @first: first completed raw sample
 * @second: second completed raw sample
 * @movement_mask: fields that differ under the stable-snapshot contract
 * @samples_valid: both samples completed before refusal
 *
 * This is out-of-band failure evidence. It never authorizes a transition
 * and is zero for read errors and successful stable snapshots.
 */
struct mt6797_a72_platform_state_failure {
	struct mt6797_a72_platform_state first;
	struct mt6797_a72_platform_state second;
	u32 movement_mask;
	bool samples_valid;
};

#define MT6797_A72_CCI400_SI_COUNT 5U

/**
 * struct mt6797_a72_fabric_state - read-only post-CPU_ON fabric evidence
 * @mp2_axi_config: raw MT6797 MP2 AXI configuration word
 * @cci_status_before: CCI-400 status before reading slave interfaces
 * @cci_si_control: raw CCI-400 SI0..SI4 snoop/DVM request words
 * @cci_status_after: CCI-400 status after reading slave interfaces
 * @valid: no CCI change was pending across the bounded read
 *
 * No SI index is assigned to an MT6797 CPU cluster here. The vendor source
 * proves SI3 and SI4 users but does not name the A72-facing interface. Keeping
 * all five raw words avoids turning an unproven port mapping into policy.
 */
struct mt6797_a72_fabric_state {
	u32 mp2_axi_config;
	u32 cci_status_before;
	u32 cci_si_control[MT6797_A72_CCI400_SI_COUNT];
	u32 cci_status_after;
	bool valid;
};

struct mt6797_a72_provider_handle;

#define MT6797_A72_PLATFORM_EFFECT_ABI 1U

enum mt6797_a72_platform_effect_operation {
	MT6797_A72_PLATFORM_EFFECT_NONE,
	MT6797_A72_PLATFORM_EFFECT_P27_ACQUIRE,
	MT6797_A72_PLATFORM_EFFECT_P27_RELEASE,
	MT6797_A72_PLATFORM_EFFECT_ISOLATION_CLEAR,
	MT6797_A72_PLATFORM_EFFECT_DCM_UPDATE,
};

enum mt6797_a72_platform_effect_bit {
	MT6797_A72_EFFECT_P27_RESET_RELEASED = BIT(0),
	MT6797_A72_EFFECT_BPLL_ORDER_READ = BIT(1),
	MT6797_A72_EFFECT_PWRAP_ASSERTED = BIT(2),
	MT6797_A72_EFFECT_PWRAP_DEASSERTED = BIT(3),
	MT6797_A72_EFFECT_P27_RESET_RESTORED = BIT(4),
	MT6797_A72_EFFECT_ISOLATION_CLEARED = BIT(5),
	MT6797_A72_EFFECT_ISOLATION_GUARD = BIT(6),
	MT6797_A72_EFFECT_DCM_TOGGLE = BIT(7),
	MT6797_A72_EFFECT_DCM_FINAL = BIT(8),
};

struct mt6797_a72_platform_effect_handle {
	u64 attempt_id;
	u64 cookie;
};

struct mt6797_a72_platform_effect_result {
	u32 abi;
	u32 operation;
	u32 attempted_effects;
	u32 completed_effects;
	u32 spm_p27_before;
	u32 spm_p27_after;
	u32 isolation_before;
	u32 isolation_after;
	u32 bpll_ordering_value;
	u32 dcm_before;
	u32 dcm_toggle;
	u32 dcm_final;
	s32 error;
	bool p27_owned;
	bool isolation_attempted;
	bool isolation_crossed;
	bool sealed;
};

#if IS_ENABLED(CONFIG_MTK_MT6797_A72_PLATFORM_EFFECTS)
int
mt6797_a72_platform_effect_p27_acquire(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result);
int
mt6797_a72_platform_effect_p27_release(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result);
int
mt6797_a72_platform_effect_isolation_clear(struct device *dev,
					   const struct mt6797_a72_platform_effect_handle *handle,
					   const struct mt6797_a72_provider_handle *provider,
					   struct mt6797_a72_platform_effect_result *result);
int
mt6797_a72_platform_effect_dcm_update(struct device *dev,
				      const struct mt6797_a72_platform_effect_handle *handle,
				      bool cpu8_online, bool cpu9_online,
				      struct mt6797_a72_platform_effect_result *result);
#else
static inline int
mt6797_a72_platform_effect_p27_acquire(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result)
{
	(void)dev;
	(void)handle;
	if (result)
		*result = (struct mt6797_a72_platform_effect_result){};
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_platform_effect_p27_release(struct device *dev,
				       const struct mt6797_a72_platform_effect_handle *handle,
				       struct mt6797_a72_platform_effect_result *result)
{
	(void)dev;
	(void)handle;
	if (result)
		*result = (struct mt6797_a72_platform_effect_result){};
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_platform_effect_isolation_clear(struct device *dev,
					   const struct mt6797_a72_platform_effect_handle *handle,
					   const struct mt6797_a72_provider_handle *provider,
					   struct mt6797_a72_platform_effect_result *result)
{
	(void)dev;
	(void)handle;
	(void)provider;
	if (result)
		*result = (struct mt6797_a72_platform_effect_result){};
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_platform_effect_dcm_update(struct device *dev,
				      const struct mt6797_a72_platform_effect_handle *handle,
				      bool cpu8_online, bool cpu9_online,
				      struct mt6797_a72_platform_effect_result *result)
{
	(void)dev;
	(void)handle;
	(void)cpu8_online;
	(void)cpu9_online;
	if (result)
		*result = (struct mt6797_a72_platform_effect_result){};
	return -EOPNOTSUPP;
}
#endif
#if IS_ENABLED(CONFIG_MTK_MT6797_A72_PLATFORM_STATE)
int mt6797_a72_platform_state_snapshot_detailed(struct device *dev,
						struct mt6797_a72_platform_state *snapshot,
						struct mt6797_a72_platform_state_failure *failure);
int mt6797_a72_platform_state_snapshot(struct device *dev,
				       struct mt6797_a72_platform_state *snapshot);
int mt6797_a72_platform_fabric_snapshot(struct device *dev,
					struct mt6797_a72_fabric_state *snapshot);
#else
static inline int
mt6797_a72_platform_state_snapshot_detailed(struct device *dev,
					    struct mt6797_a72_platform_state *snapshot,
					    struct mt6797_a72_platform_state_failure *failure)
{
	(void)dev;
	if (snapshot)
		*snapshot = (struct mt6797_a72_platform_state){};
	if (failure)
		*failure = (struct mt6797_a72_platform_state_failure){};
	return -EOPNOTSUPP;
}

static inline int mt6797_a72_platform_state_snapshot(struct device *dev,
						     struct mt6797_a72_platform_state *snapshot)
{
	(void)dev;
	if (snapshot)
		*snapshot = (struct mt6797_a72_platform_state){};
	return -EOPNOTSUPP;
}

static inline int
mt6797_a72_platform_fabric_snapshot(struct device *dev,
				    struct mt6797_a72_fabric_state *snapshot)
{
	(void)dev;
	if (snapshot)
		*snapshot = (struct mt6797_a72_fabric_state){};
	return -EOPNOTSUPP;
}
#endif

#endif
