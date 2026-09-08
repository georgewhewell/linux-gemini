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

#endif /* __SOC_MEDIATEK_MT6797_A72_POWER_H */
