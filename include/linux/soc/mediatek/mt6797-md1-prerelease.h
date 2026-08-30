/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H
#define __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H

#include <linux/mutex.h>
#include <linux/types.h>

/* A backend operation returns -EAGAIN while its bounded observation is pending. */
#define MT6797_MD1_PRERELEASE_ABI		1
#define MT6797_MD1_PRERELEASE_MAX_POLLS	3

enum mt6797_md1_prerelease_state {
	MT6797_MD1_PRERELEASE_OFF,
	MT6797_MD1_PRERELEASE_ERROR,
};

struct mt6797_md1_prerelease_ops {
	u32 abi;
	int (*pmic_on)(void *context);
	int (*pmic_off)(void *context);
	int (*domain_on)(void *context);
	int (*domain_off)(void *context);
	int (*poll)(void *context);
};

struct mt6797_md1_prerelease {
	const struct mt6797_md1_prerelease_ops *ops;
	void *context;
	struct mutex lock;
	enum mt6797_md1_prerelease_state state;
};

int mt6797_md1_prerelease_init(struct mt6797_md1_prerelease *prerelease,
		const struct mt6797_md1_prerelease_ops *ops, void *context);
int mt6797_md1_prerelease_run(struct mt6797_md1_prerelease *prerelease);
enum mt6797_md1_prerelease_state
mt6797_md1_prerelease_state(struct mt6797_md1_prerelease *prerelease);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_MD1_PRERELEASE_H */
