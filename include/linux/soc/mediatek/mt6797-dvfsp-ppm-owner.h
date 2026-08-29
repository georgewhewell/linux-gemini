/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_OWNER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_OWNER_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ppm-policy.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ppm.h>
#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>

#define MT6797_DVFSP_PPM_OWNER_ABI		1

/*
 * The state owner holds its external transition lock before taking this
 * owner lock. snapshot() must copy both objects while lock() is held and
 * must not publish a table epoch unrelated to either object.
 */
struct mt6797_dvfsp_ppm_owner_ops {
	u32 abi;
	int (*lock)(void *context);
	void (*unlock)(void *context);
	int (*snapshot)(void *context,
			const struct mt6797_dvfsp_ptp_state *ptp_state,
			struct mt6797_dvfsp_ppm_snapshot *ppm,
			struct mt6797_dvfsp_ppm_policy_snapshot *policy);
};

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_PPM_OWNER_H */
