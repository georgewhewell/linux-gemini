/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_BACKENDS_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_BACKENDS_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-state-source.h>

struct device;

/*
 * The owner supplies this tuple and keeps each device alive while invoking
 * mt6797_dvfsp_state_source_snapshot(). No device reference is retained here.
 */
struct mt6797_dvfsp_state_source_devices {
	struct device *clock_backend;
	struct device *bigidvfs_backend;
	struct device *thermal;
	struct device *calibration_source;
};

int mt6797_dvfsp_state_source_backend_read_clock(
	void *context, struct mt6797_dvfsp_clock_readback *readback);
int mt6797_dvfsp_state_source_backend_read_big(
	void *context, struct mt6797_bigidvfs_readback *readback);
int mt6797_dvfsp_state_source_backend_read_eem(
	void *context, struct mt6797_eem_readback *readback);
int mt6797_dvfsp_state_source_backend_read_ptp(
	void *context, struct mt6797_dvfsp_ptp_handoff *handoff);
int mt6797_dvfsp_state_source_backend_read_ptp_identity(
	void *context, struct mt6797_dvfsp_efuse_identity *identity);

/*
 * Initialize only the three readback callbacks. The caller must still assign
 * fill_calibration and fill_live before using the source adapter; leaving
 * either unset is rejected by its fail-closed callback check.
 */
int mt6797_dvfsp_state_source_backend_ops_init(
	const struct mt6797_dvfsp_state_source_devices *devices,
	struct mt6797_dvfsp_state_source_ops *ops);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_STATE_SOURCE_BACKENDS_H */
