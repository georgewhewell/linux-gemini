/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD1_FS_PROVIDER_H
#define _MT6797_MD1_FS_PROVIDER_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct mt6797_md1_fs_stats {
	u32 block_size;
	u32 blocks;
	u32 free_blocks;
};

#if IS_ENABLED(CONFIG_MTK_MT6797_MD1_FS_PROVIDER)
bool mt6797_md1_fs_provider_enabled(void);
int mt6797_md1_fs_provider_require(struct device *cold);
int mt6797_md1_fs_provider_claim(struct device *cold);
int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats);
/* Process context only; direct-child metadata, never opens file contents. */
int mt6797_md1_fs_provider_getattr(const char *name, unsigned int *mode);
#else
static inline bool mt6797_md1_fs_provider_enabled(void) { return false; }
static inline int mt6797_md1_fs_provider_require(struct device *cold) { return 0; }
static inline int mt6797_md1_fs_provider_claim(struct device *cold) { return 0; }
static inline int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_getattr(const char *name, unsigned int *mode)
{
	return -ENODEV;
}
#endif
#endif
