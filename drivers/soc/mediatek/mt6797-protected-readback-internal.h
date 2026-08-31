/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_PROTECTED_READBACK_INTERNAL_H
#define __MT6797_PROTECTED_READBACK_INTERNAL_H

#include <linux/types.h>

#include <linux/soc/mediatek/mt6797-dvfsp-clock-backend.h>

enum mt6797_dvfsp_clock_window {
	MT6797_DVFSP_CLOCK_WINDOW_MCUMIXED,
	MT6797_DVFSP_CLOCK_WINDOW_CSPM,
};

struct mt6797_dvfsp_clock_transport_ops {
	void (*write)(void *context, u32 window, u32 offset, u32 value);
	u32 (*read)(void *context, u32 window, u32 offset);
	void (*delay_us)(void *context, unsigned int usec);
	void (*settle_ns)(void *context, unsigned int nsec);
};

int mt6797_clock_snapshot(const struct mt6797_dvfsp_clock_transport_ops *ops,
			  void *context,
			  struct mt6797_dvfsp_clock_readback *readback);
#include <linux/soc/mediatek/mt6797-bigidvfs-backend.h>

struct mt6797_bigidvfs_transport_ops {
	int (*read)(void *context, u32 address, u32 *value);
};

int
mt6797_bigidvfs_snapshot(const struct mt6797_bigidvfs_transport_ops *ops,
			 void *context,
			 struct mt6797_bigidvfs_readback *readback);

#endif /* __MT6797_PROTECTED_READBACK_INTERNAL_H */
