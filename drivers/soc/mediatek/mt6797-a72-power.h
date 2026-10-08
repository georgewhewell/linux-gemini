/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_MEDIATEK_MT6797_A72_POWER_H
#define __SOC_MEDIATEK_MT6797_A72_POWER_H

#include <linux/bits.h>
#include <linux/types.h>

#define MT6797_A72_SPM_MP2_PWR_CON	0x218
#define MT6797_A72_SPM_EXT_BUCK_ISO	0x290

#define MT6797_A72_MP2_PWR_RST_B		BIT(0)
#define MT6797_A72_MP2_FW_OWNED_MASK	(BIT(1) | BIT(2) | BIT(3) | \
					 BIT(4) | BIT(5) | BIT(6) | \
					 BIT(8) | BIT(12) | BIT(16) | \
					 BIT(19))
#define MT6797_A72_BIG_BUCK_ISO		BIT(1)
#define MT6797_A72_CORE0_POWER_ACK	BIT(7)
#define MT6797_A72_CORE1_POWER_ACK	BIT(6)
#define MT6797_A72_CLUSTER_POWER_ACK	BIT(17)

#define MT6797_A72_SRAM_BOOT_UV		1100000
#define MT6797_A72_VPROC_MIN_UV		800000
#define MT6797_A72_VPROC_MAX_UV		1200000
#define MT6797_A72_SRAM_MIN_UV		600000
#define MT6797_A72_SRAM_MAX_UV		1200000

#define MT6797_A72_R0P1_MIDR		0x410fd081U

#define MT6797_A72_CAP_EXCLUSIVE_GUARD	BIT(0)
#define MT6797_A72_CAP_IDVFS_FENCE	BIT(1)
#define MT6797_A72_REQUIRED_CAPS		(MT6797_A72_CAP_EXCLUSIVE_GUARD | \
					 MT6797_A72_CAP_IDVFS_FENCE)

enum mt6797_a72_action {
	MT6797_A72_TRANSACTION_GUARD_ENTER,
	MT6797_A72_SPM_UPDATE,
	MT6797_A72_IDVFS_READ_FENCE,
	MT6797_A72_PWRAP_RESET_ASSERT,
	MT6797_A72_VPROC_ENABLE,
	MT6797_A72_DELAY_US,
	MT6797_A72_PWRAP_RESET_DEASSERT,
	MT6797_A72_TRANSACTION_GUARD_EXIT,
	MT6797_A72_VSRAM_SET_UV,
	MT6797_A72_NR_ACTIONS,
};

struct mt6797_a72_step {
	enum mt6797_a72_action action;
	u32 reg;
	u32 mask;
	u32 value;
};

#define MT6797_A72_PLAN_MAX_STEPS	12

struct mt6797_a72_plan {
	struct mt6797_a72_step steps[MT6797_A72_PLAN_MAX_STEPS];
	unsigned int count;
};

struct mt6797_a72_state {
	u32 mp2_pwr_con;
	u32 ext_buck_iso;
	int vproc_enabled;
	int pwrap_reset_asserted;
	int vproc_uv;
	int vsram_uv;
	u32 vsram_selector;
	u32 vsram_control;
	u32 vsram_calibration;
	bool transaction_guarded;
	bool a72_online;
};

struct mt6797_a72_backend_ops {
	int (*snapshot)(void *context, struct mt6797_a72_state *state);
	int (*apply)(void *context, const struct mt6797_a72_step *step);
	int (*verify)(void *context, const struct mt6797_a72_step *step);
	/* Undo a step only before the isolation-clear attempt. */
	int (*rollback)(void *context, const struct mt6797_a72_step *step,
			const struct mt6797_a72_state *initial);
	/* Release software locks only; retain providers and all electrical state. */
	void (*retain_fault)(void *context);
};

struct mt6797_a72_transaction {
	const struct mt6797_a72_backend_ops *ops;
	void *context;
	struct mt6797_a72_state initial;
	const struct mt6797_a72_plan *plan;
	unsigned int applied;
	/* Latched before dispatch: a failed isolation write may have taken effect. */
	bool isolation_attempted;
	bool active;
	bool faulted;
};

int mt6797_a72_build_plan(struct mt6797_a72_plan *plan);
int mt6797_a72_validate_admission(const struct mt6797_a72_state *state,
				  unsigned long capabilities);
int mt6797_a72_validate_cpu8_boot(unsigned int cpu, bool cpu4_online,
				  bool transaction_armed, bool faulted);
/* CPU9 needs no rail transaction: CPU8 online proves the cluster is powered. */
int mt6797_a72_validate_cpu9_boot(unsigned int cpu, bool cpu8_online,
				  bool faulted);
int mt6797_a72_validate_cpu8_identity(u32 midr);
int mt6797_a72_validate_removal(bool online, bool active, bool armed, bool guarded, bool faulted);
/* After isolation is attempted, any failure/abort retains state until reset. */
int mt6797_a72_transaction_begin(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context,
				  unsigned long capabilities);
int mt6797_a72_transaction_abort(struct mt6797_a72_transaction *transaction);
int mt6797_a72_transaction_commit(struct mt6797_a72_transaction *transaction);

/*
 * The surviving CPU calls this only after the target has left Linux and
 * entered PSCI CPU_OFF. AFFINITY_INFO is an effectful firmware kill on MT6797,
 * never a harmless query of a running target. CPU9 must leave before CPU8.
 */
struct mt6797_a72_off_sample {
	u32 online_mask;
	u32 power_status;
	u32 power_status_second;
	u32 mp2_pwr_con;
	u32 ext_buck_iso;
	u32 idvfs_control;
	int vproc_enabled;
};

struct mt6797_a72_off_io {
	int (*sample)(void *context, struct mt6797_a72_off_sample *sample);
	int (*sync_dcm_off)(void *context);
	int (*firmware_kill)(void *context, unsigned int cpu);
	int (*vproc_disable)(void *context);
	void (*fence)(void *context);
};

struct mt6797_a72_off_state {
	struct mt6797_a72_off_sample final;
	bool completed;
	bool cluster_off;
	bool faulted;
	int error;
};

int mt6797_a72_finish_cpu_off(struct mt6797_a72_off_state *state,
			     const struct mt6797_a72_off_io *io,
			     void *context, unsigned int cpu);

/* The owner retains both observations from the same completed shutdown. */
int mt6797_a72_transaction_restart(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context, unsigned long capabilities,
				  const struct mt6797_a72_off_state *off,
				  const struct mt6797_a72_state *stopped);

#endif /* __SOC_MEDIATEK_MT6797_A72_POWER_H */
