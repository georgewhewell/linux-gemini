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
#define MT6797_MD1_FS_CANCEL _IO('M', 0xf1)

#endif
