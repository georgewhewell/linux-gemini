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

#define MT6797_A72_CAP_SRAM_STATUS	BIT(0)
#define MT6797_A72_CAP_SRAM_DISABLE	BIT(1)
#define MT6797_A72_CAP_OCP_GUARD		BIT(2)
#define MT6797_A72_CAP_IDVFS_FENCE	BIT(3)
#define MT6797_A72_REQUIRED_CAPS		(MT6797_A72_CAP_SRAM_STATUS | \
					 MT6797_A72_CAP_SRAM_DISABLE | \
					 MT6797_A72_CAP_OCP_GUARD | \
					 MT6797_A72_CAP_IDVFS_FENCE)

enum mt6797_a72_action {
	MT6797_A72_OCP_GUARD_ENTER,
	MT6797_A72_SPM_UPDATE,
	MT6797_A72_IDVFS_READ_FENCE,
	MT6797_A72_PWRAP_RESET_ASSERT,
	MT6797_A72_VPROC_ENABLE,
	MT6797_A72_DELAY_US,
	MT6797_A72_PWRAP_RESET_DEASSERT,
	MT6797_A72_OCP_GUARD_EXIT,
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
	int vsram_enabled;
	int pwrap_reset_asserted;
	int vproc_uv;
	int vsram_uv;
	bool ocp_guarded;
	bool a72_online;
};

struct mt6797_a72_backend_ops {
	int (*snapshot)(void *context, struct mt6797_a72_state *state);
	int (*apply)(void *context, const struct mt6797_a72_step *step);
	int (*verify)(void *context, const struct mt6797_a72_step *step);
	/* Undo one step. Exit steps must re-enter their rollback guard. */
	int (*rollback)(void *context, const struct mt6797_a72_step *step,
			const struct mt6797_a72_state *initial);
};

struct mt6797_a72_transaction {
	const struct mt6797_a72_backend_ops *ops;
	void *context;
	struct mt6797_a72_state initial;
	const struct mt6797_a72_plan *plan;
	unsigned int applied;
	bool active;
	bool faulted;
};

int mt6797_a72_build_plan(struct mt6797_a72_plan *plan);
int mt6797_a72_validate_admission(const struct mt6797_a72_state *state,
				  unsigned long capabilities);
/*
 * A successful begin leaves a reversible preparation transaction active.
 * This slice deliberately exposes only abort: CPU_ON/commit needs a separate
 * firmware-handoff implementation and review.
 */
int mt6797_a72_transaction_begin(struct mt6797_a72_transaction *transaction,
				  const struct mt6797_a72_plan *plan,
				  const struct mt6797_a72_backend_ops *ops,
				  void *context,
				  unsigned long capabilities);
int mt6797_a72_transaction_abort(struct mt6797_a72_transaction *transaction);

#endif /* __SOC_MEDIATEK_MT6797_A72_POWER_H */
