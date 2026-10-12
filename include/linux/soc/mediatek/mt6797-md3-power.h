/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD3_POWER_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD3_POWER_H

#include <linux/mutex.h>
#include <linux/types.h>

/* One shared poll budget, not the vendor's unbounded MTCMOS wait. */
#define MT6797_MD3_POWER_MAX_POLLS 10000

enum mt6797_md3_power_reg {
	MT6797_MD3_SPM_CONTROL,       /* SPM + 0x328 */
	MT6797_MD3_SPM_STATUS,        /* SPM + 0x180, bit 28 */
	MT6797_MD3_SPM_STATUS_2ND,    /* SPM + 0x184, bit 28 */
	MT6797_MD3_INFRA_PROTECT_EN,  /* infracfg + 0x220, bits 13..15 */
	MT6797_MD3_INFRA_PROTECT_STA, /* infracfg + 0x228, bits 13..15 */
};

enum mt6797_md3_power_state {
	MT6797_MD3_POWER_NEW, /* Software state; does not assert hardware is off. */
	MT6797_MD3_POWER_TRANSITION,
	MT6797_MD3_POWER_DOMAIN_ON,
	MT6797_MD3_POWER_FAILED,
};

struct mt6797_md3_power_result {
	enum mt6797_md3_power_state state;
	int error;
	u32 polls;
	bool may_have_written;
};

/*
 * No backend is installed by this core. A future, separately reviewed native
 * owner must implement these operations using the EXISTING MD1 shared owner,
 * not a duplicate infracfg/SPM mapping or an auto-powered genpd attachment.
 *
 * begin() must acquire and retain the real supplier/consumer lifetime and
 * shared transaction exclusion, verify the MD3 cold lease and detached image,
 * committed MD3 MPU/remap (including unchanged MD1-owned region 9), held
 * TOPRGU C2K reset and required clock references. PMIC/clock-buffer ownership
 * must remain with the established MD1 owner. On failure begin releases only
 * its own temporary acquisition. It MUST NOT change hardware.
 *
 * All other callbacks run between successful begin/end, under this object's
 * mutex; they cannot re-enter this object. check_live() checks fatal/shutdown
 * state without dropping transaction exclusion. update_bits() must serialize
 * with other syscon users and preserve unrelated bits, including MD1 gates.
 * enable_spm() writes ONLY (0x0b16 << 16) | 1 to SPM + 0x000.
 *
 * end() releases temporary exclusion, NEVER the lasting resource leases or
 * clock references of a possibly powered domain. A failed/partial transition
 * cannot be unwound without a genuine quiesce/reset owner; retain it fenced.
 * No callback may manufacture an MPU, reset, power or firmware-ready result.
 */
struct mt6797_md3_power_ops {
	int (*begin)(void *context);
	void (*end)(void *context,
		    const struct mt6797_md3_power_result *result);
	int (*check_live)(void *context);
	int (*read)(void *context, enum mt6797_md3_power_reg reg, u32 *value);
	int (*update_bits)(void *context, enum mt6797_md3_power_reg reg,
			   u32 mask, u32 value);
	int (*enable_spm)(void *context);
};

struct mt6797_md3_power {
	struct mutex lock;
	const struct mt6797_md3_power_ops *ops;
	void *context;
	struct mt6797_md3_power_result result;
};

/* Exactly once before publication; immutable ops/context outlive the object. */
int mt6797_md3_power_init(struct mt6797_md3_power *power,
			const struct mt6797_md3_power_ops *ops, void *context);
/* Sleepable, one shot, no power-off, retry, PLL/CCIF or modem execution. */
int mt6797_md3_power_on(struct mt6797_md3_power *power);
/* Snapshot only; caller retains the object's lifetime. */
int mt6797_md3_power_result(struct mt6797_md3_power *power,
			  struct mt6797_md3_power_result *result);

#endif
