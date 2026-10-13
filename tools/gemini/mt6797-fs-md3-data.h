/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed Linux-owned V directory. Creation is explicit and preserves all
 * existing contents. No mount, raw filesystem, quota, seed or stock restore.
 */
static int md3_data_private_open(int parent, const char *name, dev_t device)
{
	struct stat st;
	int fd, ret, made = 0;

	fd = no_symlink_dir(parent, name, RESOLVE_BENEATH | RESOLVE_NO_XDEV);
	if (fd == -ENOENT) {
		if (mkdirat(parent, name, 0700) && errno != EEXIST)
			return -errno;
		made = 1;
		fd = no_symlink_dir(parent, name, RESOLVE_BENEATH | RESOLVE_NO_XDEV);
	}
	if (fd < 0)
		return fd;
	ret = fstat(fd, &st) ? -errno : 0;
	if (!ret && (!S_ISDIR(st.st_mode) || (st.st_mode & 07777) != 0700 ||
		     st.st_uid || st.st_gid || st.st_dev != device))
		ret = -EACCES;
	if (!ret && made && (fsync(fd) || fsync(parent)))
		ret = -errno;
	if (ret) {
		close(fd);
		return ret;
	}
	return fd;
}

static int md3_data_root_open(void)
{
	static const struct fs_source source = {
		"/dev/mmcblk0p32", "PARTNAME=userdata", "PARTN=32",
		"22249472\n", UINT64_C(22249472) * 512,
	};
	struct stat st;
	struct statfs fs;
	struct statvfs vfs;
	char event[1024], sectors[64];
	int persist = -1, parent = -1, root = -1, ret;

	failure_stage = "md3-data-partition";
	ret = read_small("/sys/class/block/mmcblk0p32/uevent", event, sizeof(event));
	if (!ret)
		ret = read_small("/sys/class/block/mmcblk0p32/size", sectors, sizeof(sectors));
	if (!ret)
		ret = partition_identity_for(event, sectors, &source);
	if (ret)
		return ret;
	failure_stage = "md3-data-persist";
	persist = no_symlink_dir(AT_FDCWD, "/persist", 0);
	if (persist < 0)
		return persist;
	ret = -errno;
	if (fstat(persist, &st) || fstatfs(persist, &fs) || fstatvfs(persist, &vfs)) {
		ret = -errno;
		goto out;
	}
	ret = -EXDEV;
	if (!S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022) ||
	    st.st_ino != 2 || major(st.st_dev) != MMC_BLOCK_MAJOR ||
	    minor(st.st_dev) != 32 || fs.f_type != EXT4_SUPER_MAGIC ||
	    (vfs.f_flag & ST_RDONLY) || vfs.f_frsize < 512 ||
	    vfs.f_frsize > 65536 || vfs.f_frsize % 512 ||
	    !vfs.f_blocks || vfs.f_blocks > source.bytes / vfs.f_frsize)
		goto out;
	failure_stage = "md3-data-space";
	ret = -ENOSPC;
	if (vfs.f_bavail > vfs.f_blocks || vfs.f_ffree > vfs.f_files ||
	    vfs.f_ffree <= 128 ||
	    vfs.f_bavail <= (64ULL * 1024 * 1024) / vfs.f_frsize + 2)
		goto out;
	failure_stage = "md3-data-parent";
	parent = md3_data_private_open(persist, "gemini-cellular", st.st_dev);
	if (parent < 0) { ret = parent; goto out; }
	failure_stage = "md3-data-root";
	root = md3_data_private_open(parent, "md3-data", st.st_dev);
	ret = root;
out:
	if (parent >= 0)
		close(parent);
	close(persist);
	return ret;
}
