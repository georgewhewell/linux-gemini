/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_IDENTITY_SOURCE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_IDENTITY_SOURCE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-ptp.h>
#include <linux/soc/mediatek/mt6797-dvfsp-state-owner-source.h>

#define MT6797_DVFSP_IDENTITY_SOURCE_ABI	1

/*
 * These callbacks are the provider's provenance boundary. The bridge never
 * invents an epoch or calibration handle and never reads a mutable table
 * without the provider's explicit source contract.
 */
struct mt6797_dvfsp_identity_provenance_ops {
	u32 abi;
	int (*read_table_epoch)(void *context,
				const struct mt6797_dvfsp_ptp_identity *identity,
				u64 *table_epoch);
	int (*read_calibration_handle)(void *context,
					const struct mt6797_dvfsp_ptp_identity *identity,
					u64 *calibration_handle);
};

/* Device pointers are borrowed and remain owned by the caller. */
struct mt6797_dvfsp_identity_source {
	struct mt6797_dvfsp_state_source_devices devices;
	const struct mt6797_dvfsp_identity_provenance_ops *provenance_ops;
	void *provenance_context;
};

int mt6797_dvfsp_identity_source_read(
	void *context, const struct mt6797_dvfsp_ptp_state *ptp_state,
	struct mt6797_dvfsp_state_owner_source_identity *identity);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_IDENTITY_SOURCE_H */
