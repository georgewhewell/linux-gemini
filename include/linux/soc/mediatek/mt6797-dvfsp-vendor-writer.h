/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_H

#include <linux/mutex.h>
#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-live-source.h>

#define MT6797_DVFSP_VENDOR_WRITER_ABI	1

/* The vendor writer supplies provenance while the shared lock is held. */
struct mt6797_dvfsp_vendor_writer_ops {
	u32 abi;
	int (*read_provenance)(void *context,
				      struct mt6797_dvfsp_state_provenance *provenance);
};

/* This object is the one transition owner used by writer and reader paths. */
struct mt6797_dvfsp_vendor_writer {
	struct mutex *transition_lock;
	struct mt6797_dvfsp_vendor_writer_ops ops;
	void *context;
	u64 *generation;
	u64 owner_handle;
	u64 transition_handle;
	bool initialized;
	bool update_held;
};

int mt6797_dvfsp_vendor_writer_init(
	struct mt6797_dvfsp_vendor_writer *writer,
	struct mutex *transition_lock,
	u64 *generation,
	const struct mt6797_dvfsp_vendor_writer_ops *ops,
	void *context, u64 owner_handle, u64 transition_handle);

int mt6797_dvfsp_vendor_writer_begin(
	struct mt6797_dvfsp_vendor_writer *writer, u64 *generation);
typedef int (*mt6797_dvfsp_vendor_writer_update_fn)(void *context,
							   u64 generation);

enum mt6797_dvfsp_vendor_writer_site_id {
	MT6797_DVFSP_VENDOR_WRITER_SITE_PTP_TABLE = 0,
	MT6797_DVFSP_VENDOR_WRITER_SITE_VOLTAGE,
	MT6797_DVFSP_VENDOR_WRITER_SITE_PPM,
	MT6797_DVFSP_VENDOR_WRITER_SITE_OBSERVE,
	MT6797_DVFSP_VENDOR_WRITER_SITE_COUNT,
};

#define MT6797_DVFSP_VENDOR_WRITER_SITE_ABI	1

struct mt6797_dvfsp_vendor_writer_site {
	u32 abi;
	enum mt6797_dvfsp_vendor_writer_site_id id;
	mt6797_dvfsp_vendor_writer_update_fn update;
	void *context;
};

/* Acquire the owner before the caller enters its vendor critical section. */
int mt6797_dvfsp_vendor_writer_run(
	struct mt6797_dvfsp_vendor_writer *writer,
	mt6797_dvfsp_vendor_writer_update_fn update, void *context);
int mt6797_dvfsp_vendor_writer_run_site(
	struct mt6797_dvfsp_vendor_writer *writer,
	const struct mt6797_dvfsp_vendor_writer_site *site);
int mt6797_dvfsp_vendor_writer_commit(
	struct mt6797_dvfsp_vendor_writer *writer);
int mt6797_dvfsp_vendor_writer_abort(
	struct mt6797_dvfsp_vendor_writer *writer);

/* Called by the live-source adapter while the shared transition lock is held. */
int mt6797_dvfsp_vendor_writer_read_generation(
	void *context, const struct mt6797_dvfsp_clock_state *clock,
	const struct mt6797_dvfsp_calibration_state *calibration,
	const struct mt6797_dvfsp_cspm_state *cspm,
	const struct mt6797_dvfsp_ppm_snapshot *ppm, u64 *generation,
	u64 *owner_handle, u64 *transition_handle,
	struct mt6797_dvfsp_state_provenance *provenance);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_H */
