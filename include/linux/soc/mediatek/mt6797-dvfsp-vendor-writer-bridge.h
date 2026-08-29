/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_BRIDGE_H
#define __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_BRIDGE_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-vendor-writer.h>

#define MT6797_DVFSP_VENDOR_WRITER_BRIDGE_ABI	1

/*
 * Keep site an unsigned integer at this source boundary: the pinned vendor
 * tree has a private enum and must validate the same numeric IDs itself.
 */
struct mt6797_dvfsp_vendor_writer_bridge_ops {
	u32 abi;
	int (*begin)(void *context, unsigned int site, u64 *generation);
	int (*commit)(void *context, unsigned int site, u64 generation);
	int (*abort)(void *context, unsigned int site, u64 generation);
	int (*read_identity)(void *context, u64 *owner_handle,
				    u64 *transition_handle);
};

struct mt6797_dvfsp_vendor_writer_bridge {
	struct mt6797_dvfsp_vendor_writer *writer;
	unsigned int active_site;
	u64 active_generation;
	bool active;
};

int mt6797_dvfsp_vendor_writer_bridge_init(
	struct mt6797_dvfsp_vendor_writer_bridge *bridge,
	struct mt6797_dvfsp_vendor_writer *writer);

const struct mt6797_dvfsp_vendor_writer_bridge_ops *
mt6797_dvfsp_vendor_writer_bridge_ops(void);

#endif /* __LINUX_SOC_MEDIATEK_MT6797_DVFSP_VENDOR_WRITER_BRIDGE_H */
