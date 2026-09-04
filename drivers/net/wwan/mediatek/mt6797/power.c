// SPDX-License-Identifier: GPL-2.0-only
/* Pure helpers for fail-closed MT6797 MD1 power-state observation. */

#include <linux/bits.h>

#include "power.h"

#define MT6797_MD1_POWER_STATUS	BIT(0)

enum mt6797_md1_power_state
mt6797_md1_power_state_decode(u32 status, u32 status_2nd)
{
	bool primary = status & MT6797_MD1_POWER_STATUS;
	bool secondary = status_2nd & MT6797_MD1_POWER_STATUS;

	if (primary != secondary)
		return MT6797_MD1_POWER_UNSTABLE;

	return primary ? MT6797_MD1_POWERED : MT6797_MD1_POWER_OFF;
}
