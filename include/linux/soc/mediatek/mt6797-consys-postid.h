/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hardware-neutral MT6797 CONSYS post-identity bootstrap owner.
 *
 * This is deliberately only the reviewed vendor prefix after identity:
 * MBIST configuration, AFE configuration, and CONMCU reset release.  The
 * backend owns all register access.  There is no Device Tree consumer,
 * platform probe, firmware path, transport, or radio operation here.
 */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTID_H
#define __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTID_H

#include <linux/mutex.h>
#include <linux/soc/mediatek/mt6797-consys.h>

#define MT6797_CONSYS_POSTID_ABI	1

enum mt6797_consys_postid_state {
	MT6797_CONSYS_POSTID_IDLE,
	MT6797_CONSYS_POSTID_ACTIVE,
	MT6797_CONSYS_POSTID_ERROR,
};

/* All callbacks are backend-owned; tests provide only an in-memory fake. */
struct mt6797_consys_postid_ops {
	u32 abi;
	int (*configure_mbist)(void *context);
	int (*configure_afe)(void *context);
	int (*release_reset)(void *context);
	int (*assert_reset)(void *context);
};

struct mt6797_consys_postid_owner {
	struct mt6797_consys_lifecycle *lifecycle;
	const struct mt6797_consys_postid_ops *ops;
	void *context;
	const struct mt6797_consys_runtime_lease *lease;
	struct mutex lock;
	enum mt6797_consys_postid_state state;
};

int mt6797_consys_postid_init(
		struct mt6797_consys_postid_owner *owner,
		struct mt6797_consys_lifecycle *lifecycle,
		const struct mt6797_consys_postid_ops *ops, void *context);

/* @experiment_gate is independent of the identity provider's gate. */
int mt6797_consys_postid_start(
		struct mt6797_consys_postid_owner *owner,
		const struct mt6797_consys_runtime_lease *lease, bool experiment_gate);
int mt6797_consys_postid_stop(struct mt6797_consys_postid_owner *owner);
enum mt6797_consys_postid_state
mt6797_consys_postid_state(struct mt6797_consys_postid_owner *owner);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_CONSYS_POSTID_H */
