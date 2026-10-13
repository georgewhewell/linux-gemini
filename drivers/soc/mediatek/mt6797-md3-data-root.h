/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional V authority: a directory on existing internal Linux userdata.
 * Included after md3_roots/data_live; no independent mount or storage owner.
 */
#ifndef _MT6797_MD3_DATA_ROOT_H
#define _MT6797_MD3_DATA_ROOT_H

#define MT6797_MD3_DATA_SECTORS 22249472ULL
#define MT6797_MD3_DATA_RESERVE (64ULL * 1024 * 1024)

static bool mt6797_fs_md3_private_dir(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	return d_is_dir(dentry) && uid_eq(inode->i_uid, GLOBAL_ROOT_UID) &&
		gid_eq(inode->i_gid, GLOBAL_ROOT_GID) &&
		(READ_ONCE(inode->i_mode) & 07777) == 0700;
}

/* Geometry is the existing provisioned p32, not the original Android size.
 * Directory confinement below is still mandatory: this is not a p32 grant.
 */
static bool mt6797_fs_md3_data_backing(struct file *file)
{
	struct super_block *sb;
	struct block_device *bdev;

	if (!file)
		return false;
	sb = file_inode(file)->i_sb;
	bdev = sb->s_bdev;
	return sb->s_magic == EXT4_SUPER_MAGIC && bdev &&
		MAJOR(bdev->bd_dev) == MMC_BLOCK_MAJOR && MINOR(bdev->bd_dev) == 32 &&
		bdev_nr_sectors(bdev) == MT6797_MD3_DATA_SECTORS &&
		!(READ_ONCE(sb->s_flags) & SB_RDONLY) &&
		!(READ_ONCE(file->f_path.mnt->mnt_flags) & MNT_READONLY) &&
		mnt_idmap(file->f_path.mnt) == &nop_mnt_idmap;
}

/* Only the consumed, boot-pinned fourth root can authorize p32 files.
 * The release/acquire flag is never cleared: CANCEL is forbidden after claim.
 * No callback/VFS operation; root_get_for may call this under provider_lock.
 */
static bool mt6797_fs_md3_data_writable(struct file *file)
{
	struct file *root;

	if (!smp_load_acquire(&md3_data_live) || !file)
		return false;
	root = md3_roots[3];
	return root && mt6797_fs_md3_data_backing(file) &&
		mt6797_fs_md3_private_dir(root->f_path.dentry) &&
		file->f_path.mnt == root->f_path.mnt &&
		file_inode(file)->i_sb == file_inode(root)->i_sb &&
		(!d_is_reg(file->f_path.dentry) || file_inode(file)->i_nlink == 1) &&
		is_subdir(file->f_path.dentry, root->f_path.dentry);
}

static bool mt6797_fs_md3_data_entry(struct file *root, struct dentry *child)
{
	return !mt6797_fs_md3_data_writable(root) || !d_is_reg(child) ||
		d_inode(child)->i_nlink == 1;
}

static int mt6797_fs_md3_data_statfs(struct file *file, struct kstatfs *fs)
{
	int ret;

	if (!mt6797_fs_md3_data_backing(file))
		return -EROFS;
	ret = vfs_statfs(&file->f_path, fs);
	if (ret)
		return ret;
	if (fs->f_type != EXT4_SUPER_MAGIC || fs->f_bsize < 512 ||
	    fs->f_bsize > 65536 || fs->f_bsize % 512 || !fs->f_blocks ||
	    fs->f_blocks > MT6797_MD3_DATA_SECTORS * 512 / fs->f_bsize ||
	    fs->f_bfree > fs->f_blocks || fs->f_bavail > fs->f_bfree ||
	    !fs->f_files || fs->f_ffree > fs->f_files)
		return -EOVERFLOW;
	return mt6797_fs_md3_data_backing(file) ? 0 : -EROFS;
}

static int mt6797_fs_md3_data_stats(struct file *file,
				   struct mt6797_md1_fs_stats *stats)
{
	struct kstatfs fs;
	u64 reserve;
	int ret;

	ret = mt6797_fs_md3_data_statfs(file, &fs);
	if (ret)
		return ret;
	reserve = DIV_ROUND_UP_ULL(MT6797_MD3_DATA_RESERVE, fs.f_bsize);
	/* V shares Linux userdata. Report actual geometry and currently usable
	 * blocks above the local reserve, never a fabricated isolated volume.
	 */
	fs.f_bfree = fs.f_bavail > reserve ? fs.f_bavail - reserve : 0;
	return mt6797_fs_stats_convert(&fs, stats);
}

/* A best-effort low-space admission guard, not a quota against other AP
 * writers. Real VFS/ENOSPC/writeback results remain authoritative. Per-file
 * 16MiB, path, request and recursive-operation bounds remain unchanged.
 */
static int mt6797_fs_md3_data_space(struct file *file, size_t additional)
{
	struct kstatfs fs;
	u64 reserve, required;
	int ret;

	if (!mt6797_fs_md3_data_writable(file))
		return 0; /* Existing NV/tmpfs policy is unchanged. */
	ret = mt6797_fs_md3_data_statfs(file, &fs);
	if (ret)
		return ret;
	reserve = DIV_ROUND_UP_ULL(MT6797_MD3_DATA_RESERVE, fs.f_bsize);
	required = DIV_ROUND_UP_ULL(additional, fs.f_bsize);
	return fs.f_ffree > 128 && fs.f_bavail >= reserve &&
		required <= fs.f_bavail - reserve ? 0 : -ENOSPC;
}

static int mt6797_fs_validate_md3_data_root(struct file *file)
{
	struct dentry *dir = file->f_path.dentry, *parent, *top;
	struct kstatfs fs;
	int ret;

	if (!mt6797_fs_md3_data_backing(file) || (file->f_mode & FMODE_WRITE))
		return -EROFS;
	if (!mt6797_fs_md3_private_dir(dir))
		return -EACCES;
	parent = dget_parent(dir);
	top = dget_parent(parent);
	ret = -EXDEV;
	if (parent == dir || top == parent || top != file_inode(file)->i_sb->s_root ||
	    file->f_path.mnt->mnt_root != top ||
	    !uid_eq(d_inode(top)->i_uid, GLOBAL_ROOT_UID) ||
	    (READ_ONCE(d_inode(top)->i_mode) & 0022))
		goto out;
	/* Root then parent order pins both names and rejects rename/mount aliases
	 * during validation. No modem lookup ever starts above the retained V FD.
	 */
	inode_lock(d_inode(top));
	inode_lock_nested(d_inode(parent), I_MUTEX_CHILD);
	if (dir->d_parent == parent && parent->d_parent == top &&
	    !d_mountpoint(dir) && !d_mountpoint(parent) &&
	    mt6797_fs_md3_private_dir(dir) && mt6797_fs_md3_private_dir(parent) &&
	    dir->d_name.len == 8 && !memcmp(dir->d_name.name, "md3-data", 8) &&
	    parent->d_name.len == sizeof("gemini-cellular") - 1 &&
	    !memcmp(parent->d_name.name, "gemini-cellular", sizeof("gemini-cellular") - 1))
		ret = 0;
	inode_unlock(d_inode(parent));
	inode_unlock(d_inode(top));
	if (!ret)
		ret = mt6797_fs_md3_data_statfs(file, &fs);
out:
	dput(top);
	dput(parent);
	return ret;
}
#endif
