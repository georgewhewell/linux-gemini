/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_MD1_POWER_H
#define __MT6797_MD1_POWER_H

#include <linux/types.h>

enum mt6797_md1_power_state {
	MT6797_MD1_POWER_OFF,
	MT6797_MD1_POWERED,
	MT6797_MD1_POWER_UNSTABLE,
};

enum mt6797_md1_power_state
mt6797_md1_power_state_decode(u32 status, u32 status_2nd);

#endif /* __MT6797_MD1_POWER_H */
