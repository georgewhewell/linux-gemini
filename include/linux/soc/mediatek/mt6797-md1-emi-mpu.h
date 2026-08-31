/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6797 MD1 secure EMI-MPU provider contract.
 *
 * The vendor MD1 operation is the secure-monitor SET transaction.  It has
 * neither a source-backed readback nor an inverse.  This interface therefore
 * treats the first attempted SMC as a boot-lifetime commitment: any failed or
 * incomplete sequence is fatal and requires a reset/reboot before another
 * MD1 MPU sequence can be considered.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_EMI_MPU_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_EMI_MPU_H

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-md1-mpu-remap-plan.h>

struct device;

#define MT6797_MD1_EMI_MPU_ABI		1
#define MT6797_MD1_EMI_MPU_SMC_SET	0x82000209
#define MT6797_MD1_EMI_MPU_PERMISSION_MASK	GENMASK(26, 0)
#define MT6797_MD1_EMI_MPU_REGION_SHIFT	27

enum mt6797_md1_emi_mpu_phase {
	MT6797_MD1_EMI_MPU_INITIAL,
	MT6797_MD1_EMI_MPU_STAGE1,
};

enum mt6797_md1_emi_mpu_state {
	MT6797_MD1_EMI_MPU_IDLE,
	MT6797_MD1_EMI_MPU_INITIAL_COMMITTED,
	MT6797_MD1_EMI_MPU_STAGE1_COMMITTED,
	MT6797_MD1_EMI_MPU_FATAL,
};

/* The callback returns the captured, normalized SIP result for one region. */
typedef int (*mt6797_md1_emi_mpu_set_fn)(void *context, u64 start, u64 end,
					 u32 region_permission);
typedef int (*mt6797_md1_emi_mpu_begin_fn)(void *context);

struct mt6797_md1_emi_mpu_core {
	struct mutex lock;
	enum mt6797_md1_emi_mpu_state state;
};

#define MT6797_MD1_EMI_MPU_CORE_INITIALIZER(core) \
	{ .lock = __MUTEX_INITIALIZER((core).lock) }

void mt6797_md1_emi_mpu_core_init(struct mt6797_md1_emi_mpu_core *core);
int mt6797_md1_emi_mpu_core_apply(struct mt6797_md1_emi_mpu_core *core,
	mt6797_md1_emi_mpu_begin_fn begin, mt6797_md1_emi_mpu_set_fn set,
	void *context, const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count);
void mt6797_md1_emi_mpu_core_latch_fatal(struct mt6797_md1_emi_mpu_core *core);
enum mt6797_md1_emi_mpu_state
mt6797_md1_emi_mpu_core_state(struct mt6797_md1_emi_mpu_core *core);

/*
 * The platform implementation finds the provider through a consumer's
 * mediatek,emi-mpu phandle.  It performs no action until both its disabled DT
 * node and its read-only module gate have been enabled.
 */
int mt6797_md1_emi_mpu_apply(struct device *consumer,
	const struct mt6797_md1_mpu_remap_plan *plan,
	enum mt6797_md1_emi_mpu_phase phase,
	const struct mt6797_md1_mpu_plan_entry *entries, size_t count);
void mt6797_md1_emi_mpu_latch_fatal(void);
bool mt6797_md1_emi_mpu_is_fatal(void);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_EMI_MPU_H */
