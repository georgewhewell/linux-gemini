/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MT6797_MD1_FS_H
#define _UAPI_MT6797_MD1_FS_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Experimental, root-only boot authority; no modem-selected AP paths. */
struct mt6797_md1_fs_registration {
	__u32 version; /* 1 */
	__s32 directory_fd;
	__u32 flags; /* must be zero */
	__u32 reserved; /* must be zero */
};

#define MT6797_MD1_FS_REGISTER _IOW('M', 0xf0, struct mt6797_md1_fs_registration)
/* Explicit experimental mode; requires enable_volatile and bounded tmpfs.
 * This never grants write authority to the ext4 backing accepted by REGISTER.
 */
#define MT6797_MD1_FS_REGISTER_RAM _IOW('M', 0xf2, struct mt6797_md1_fs_registration)
/* Atomic three-root RAM authority. Fixed order Z, X, Y; all roots must be
 * bounded writable tmpfs on distinct superblocks. No partial publication.
 */
struct mt6797_md1_fs_ram_roots {
	__u32 version; /* 1 */
	__s32 directory_fd[3]; /* Z, X, Y */
	__u32 flags; /* zero */
	__u32 reserved; /* zero */
};
#define MT6797_MD1_FS_REGISTER_RAM_ROOTS _IOW('M', 0xf3, struct mt6797_md1_fs_ram_roots)
/* Optional sealed MD1BKP1 archive; version1, flags/reserved zero. The
 * directory_fd field carries the archive memfd. Register before RAM roots.
 */
#define MT6797_MD1_FS_REGISTER_ARCHIVE _IOW('M', 0xf4, struct mt6797_md1_fs_registration)
/* Explicit persistent authority for the fixed internal modem partitions.
 * Fixed Z/X/Y order; writable ext4 on mmcblk0p6/p8/p9 only. This does not
 * change the write refusal of REGISTER or REGISTER_RAM_ROOTS.
 */
struct mt6797_md1_fs_nv_roots {
	__u32 version; /* 1 */
	__s32 directory_fd[3]; /* Z=nvdata, X=protect1, Y=protect2 */
	__u32 flags; /* zero */
	__u32 reserved; /* zero */
};
#define MT6797_MD1_FS_REGISTER_NV_ROOTS _IOW('M', 0xf5, struct mt6797_md1_fs_nv_roots)
/* Optional companion authority, not MD1 registration or a start command.
 * The same fixed partitions must already be consumed by the MD1 provider.
 * Each fd identifies its existing md3 sibling, never md or a partition root.
 * No W/V or restore archive is granted. Registration never creates files.
 */
#define MT6797_MD3_FS_REGISTER_NV_ROOTS \
	_IOW('M', 0xf6, struct mt6797_md1_fs_nv_roots)
/* Atomic optional Z/X/Y plus V directory authority. V is the dedicated
 * root-owned gemini-cellular/md3-data directory on internal userdata p32;
 * it grants neither the filesystem root nor a W firmware/restore source.
 * Registration and MD3 consumption are one-shot in the same cold boot.
 */
struct mt6797_md3_fs_roots {
	__u32 version; /* 1 */
	__s32 directory_fd[4]; /* Z, X, Y, V */
	__u32 flags; /* zero */
	__u32 reserved; /* zero */
};
#define MT6797_MD3_FS_REGISTER_ROOTS \
	_IOW('M', 0xf8, struct mt6797_md3_fs_roots)
#define MT6797_MD3_FS_CANCEL _IO('M', 0xf7)
/* Provider backing selector; zero/one preserve the historical RO/RAM values. */
#define MT6797_MD1_FS_BACKING_INTERNAL 2
#define MT6797_MD1_FS_CANCEL _IO('M', 0xf1)

#endif
