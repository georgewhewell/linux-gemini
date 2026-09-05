// SPDX-License-Identifier: GPL-2.0-only
/* One-shot registration of the proven nvdata:/md metadata authority.
 * Never mounts, repairs, enumerates provision files, or executes Android code.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "../../include/uapi/linux/mt6797_md1_fs.h"

#define NVDATA_BYTES UINT64_C(33554432)
#define NVDATA_SECTORS "65536\n"

static uint16_t get16(const unsigned char *p)
{
	return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const unsigned char *p)
{
	return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static int partition_identity(const char *uevent, const char *sectors)
{
	const char *p = uevent;
	unsigned int labels = 0, numbers = 0;

	while (*p) {
		const char *end = strchr(p, '\n');
		size_t n;

		if (!end)
			return -EINVAL;
		n = end - p;
		if (n >= 9 && !memcmp(p, "PARTNAME=", 9)) {
			if (n != 15 || memcmp(p, "PARTNAME=nvdata", 15))
				return -ENODEV;
			labels++;
		}
		if (n >= 6 && !memcmp(p, "PARTN=", 6)) {
			if (n != 7 || memcmp(p, "PARTN=6", 7))
				return -ENODEV;
			numbers++;
		}
		p = end + 1;
	}
	return labels == 1 && numbers == 1 && !strcmp(sectors, NVDATA_SECTORS) ?
		0 : -ENODEV;
}

static int clean_ext4(const unsigned char sb[1024], uint64_t bytes)
{
	uint32_t log_block = get32(sb + 24);
	uint32_t incompat = get32(sb + 96);
	uint64_t blocks = get32(sb + 4);
	uint16_t state = get16(sb + 58);

	if (bytes != NVDATA_BYTES || get16(sb + 56) != EXT4_SUPER_MAGIC)
		return -ENODEV;
	/* Refuse dirty/error/orphan state and pending journal recovery. No repair. */
	if (state != 1 || (incompat & 4) || get32(sb + 0xe8) ||
	    (get32(sb + 0x64) & 0x10000))
		return -EUCLEAN;
	if (incompat & 0x80) /* 64-bit block-count format */
		blocks |= (uint64_t)get32(sb + 0x150) << 32;
	if (log_block > 6 || !blocks || blocks > (bytes >> (10 + log_block)))
		return -EINVAL;
	return 0;
}

static int read_small(const char *path, char *out, size_t capacity)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ssize_t n;
	int ret;

	if (fd < 0)
		return -errno;
	n = read(fd, out, capacity);
	ret = n < 0 ? -errno : 0;
	close(fd);
	if (ret)
		return ret;
	if (!n || (size_t)n >= capacity || memchr(out, 0, n))
		return -EINVAL;
	out[n] = 0;
	return 0;
}

static int no_symlink_dir(int dirfd, const char *path, uint64_t resolve)
{
	struct open_how how = {
		.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
		.resolve = resolve | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
	};
	int fd = syscall(SYS_openat2, dirfd, path, &how, sizeof(how));

	return fd < 0 ? -errno : fd;
}

static int register_nvdata(const char *mountpoint)
{
	struct mt6797_md1_fs_registration registration = { .version = 1 };
	struct stat block_stat, root_stat, md_stat;
	struct statfs fs;
	struct statvfs vfs;
	unsigned char super[1024];
	char path[128], uevent[1024], sectors[64];
	uint64_t bytes;
	int block = -1, root = -1, md = -1, provider = -1;
	int ret;

	block = open("/dev/mmcblk0p6", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (block < 0)
		return -errno;
	if (fstat(block, &block_stat)) {
		ret = -errno;
		goto out;
	}
	ret = -ENODEV;
	if (!S_ISBLK(block_stat.st_mode))
		goto out;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/uevent",
		 major(block_stat.st_rdev), minor(block_stat.st_rdev));
	ret = read_small(path, uevent, sizeof(uevent));
	if (ret)
		goto out;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/size",
		 major(block_stat.st_rdev), minor(block_stat.st_rdev));
	ret = read_small(path, sectors, sizeof(sectors));
	if (ret)
		goto out;
	ret = partition_identity(uevent, sectors);
	if (ret)
		goto out;
	if (ioctl(block, BLKGETSIZE64, &bytes)) {
		ret = -errno;
		goto out;
	}
	/* Only the ext4 superblock, not directory entries or provision contents. */
	if (pread(block, super, sizeof(super), 1024) != sizeof(super)) {
		ret = -EIO;
		goto out;
	}
	ret = clean_ext4(super, bytes);
	if (ret)
		goto out;
	root = no_symlink_dir(AT_FDCWD, mountpoint, 0);
	if (root < 0) {
		ret = root;
		goto out;
	}
	if (fstat(root, &root_stat) || fstatfs(root, &fs) || fstatvfs(root, &vfs)) {
		ret = -errno;
		goto out;
	}
	ret = -EROFS;
	if (root_stat.st_dev != block_stat.st_rdev || root_stat.st_ino != 2 ||
	    fs.f_type != EXT4_SUPER_MAGIC || !(vfs.f_flag & ST_RDONLY))
		goto out;
	md = no_symlink_dir(root, "md", RESOLVE_BENEATH | RESOLVE_NO_XDEV);
	if (md < 0) {
		ret = md;
		goto out;
	}
	if (fstat(md, &md_stat)) {
		ret = -errno;
		goto out;
	}
	ret = -ENODEV;
	if (!S_ISDIR(md_stat.st_mode) || md_stat.st_dev != block_stat.st_rdev)
		goto out;
	provider = open("/dev/mt6797-md1-fs-provider", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (provider < 0) {
		ret = -errno;
		goto out;
	}
	registration.directory_fd = md;
	/* One call only. An interrupted result is ambiguous: never retry GO. */
	ret = ioctl(provider, MT6797_MD1_FS_REGISTER, &registration) ? -errno : 0;
out:
	if (provider >= 0)
		close(provider);
	if (md >= 0)
		close(md);
	if (root >= 0)
		close(root);
	close(block);
	return ret;
}

int main(int argc, char **argv)
{
	int ret;

	if (argc != 2 || argv[1][0] != '/' || geteuid()) {
		fprintf(stderr, "usage (root only): mt6797-fs-register /absolute/verified-ro-nvdata-mount\n");
		return 2;
	}
	ret = register_nvdata(argv[1]);
	if (ret) {
		fprintf(stderr, "MD1 FS authority registration failed: %s; no retry\n", strerror(-ret));
		return 1;
	}
	puts("MD1 FS authority published once; this is not an HS2/AT result");
	return 0;
}
