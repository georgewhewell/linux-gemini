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
static const char *failure_stage = "arguments";

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

static int nvdata_authority(const char *mountpoint, int publish, const char *ramdir)
{
	struct mt6797_md1_fs_registration registration = { .version = 1 };
	struct stat block_stat, root_stat, md_stat;
	struct statfs fs;
	struct statvfs vfs;
	unsigned char super[1024];
	char path[128], uevent[1024], sectors[64];
	uint64_t bytes;
	int block = -1, root = -1, md = -1, provider = -1;
	int ram = -1, ram_md = -1;
	int ret;

	failure_stage = "nvdata-block-open";
	block = open("/dev/mmcblk0p6", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (block < 0)
		return -errno;
	failure_stage = "nvdata-block-stat";
	if (fstat(block, &block_stat)) {
		ret = -errno;
		goto out;
	}
	ret = -ENODEV;
	if (!S_ISBLK(block_stat.st_mode))
		goto out;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/uevent",
		 major(block_stat.st_rdev), minor(block_stat.st_rdev));
	failure_stage = "nvdata-sysfs-uevent";
	ret = read_small(path, uevent, sizeof(uevent));
	if (ret)
		goto out;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/size",
		 major(block_stat.st_rdev), minor(block_stat.st_rdev));
	failure_stage = "nvdata-sysfs-size";
	ret = read_small(path, sectors, sizeof(sectors));
	if (ret)
		goto out;
	failure_stage = "nvdata-partition-identity";
	ret = partition_identity(uevent, sectors);
	if (ret)
		goto out;
	failure_stage = "nvdata-block-size";
	if (ioctl(block, BLKGETSIZE64, &bytes)) {
		ret = -errno;
		goto out;
	}
	/* Only the ext4 superblock, not directory entries or provision contents. */
	failure_stage = "nvdata-superblock-read";
	if (pread(block, super, sizeof(super), 1024) != sizeof(super)) {
		ret = -EIO;
		goto out;
	}
	failure_stage = "nvdata-superblock-validation";
	ret = clean_ext4(super, bytes);
	if (ret)
		goto out;
	/* Pre-mount mode shares every block metadata check, but opens neither
	 * filesystem directories nor the provider and can never issue REGISTER.
	 */
	if (!mountpoint)
		goto out;
	failure_stage = "nvdata-mount-root-open";
	root = no_symlink_dir(AT_FDCWD, mountpoint, 0);
	if (root < 0) {
		ret = root;
		goto out;
	}
	failure_stage = "nvdata-mount-root-stat";
	if (fstat(root, &root_stat) || fstatfs(root, &fs) || fstatvfs(root, &vfs)) {
		ret = -errno;
		goto out;
	}
	failure_stage = "nvdata-mount-root-validation";
	ret = -EROFS;
	if (root_stat.st_dev != block_stat.st_rdev || root_stat.st_ino != 2 ||
	    fs.f_type != EXT4_SUPER_MAGIC || !(vfs.f_flag & ST_RDONLY))
		goto out;
	failure_stage = "nvdata-md-directory-open";
	md = no_symlink_dir(root, "md", RESOLVE_BENEATH | RESOLVE_NO_XDEV);
	if (md < 0) {
		ret = md;
		goto out;
	}
	failure_stage = "nvdata-md-directory-stat";
	if (fstat(md, &md_stat)) {
		ret = -errno;
		goto out;
	}
	ret = -ENODEV;
	if (!S_ISDIR(md_stat.st_mode) || md_stat.st_dev != block_stat.st_rdev)
		goto out;
	if (!publish) {
		ret = 0;
		goto out;
	}
	if (ramdir) {
		failure_stage = "ram-root-open";
		ram = no_symlink_dir(AT_FDCWD, ramdir, 0);
		if (ram < 0) { ret = ram; goto out; }
		failure_stage = "ram-root-stat";
		if (fstat(ram, &root_stat) || fstatfs(ram, &fs) || fstatvfs(ram, &vfs)) {
			ret = -errno; goto out;
		}
		failure_stage = "ram-root-validation";
		ret = -EINVAL;
		if (!S_ISDIR(root_stat.st_mode) || root_stat.st_uid ||
		    (root_stat.st_mode & 0777) != 0700 || root_stat.st_ino != 1 ||
		    root_stat.st_dev == block_stat.st_rdev || fs.f_type != TMPFS_MAGIC ||
		    (vfs.f_flag & ST_RDONLY) || fs.f_bsize <= 0 || !fs.f_blocks ||
		    fs.f_blocks > (UINT64_C(64) * 1024 * 1024) / fs.f_bsize ||
		    !fs.f_files || fs.f_files > 4096)
			goto out;
		failure_stage = "ram-md-open";
		ram_md = no_symlink_dir(ram, "md", RESOLVE_BENEATH | RESOLVE_NO_XDEV);
		if (ram_md < 0) { ret = ram_md; goto out; }
		failure_stage = "ram-md-stat";
		if (fstat(ram_md, &md_stat)) { ret = -errno; goto out; }
		ret = -EINVAL;
		if (!S_ISDIR(md_stat.st_mode) || md_stat.st_dev != root_stat.st_dev)
			goto out;
	}
	failure_stage = "provider-device-open";
	provider = open("/dev/mt6797-md1-fs-provider", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (provider < 0) {
		ret = -errno;
		goto out;
	}
	registration.directory_fd = ramdir ? ram_md : md;
	/* One call only. An interrupted result is ambiguous: never retry GO. */
	failure_stage = "provider-register-once";
	ret = ioctl(provider, ramdir ? MT6797_MD1_FS_REGISTER_RAM :
		    MT6797_MD1_FS_REGISTER, &registration) ? -errno : 0;
out:
	if (ram_md >= 0)
		close(ram_md);
	if (ram >= 0)
		close(ram);
	if (provider >= 0)
		close(provider);
	if (md >= 0)
		close(md);
	if (root >= 0)
		close(root);
	close(block);
	return ret;
}

static int register_nvdata(const char *mountpoint)
{
	return nvdata_authority(mountpoint, 1, NULL);
}

int main(int argc, char **argv)
{
	int ret, published = 0;

	if (geteuid())
		goto usage;
	if (argc == 2 && !strcmp(argv[1], "--check-device"))
		ret = register_nvdata(NULL);
	else if (argc == 2 && argv[1][0] == '/') {
		ret = register_nvdata(argv[1]);
		published = 1;
	} else if (argc == 3 && !strcmp(argv[1], "--check-mount") && argv[2][0] == '/')
		ret = nvdata_authority(argv[2], 0, NULL);
	else if (argc == 4 && !strcmp(argv[1], "--ram") &&
		 argv[2][0] == '/' && argv[3][0] == '/') {
		ret = nvdata_authority(argv[2], 1, argv[3]);
		published = 1;
	} else
		goto usage;
	if (ret) {
		fprintf(stderr, "MD1 FS authority failed stage=%s error=%s; no retry\n",
			failure_stage, strerror(-ret));
		return 1;
	}
	puts(published ? "MD1 FS authority published once; this is not an HS2/AT result" :
	     "MD1 nvdata metadata precheck passed; no provider registration");
	return 0;
usage:
	fprintf(stderr, "usage (root only): mt6797-fs-register --check-device | /absolute/ro-mount | --check-mount /absolute/ro-mount | --ram /absolute/ro-mount /absolute/ram-mount\n");
	return 2;
}
