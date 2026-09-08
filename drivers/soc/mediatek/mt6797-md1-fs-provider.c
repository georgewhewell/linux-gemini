// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in, boot-lifetime MD1 filesystem authority. Legacy Z is read-only
 * ext4 or bounded tmpfs; atomic Z/X/Y mode retains three distinct tmpfs roots.
 */
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/statfs.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/soc/mediatek/mt6797-md1-fs-provider.h>
#include <uapi/linux/mt6797_md1_fs.h>
#include "mt6797-md1-fs-state.h"

static bool enable;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Require explicit read-only MD1 filesystem authority before cold boot");

static bool enable_volatile;
module_param(enable_volatile, bool, 0400);
MODULE_PARM_DESC(enable_volatile, "Allow experimental bounded writable tmpfs backing only");

static DEFINE_SPINLOCK(provider_lock);
static struct mt6797_fs_state provider_state;
static struct file *provider_file; /* Z, including legacy registration */
static struct file *provider_extra[2]; /* X, Y; NULL in legacy mode */
static bool provider_volatile;
static struct device *provider_cold;

bool mt6797_md1_fs_provider_enabled(void)
{
	return READ_ONCE(enable);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_enabled);

/* No VFS callbacks: safe under the IRQ-safe state lock and from NAPI. */
static bool mt6797_fs_readonly(struct file *file)
{
	return file && (READ_ONCE(file->f_path.mnt->mnt_flags) & MNT_READONLY) &&
		(READ_ONCE(file_inode(file)->i_sb->s_flags) & SB_RDONLY);
}

/* RAM policy is separate from readonly(), so no existing disk write refusal
 * is weakened. These checks have no VFS callbacks and may run under lock.
 */
static bool mt6797_fs_tmpfs_writable(struct file *file)
{
	return file && file_inode(file)->i_sb->s_magic == TMPFS_MAGIC &&
		!(READ_ONCE(file->f_path.mnt->mnt_flags) & MNT_READONLY) &&
		!(READ_ONCE(file_inode(file)->i_sb->s_flags) & SB_RDONLY);
}

static bool mt6797_fs_backing_matches(struct file *file, bool ram)
{
	return ram ? mt6797_fs_tmpfs_writable(file) : mt6797_fs_readonly(file);
}

static bool mt6797_fs_accessible(struct file *file)
{
	return mt6797_fs_backing_matches(file, READ_ONCE(provider_volatile));
}

/* Called under provider_lock. Three-root mode remains all-or-nothing even
 * when a backing mount changes before cold ownership is consumed.
 */
static bool mt6797_fs_roots_accessible(void)
{
	int i;

	if (!mt6797_fs_accessible(provider_file))
		return false;
	for (i = 0; i < 2; i++)
		if (provider_extra[i] && !mt6797_fs_accessible(provider_extra[i]))
			return false;
	return true;
}

static struct file *mt6797_fs_drive_file(u8 drive)
{
	switch (drive) {
	case 'Z':
		return provider_file;
	case 'X':
		return provider_extra[0];
	case 'Y':
		return provider_extra[1];
	default:
		return NULL;
	}
}

int mt6797_md1_fs_provider_require(struct device *cold)
{
	unsigned long flags;
	int ret;

	if (!enable)
		return 0;
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_require(&provider_state);
	if (!ret && (provider_cold != cold || !mt6797_fs_roots_accessible()))
		ret = -EPERM;
	spin_unlock_irqrestore(&provider_lock, flags);
	return ret;
}

int mt6797_md1_fs_provider_claim(struct device *cold)
{
	unsigned long flags;
	int ret;

	if (!enable)
		return 0;
	spin_lock_irqsave(&provider_lock, flags);
	ret = provider_cold == cold && mt6797_fs_roots_accessible() ?
		mt6797_fs_claim(&provider_state) : -EPERM;
	spin_unlock_irqrestore(&provider_lock, flags);
	return ret;
}

/* A retained, consumed root with its backing policy intact; NULL if absent. */
static struct file *mt6797_fs_root_get(u8 drive)
{
	struct file *file = NULL;
	unsigned long flags;

	spin_lock_irqsave(&provider_lock, flags);
	if (provider_state.stage == MT6797_FS_CONSUMED &&
	    mt6797_fs_accessible(mt6797_fs_drive_file(drive)))
		file = get_file(mt6797_fs_drive_file(drive));
	spin_unlock_irqrestore(&provider_lock, flags);
	return file;
}

/* One component below @base. lookup_one never traverses a mount or follows a
 * symlink; covered mountpoints, special inodes and other superblocks are
 * refused rather than resolved.
 */
static struct dentry *mt6797_fs_walk_component(const struct file *root,
					       struct dentry *base,
					       const char *name, size_t len)
{
	struct qstr qname = QSTR_INIT(name, len);
	struct dentry *child;

	if (!d_is_dir(base))
		return ERR_PTR(-ENOTDIR);
	child = lookup_one_positive_unlocked(mnt_idmap(root->f_path.mnt),
					     &qname, base);
	if (IS_ERR(child))
		return child;
	if (d_mountpoint(child) || d_is_symlink(child) ||
	    (!d_is_reg(child) && !d_is_dir(child)) ||
	    child->d_sb != file_inode(root)->i_sb) {
		dput(child);
		return ERR_PTR(-EACCES);
	}
	return child;
}

/* @path is slash separated and relative to the root; "" is the root itself.
 * Absolute paths, trailing separators, empty, dot and dot-dot components are
 * refused before any lookup.
 * Returns a referenced dentry on the root's superblock.
 */
static struct dentry *mt6797_fs_walk(const struct file *root, const char *path)
{
	struct dentry *base, *child;
	const char *end;
	size_t len;

	if (!path || strnlen(path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	base = dget(root->f_path.dentry);
	while (*path) {
		end = strchrnul(path, '/');
		len = end - path;
		if (!len || len > NAME_MAX || (*end && !end[1]) ||
		    (len <= 2 && !strncmp(path, "..", len))) {
			dput(base);
			return ERR_PTR(-EINVAL);
		}
		child = mt6797_fs_walk_component(root, base, path, len);
		dput(base);
		if (IS_ERR(child))
			return child;
		base = child;
		path = *end ? end + 1 : end;
	}
	return base;
}

static int mt6797_fs_stat(const struct file *root, struct dentry *dentry,
			  struct mt6797_md1_fs_attr *attr)
{
	struct path path = { .mnt = root->f_path.mnt, .dentry = dentry };
	struct kstat stat;
	int ret;

	ret = vfs_getattr(&path, &stat, STATX_MODE | STATX_SIZE | STATX_ATIME |
			  STATX_MTIME, AT_STATX_DONT_SYNC);
	if (ret)
		return ret;
	if (!mt6797_fs_accessible((struct file *)root))
		return -EACCES;
	attr->mode = stat.mode;
	attr->size = stat.size;
	attr->atime = stat.atime.tv_sec;
	attr->mtime = stat.mtime.tv_sec;
	return 0;
}

int mt6797_md1_fs_provider_getattr_drive(u8 drive, const char *path,
				   struct mt6797_md1_fs_attr *attr)
{
	struct dentry *dentry;
	struct file *root;
	int ret;

	might_sleep();
	if (!attr)
		return -EINVAL;
	root = mt6797_fs_root_get(drive);
	if (!root)
		return -ENODEV;
	dentry = mt6797_fs_walk(root, path);
	if (IS_ERR(dentry)) {
		ret = PTR_ERR(dentry);
		goto put_root;
	}
	ret = mt6797_fs_stat(root, dentry, attr);
	dput(dentry);
put_root:
	fput(root);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_getattr_drive);

int mt6797_md1_fs_provider_getattr(const char *path, struct mt6797_md1_fs_attr *attr)
{
	return mt6797_md1_fs_provider_getattr_drive('Z', path, attr);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_getattr);

struct file *mt6797_md1_fs_provider_open_drive(u8 drive, const char *path, bool directory)
{
	struct dentry *dentry;
	struct file *root, *file;
	struct path target;
	int flags = O_RDONLY | O_LARGEFILE;

	might_sleep();
	root = mt6797_fs_root_get(drive);
	if (!root)
		return ERR_PTR(-ENODEV);
	dentry = mt6797_fs_walk(root, path);
	if (IS_ERR(dentry)) {
		file = ERR_CAST(dentry);
		goto put_root;
	}
	if (directory != d_is_dir(dentry)) {
		file = ERR_PTR(directory ? -ENOTDIR : -EISDIR);
		goto put_dentry;
	}
	if (directory)
		flags |= O_DIRECTORY;
	target.mnt = root->f_path.mnt;
	target.dentry = dentry;
	/* The new file takes its own mount and dentry references. */
	file = dentry_open(&target, flags, current_cred());
	if (!IS_ERR(file) && !mt6797_fs_accessible(file)) {
		fput(file);
		file = ERR_PTR(-EACCES);
	}
put_dentry:
	dput(dentry);
put_root:
	fput(root);
	return file;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_open_drive);

struct file *mt6797_md1_fs_provider_open(const char *path, bool directory)
{
	return mt6797_md1_fs_provider_open_drive('Z', path, directory);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_open);

/* Only a retained tmpfs root can reach creation or truncation. Parent lookup
 * never follows symlinks or mountpoints; final lookup uses that exact mount.
 * O_TRUNC is deliberately not passed to name lookup or dentry_open.
 */
struct file *mt6797_md1_fs_provider_open_ram_drive(u8 drive, const char *path, bool writable,
					  bool create, bool truncate)
{
	struct file *root, *file;
	struct dentry *parent, *child;
	struct path target;
	struct qstr name;
	char *copy, *leaf, *slash;
	int ret;

	might_sleep();
	if (!path || strnlen(path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	root = mt6797_fs_root_get(drive);
	if (!root)
		return ERR_PTR(-ENODEV);
	file = ERR_PTR(-EROFS);
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(root))
		goto put_root;
	copy = kstrdup(path, GFP_KERNEL);
	if (!copy) {
		file = ERR_PTR(-ENOMEM);
		goto put_root;
	}
	slash = strrchr(copy, '/');
	leaf = slash ? slash + 1 : copy;
	name = (struct qstr)QSTR_INIT(leaf, strlen(leaf));
	if (!name.len || name.len > NAME_MAX || path[0] == '/' ||
	    (name.len <= 2 && !strncmp(leaf, "..", name.len))) {
		file = ERR_PTR(-EINVAL);
		goto free_copy;
	}
	if (slash)
		*slash = '\0';
	parent = mt6797_fs_walk(root, slash ? copy : "");
	if (IS_ERR(parent)) {
		file = ERR_CAST(parent);
		goto free_copy;
	}
	if (!d_is_dir(parent)) {
		file = ERR_PTR(-ENOTDIR);
		goto put_parent;
	}
	ret = mnt_want_write(root->f_path.mnt);
	if (ret) {
		file = ERR_PTR(ret);
		goto put_parent;
	}
	inode_lock(d_inode(parent));
	child = lookup_one(mnt_idmap(root->f_path.mnt), &name, parent);
	if (IS_ERR(child)) {
		file = ERR_CAST(child);
		goto unlock;
	}
	file = ERR_PTR(-EACCES);
	if (!mt6797_fs_tmpfs_writable(root) || d_mountpoint(child) ||
	    d_is_symlink(child) || child->d_sb != file_inode(root)->i_sb)
		goto put_child;
	if (d_is_negative(child)) {
		file = ERR_PTR(-ENOENT);
		if (!create)
			goto put_child;
		ret = vfs_create(mnt_idmap(root->f_path.mnt), child, 0600, NULL);
		if (ret) {
			file = ERR_PTR(ret);
			goto put_child;
		}
	}
	file = ERR_PTR(-EISDIR);
	if (!d_is_reg(child))
		goto put_child;
	target.mnt = root->f_path.mnt;
	target.dentry = child;
	file = dentry_open(&target, (writable ? O_RDWR : O_RDONLY) | O_LARGEFILE,
			    current_cred());
	/* Retain child across truncate, which takes its own inode locks. */
	inode_unlock(d_inode(parent));
	if (!IS_ERR(file) && truncate) {
		ret = vfs_truncate(&target, 0);
		if (ret) {
			fput(file);
			file = ERR_PTR(ret);
		}
	}
	dput(child);
	goto drop_write;
put_child:
	dput(child);
unlock:
	inode_unlock(d_inode(parent));
drop_write:
	mnt_drop_write(root->f_path.mnt);
put_parent:
	dput(parent);
free_copy:
	kfree(copy);
put_root:
	fput(root);
	return file;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_open_ram_drive);

/* Namespace operations never target the root itself and never recurse. Keep
 * the parent locked from final-component lookup through the VFS operation.
 * Legacy read-only authorities cannot reach any mutation callback.
 */
int mt6797_md1_fs_provider_mutate_drive(u8 drive, const char *path,
				      enum mt6797_md1_fs_mutation operation)
{
	struct file *root;
	struct dentry *parent, *child;
	struct qstr name;
	char *copy, *leaf, *slash;
	int ret;

	might_sleep();
	if (operation != MT6797_MD1_FS_MKDIR &&
	    operation != MT6797_MD1_FS_RMDIR &&
	    operation != MT6797_MD1_FS_UNLINK)
		return -EINVAL;
	if (!path || strnlen(path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return -ENAMETOOLONG;
	root = mt6797_fs_root_get(drive);
	if (!root)
		return -ENODEV;
	ret = -EROFS;
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(root))
		goto put_root;
	copy = kstrdup(path, GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto put_root;
	}
	slash = strrchr(copy, '/');
	leaf = slash ? slash + 1 : copy;
	name = (struct qstr)QSTR_INIT(leaf, strlen(leaf));
	ret = -EINVAL;
	if (!name.len || name.len > NAME_MAX || path[0] == '/' ||
	    (name.len <= 2 && !strncmp(leaf, "..", name.len)))
		goto free_copy;
	if (slash)
		*slash = '\0';
	parent = mt6797_fs_walk(root, slash ? copy : "");
	if (IS_ERR(parent)) {
		ret = PTR_ERR(parent);
		goto free_copy;
	}
	ret = -ENOTDIR;
	if (!d_is_dir(parent))
		goto put_parent;
	ret = mnt_want_write(root->f_path.mnt);
	if (ret)
		goto put_parent;
	inode_lock(d_inode(parent));
	child = lookup_one(mnt_idmap(root->f_path.mnt), &name, parent);
	if (IS_ERR(child)) {
		ret = PTR_ERR(child);
		goto unlock;
	}
	ret = -EACCES;
	if (!mt6797_fs_tmpfs_writable(root) || d_mountpoint(child) ||
	    d_is_symlink(child) || child->d_sb != file_inode(root)->i_sb)
		goto put_child;
	if (operation == MT6797_MD1_FS_MKDIR) {
		ret = -EEXIST;
		if (!d_is_negative(child))
			goto put_child;
		/* This kernel's vfs_mkdir consumes the input dentry even on
		 * error; only its successful returned reference is ours.
		 */
		child = vfs_mkdir(mnt_idmap(root->f_path.mnt), d_inode(parent),
				  child, 0770, NULL);
		if (IS_ERR(child)) {
			ret = PTR_ERR(child);
			goto unlock;
		}
		ret = 0;
	} else if (d_is_negative(child)) {
		ret = -ENOENT;
	} else if (operation == MT6797_MD1_FS_RMDIR) {
		ret = d_is_dir(child) ?
			vfs_rmdir(mnt_idmap(root->f_path.mnt), d_inode(parent), child, NULL) :
			-ENOTDIR;
	} else {
		ret = d_is_reg(child) ?
			vfs_unlink(mnt_idmap(root->f_path.mnt), d_inode(parent), child, NULL) :
			d_is_dir(child) ? -EISDIR : -EACCES;
	}
put_child:
	dput(child);
unlock:
	inode_unlock(d_inode(parent));
	mnt_drop_write(root->f_path.mnt);
put_parent:
	dput(parent);
free_copy:
	kfree(copy);
put_root:
	fput(root);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_mutate_drive);

/* Split a relative non-root path without following its final component.
 * The caller owns @copy for as long as the returned qstr is used.
 */
static struct dentry *mt6797_fs_rename_parent(struct file *root, char *copy,
					    struct qstr *name)
{
	char *slash = strrchr(copy, '/');
	char *leaf = slash ? slash + 1 : copy;

	*name = (struct qstr)QSTR_INIT(leaf, strlen(leaf));
	if (!name->len || name->len > NAME_MAX || copy[0] == '/' ||
	    (name->len <= 2 && !strncmp(leaf, "..", name->len)))
		return ERR_PTR(-EINVAL);
	if (slash)
		*slash = '\0';
	return mt6797_fs_walk(root, slash ? copy : "");
}

int mt6797_md1_fs_provider_rename_drive(u8 old_drive, const char *old_path,
				      u8 new_drive, const char *new_path)
{
	struct file *old_root, *new_root;
	struct renamedata rd = {};
	struct qstr old_name, new_name;
	char *old_copy, *new_copy;
	int ret;

	might_sleep();
	if (!old_path || !new_path ||
	    strnlen(old_path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX ||
	    strnlen(new_path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return -ENAMETOOLONG;
	old_root = mt6797_fs_root_get(old_drive);
	if (!old_root)
		return -ENODEV;
	new_root = mt6797_fs_root_get(new_drive);
	ret = -ENODEV;
	if (!new_root)
		goto put_old_root;
	ret = -EROFS;
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(old_root) ||
	    !mt6797_fs_tmpfs_writable(new_root))
		goto put_new_root;
	ret = -EXDEV;
	if (old_root->f_path.mnt != new_root->f_path.mnt)
		goto put_new_root;
	old_copy = kstrdup(old_path, GFP_KERNEL);
	ret = -ENOMEM;
	if (!old_copy)
		goto put_new_root;
	new_copy = kstrdup(new_path, GFP_KERNEL);
	if (!new_copy)
		goto free_old;
	rd.old_parent = mt6797_fs_rename_parent(old_root, old_copy, &old_name);
	if (IS_ERR(rd.old_parent)) {
		ret = PTR_ERR(rd.old_parent);
		goto free_new;
	}
	rd.new_parent = mt6797_fs_rename_parent(new_root, new_copy, &new_name);
	if (IS_ERR(rd.new_parent)) {
		ret = PTR_ERR(rd.new_parent);
		goto put_old_parent;
	}
	ret = -ENOTDIR;
	if (!d_is_dir(rd.old_parent) || !d_is_dir(rd.new_parent))
		goto put_new_parent;
	ret = mnt_want_write(old_root->f_path.mnt);
	if (ret)
		goto put_new_parent;
	rd.mnt_idmap = mnt_idmap(old_root->f_path.mnt);
	/* Native rename semantics: replacement allowed, ancestor traps handled
	 * by start_renaming. No effects until both final dentries are checked.
	 */
	ret = start_renaming(&rd, LOOKUP_NO_SYMLINKS, &old_name, &new_name);
	if (ret)
		goto drop_write;
	ret = -EACCES;
	if (!mt6797_fs_tmpfs_writable(old_root) || !mt6797_fs_tmpfs_writable(new_root) ||
	    d_mountpoint(rd.old_dentry) || d_mountpoint(rd.new_dentry) ||
	    rd.old_dentry->d_sb != file_inode(old_root)->i_sb ||
	    rd.new_dentry->d_sb != file_inode(new_root)->i_sb)
		goto end_rename;
	ret = -ENOENT;
	if (d_is_negative(rd.old_dentry))
		goto end_rename;
	ret = -EACCES;
	if ((!d_is_reg(rd.old_dentry) && !d_is_dir(rd.old_dentry)) ||
	    (!d_is_negative(rd.new_dentry) &&
	     !d_is_reg(rd.new_dentry) && !d_is_dir(rd.new_dentry)))
		goto end_rename;
	ret = vfs_rename(&rd);
end_rename:
	end_renaming(&rd);
drop_write:
	mnt_drop_write(old_root->f_path.mnt);
put_new_parent:
	dput(rd.new_parent);
put_old_parent:
	dput(rd.old_parent);
free_new:
	kfree(new_copy);
free_old:
	kfree(old_copy);
put_new_root:
	fput(new_root);
put_old_root:
	fput(old_root);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_rename_drive);

struct file *mt6797_md1_fs_provider_open_ram(const char *path, bool writable, bool create, bool truncate)
{
	return mt6797_md1_fs_provider_open_ram_drive('Z', path, writable, create, truncate);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_open_ram);

ssize_t mt6797_md1_fs_provider_write(struct file *file, const void *buf,
				     size_t count, loff_t *pos)
{
	might_sleep();
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(file) ||
	    !(file->f_mode & FMODE_WRITE))
		return -EROFS;
	/* Per-file bound in addition to the registered tmpfs byte/inode quota. */
	if (!pos || *pos < 0 || *pos > 16 * 1024 * 1024 ||
	    count > 16ULL * 1024 * 1024 - (u64)*pos)
		return -EFBIG;
	return kernel_write(file, buf, count, pos);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_write);

int mt6797_md1_fs_provider_fstat(struct file *file,
				 struct mt6797_md1_fs_attr *attr)
{
	might_sleep();
	if (!file || !attr || !mt6797_fs_accessible(file))
		return -EACCES;
	return mt6797_fs_stat(file, file->f_path.dentry, attr);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_fstat);

ssize_t mt6797_md1_fs_provider_read(struct file *file, void *buf,
				    size_t count, loff_t *pos)
{
	might_sleep();
	if (!file || !mt6797_fs_accessible(file))
		return -EACCES;
	return kernel_read(file, buf, count, pos);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_read);

/* Snapshot a bounded source before modifying the destination. This is a copy
 * primitive, not MOVE's vendor flag/recursive-overwrite policy. It never
 * unlinks the source, including when both names resolve to the same inode.
 */
int mt6797_md1_fs_provider_copy_drive(u8 old_drive, const char *old_path,
				    u8 new_drive, const char *new_path)
{
	struct mt6797_md1_fs_attr attr;
	struct file *source, *destination;
	void *buffer = NULL;
	loff_t pos = 0;
	size_t size, done;
	ssize_t count;
	int ret;

	might_sleep();
	if (!old_path || !new_path ||
	    strnlen(old_path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX ||
	    strnlen(new_path, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return -ENAMETOOLONG;
	if (old_drive == new_drive && !strcmp(old_path, new_path))
		return -EINVAL;
	source = mt6797_md1_fs_provider_open_ram_drive(old_drive, old_path,
						    false, false, false);
	if (IS_ERR(source))
		return PTR_ERR(source);
	ret = mt6797_md1_fs_provider_fstat(source, &attr);
	if (ret)
		goto put_source;
	ret = -EFBIG;
	if (attr.size > 16 * 1024 * 1024)
		goto put_source;
	size = attr.size;
	if (size) {
		buffer = kvmalloc(size, GFP_KERNEL);
		ret = -ENOMEM;
		if (!buffer)
			goto put_source;
	}
	for (done = 0; done < size; done += count) {
		count = mt6797_md1_fs_provider_read(source, buffer + done,
						 min_t(size_t, size - done, 65536), &pos);
		ret = count < 0 ? count : -EIO;
		if (count <= 0)
			goto free_buffer;
		cond_resched();
	}
	/* Avoid truncating an existing destination before same-inode checking.
	 * Path creation is confined by the existing RAM open implementation.
	 */
	destination = mt6797_md1_fs_provider_open_ram_drive(new_drive, new_path,
							 true, true, false);
	if (IS_ERR(destination)) {
		ret = PTR_ERR(destination);
		goto free_buffer;
	}
	ret = -EINVAL;
	if (file_inode(source) == file_inode(destination))
		goto put_destination;
	ret = -EROFS;
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(destination))
		goto put_destination;
	ret = vfs_truncate(&destination->f_path, 0);
	if (ret)
		goto put_destination;
	pos = 0;
	for (done = 0; done < size; done += count) {
		count = mt6797_md1_fs_provider_write(destination, buffer + done,
						  min_t(size_t, size - done, 65536), &pos);
		ret = count < 0 ? count : -EIO;
		if (count <= 0)
			goto put_destination;
		cond_resched();
	}
	ret = 0;
put_destination:
	fput(destination);
free_buffer:
	kvfree(buffer);
put_source:
	fput(source);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_copy_drive);

loff_t mt6797_md1_fs_provider_llseek(struct file *file, loff_t offset,
				     int whence)
{
	might_sleep();
	if (!file || !mt6797_fs_accessible(file))
		return -EACCES;
	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
		return -EINVAL;
	return vfs_llseek(file, offset, whence);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_llseek);

struct mt6797_fs_dir_walk {
	struct dir_context ctx;
	struct mt6797_md1_fs_entry *entry;
	bool found;
};

static bool mt6797_fs_dir_actor(struct dir_context *ctx, const char *name,
				int len, loff_t pos, u64 ino, unsigned int type)
{
	struct mt6797_fs_dir_walk *walk =
		container_of(ctx, struct mt6797_fs_dir_walk, ctx);

	/* A refused emission is re-presented by the next iterate_dir; accept
	 * the wanted entry so the position moves past it, refuse the one after.
	 */
	if (walk->found)
		return false;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.') ||
	    len < 1 || len > NAME_MAX)
		return true;
	memcpy(walk->entry->name, name, len);
	walk->entry->name[len] = 0;
	walk->found = true;
	return true;
}

/* Next regular file or directory entry of an open directory, with its
 * metadata. Symlinks, special inodes and covered mountpoints are skipped.
 * Returns -ENOENT once the directory is exhausted.
 */
int mt6797_md1_fs_provider_next_entry(struct file *dir,
				      struct mt6797_md1_fs_entry *entry)
{
	struct mt6797_fs_dir_walk walk = {
		.ctx.actor = mt6797_fs_dir_actor,
		.entry = entry,
	};
	struct dentry *child;
	int ret;

	might_sleep();
	if (!dir || !entry)
		return -EINVAL;
	for (;;) {
		if (!mt6797_fs_accessible(dir))
			return -EACCES;
		walk.found = false;
		walk.ctx.pos = dir->f_pos;
		ret = iterate_dir(dir, &walk.ctx);
		if (ret)
			return ret;
		if (!walk.found)
			return -ENOENT;
		child = mt6797_fs_walk_component(dir, dir->f_path.dentry,
						 entry->name, strlen(entry->name));
		if (IS_ERR(child)) {
			if (PTR_ERR(child) == -ENOENT || PTR_ERR(child) == -EACCES) {
				cond_resched();
				continue;
			}
			return PTR_ERR(child);
		}
		ret = mt6797_fs_stat(dir, child, &entry->attr);
		dput(child);
		return ret;
	}
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_next_entry);

/* Iterative post-order traversal: bounded by the registered inode budget and
 * the maximum number of nonempty components in a provider path. Errors can
 * follow partial deletion; return the error, never an invented full count.
 */
int mt6797_md1_fs_provider_xdelete_drive(u8 drive, const char *name, u32 flags)
{
	struct delete_frame {
		struct file *dir;
		unsigned int length;
	};
	const unsigned int frames = MT6797_MD1_FS_PATH_MAX / 2 + 1;
	struct delete_frame *stack;
	struct mt6797_md1_fs_entry *entry;
	struct file *root;
	char *path;
	unsigned int depth = 0, visited = 0, length, base;
	int ret, deleted = 0;

	might_sleep();
	if (!flags || (flags & ~0x1cU))
		return -EINVAL;
	if (!name || strnlen(name, MT6797_MD1_FS_PATH_MAX + 1) > MT6797_MD1_FS_PATH_MAX)
		return -ENAMETOOLONG;
	/* Contents of a RAM root may be selected; the retained root itself may
	 * never be removed, even after its children have been deleted.
	 */
	if (!*name && (flags & 0x18) == 0x18)
		return -EINVAL;
	root = mt6797_fs_root_get(drive);
	if (!root)
		return -ENODEV;
	ret = -EROFS;
	if (!READ_ONCE(provider_volatile) || !mt6797_fs_tmpfs_writable(root)) {
		fput(root);
		return ret;
	}
	fput(root);
	stack = kcalloc(frames, sizeof(*stack), GFP_KERNEL);
	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	path = kmalloc(MT6797_MD1_FS_PATH_MAX + 1, GFP_KERNEL);
	ret = -ENOMEM;
	if (!stack || !entry || !path)
		goto free_memory;
	strcpy(path, name);
	stack[0].length = strlen(path);
	stack[0].dir = mt6797_md1_fs_provider_open_drive(drive, path, true);
	if (IS_ERR(stack[0].dir)) {
		ret = PTR_ERR(stack[0].dir);
		stack[0].dir = NULL;
		goto free_memory;
	}
	for (;;) {
		ret = mt6797_md1_fs_provider_next_entry(stack[depth].dir, entry);
		if (ret == -ENOENT) {
			fput(stack[depth].dir);
			stack[depth].dir = NULL;
			if ((flags & 8) && (depth || (flags & 16))) {
				ret = mt6797_md1_fs_provider_mutate_drive(drive, path,
								MT6797_MD1_FS_RMDIR);
				if (ret)
					goto close_dirs;
				deleted++;
			}
			if (!depth) {
				ret = deleted;
				goto close_dirs;
			}
			depth--;
			path[stack[depth].length] = '\0';
			continue;
		}
		if (ret)
			goto close_dirs;
		if (++visited > 4096) {
			ret = -E2BIG;
			goto close_dirs;
		}
		base = stack[depth].length;
		length = strlen(entry->name);
		if (base + !!base + length > MT6797_MD1_FS_PATH_MAX) {
			ret = -ENAMETOOLONG;
			goto close_dirs;
		}
		if (base)
			path[base] = '/';
		memcpy(path + base + !!base, entry->name, length + 1);
		if (S_ISDIR(entry->attr.mode) && (flags & 16)) {
			if (depth + 1 == frames) {
				ret = -E2BIG;
				goto close_dirs;
			}
			root = mt6797_md1_fs_provider_open_drive(drive, path, true);
			if (IS_ERR(root)) {
				ret = PTR_ERR(root);
				goto close_dirs;
			}
			depth++;
			stack[depth].dir = root;
			stack[depth].length = strlen(path);
		} else {
			if (S_ISDIR(entry->attr.mode) && (flags & 8))
				ret = mt6797_md1_fs_provider_mutate_drive(drive, path,
								MT6797_MD1_FS_RMDIR);
			else if (S_ISREG(entry->attr.mode) && (flags & 4))
				ret = mt6797_md1_fs_provider_mutate_drive(drive, path,
								MT6797_MD1_FS_UNLINK);
			else {
				path[base] = '\0';
				cond_resched();
				continue;
			}
			if (ret)
				goto close_dirs;
			deleted++;
			path[base] = '\0';
		}
		cond_resched();
	}
close_dirs:
	for (;;) {
		if (stack[depth].dir)
			fput(stack[depth].dir);
		if (!depth)
			break;
		depth--;
	}
free_memory:
	kfree(path);
	kfree(entry);
	kfree(stack);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_xdelete_drive);

void mt6797_md1_fs_provider_close(struct file *file)
{
	might_sleep();
	if (!IS_ERR_OR_NULL(file))
		fput(file);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_close);

static int mt6797_fs_cancel_registration(void)
{
	struct device *cold = NULL;
	struct file *file = NULL, *extra[2] = {};
	unsigned long flags;
	int i, ret;

	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_cancel(&provider_state);
	if (!ret) {
		file = provider_file;
		for (i = 0; i < 2; i++) {
			extra[i] = provider_extra[i];
			provider_extra[i] = NULL;
		}
		cold = provider_cold;
		provider_file = NULL;
		provider_cold = NULL;
	}
	spin_unlock_irqrestore(&provider_lock, flags);
	if (file)
		fput(file);
	for (i = 0; i < 2; i++)
		if (extra[i])
			fput(extra[i]);
	put_device(cold);
	return ret;
}

static struct device *mt6797_fs_find_cold(void)
{
	struct platform_device *pdev = NULL;
	struct device_node *node;

	for_each_compatible_node(node, NULL, "mediatek,mt6797-md1-cold-owner") {
		if (!of_device_is_available(node))
			continue;
		if (pdev) {
			put_device(&pdev->dev);
			of_node_put(node);
			return ERR_PTR(-EEXIST);
		}
		pdev = of_find_device_by_node(node);
		if (!pdev) {
			of_node_put(node);
			return ERR_PTR(-ENODEV);
		}
	}
	return pdev ? &pdev->dev : ERR_PTR(-ENODEV);
}

static int mt6797_fs_validate_root(struct file *file, bool ram,
				   struct mt6797_md1_fs_stats *stats)
{
	struct kstatfs fs;
	int ret;

	ret = -ENOTDIR;
	if (!d_is_dir(file->f_path.dentry))
		return ret;
	ret = -EROFS;
	if (!mt6797_fs_backing_matches(file, ram) || (file->f_mode & FMODE_WRITE))
		return ret;
	/* Local ext4 only: no unbounded network statfs or arbitrary backing type. */
	ret = -EINVAL;
	if (file_inode(file)->i_sb->s_magic != (ram ? TMPFS_MAGIC : EXT4_SUPER_MAGIC))
		return ret;
	ret = vfs_statfs(&file->f_path, &fs);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (fs.f_type != (ram ? TMPFS_MAGIC : EXT4_SUPER_MAGIC))
		return ret;
	/* Require finite tmpfs limits: at most 64 MiB and 4096 inodes. */
	if (ram && (fs.f_bsize <= 0 || !fs.f_blocks ||
		    fs.f_blocks > (64ULL * 1024 * 1024) / fs.f_bsize ||
		    !fs.f_files || fs.f_files > 4096)) {
		ret = -EFBIG;
		return ret;
	}
	ret = mt6797_fs_stats_convert(&fs, stats);
	if (ret)
		return ret;
	return 0;
}

/* Query live geometry in process context. Cached registration-time free
 * space becomes stale after RAM writes, truncation or namespace mutations.
 */
int mt6797_md1_fs_provider_stats_drive(u8 drive, struct mt6797_md1_fs_stats *stats)
{
	struct mt6797_md1_fs_stats snapshot;
	struct file *root;
	int ret;

	might_sleep();
	if (!stats)
		return -EINVAL;
	root = mt6797_fs_root_get(drive);
	if (!root)
		return -ENODEV;
	ret = mt6797_fs_validate_root(root, READ_ONCE(provider_volatile), &snapshot);
	/* A remount during statfs must not publish a result from an authority
	 * whose backing policy no longer holds.
	 */
	if (!ret && !mt6797_fs_accessible(root))
		ret = -EROFS;
	if (!ret)
		*stats = snapshot;
	fput(root);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_stats_drive);

int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats)
{
	return mt6797_md1_fs_provider_stats_drive('Z', stats);
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_stats);

static int mt6797_fs_register_fds(const int *fds, unsigned int count, bool ram)
{
	struct mt6797_md1_fs_stats stats[3];
	struct file *files[3] = {};
	struct device *cold = NULL;
	unsigned long flags;
	unsigned int i, j;
	int ret;

	if (count != 1 && (count != 3 || !ram))
		return -EINVAL;
	if (ram && !enable_volatile)
		return -EPERM;
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_begin(&provider_state);
	spin_unlock_irqrestore(&provider_lock, flags);
	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		files[i] = fget(fds[i]);
		ret = -EBADF;
		if (!files[i])
			goto fail;
		ret = mt6797_fs_validate_root(files[i], ram, &stats[i]);
		if (ret)
			goto fail;
		/* Separate superblocks prevent aliases and ancestor roots from
		 * sharing authority or exhausting another drive's quota.
		 */
		for (j = 0; j < i; j++) {
			ret = -EXDEV;
			if (file_inode(files[i])->i_sb == file_inode(files[j])->i_sb)
				goto fail;
		}
	}
	cold = mt6797_fs_find_cold();
	if (IS_ERR(cold)) {
		ret = PTR_ERR(cold);
		cold = NULL;
		goto fail;
	}
	/* Keep a separate local target reference across cancellation/reprobe. */
	get_device(cold);
	spin_lock_irqsave(&provider_lock, flags);
	ret = 0;
	for (i = 0; i < count; i++)
		if (!mt6797_fs_backing_matches(files[i], ram))
			ret = -EROFS;
	if (!ret)
		ret = mt6797_fs_publish(&provider_state);
	if (!ret) {
		provider_volatile = ram;
		provider_file = files[0];
		provider_cold = cold;
		for (i = 1; i < count; i++)
			provider_extra[i - 1] = files[i];
		for (i = 0; i < count; i++)
			files[i] = NULL;
	}
	spin_unlock_irqrestore(&provider_lock, flags);
	if (ret) {
		put_device(cold);
		goto fail;
	}
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_reprobe(&provider_state);
	spin_unlock_irqrestore(&provider_lock, flags);
	if (!ret) {
		/* Exact device only; its normal probe retains every independent gate. */
		ret = device_attach(cold);
		if (ret < 0)
			dev_warn(cold, "FS authority published; target attach returned %d\n", ret);
	}
	put_device(cold);
	/* Success reports publication, not GO. An ordinary deferred probe may
	 * already have consumed it, or a concurrent CANCEL may have revoked it.
	 * Neither case permits another attach attempt or registration here.
	 */
	return 0;
fail:
	for (i = 0; i < count; i++)
		if (files[i])
			fput(files[i]);
	put_device(cold);
	mt6797_fs_cancel_registration();
	return ret;
}

static int mt6797_fs_register_fd(int fd, bool ram)
{
	return mt6797_fs_register_fds(&fd, 1, ram);
}

static long mt6797_fs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mt6797_md1_fs_registration registration;
	struct mt6797_md1_fs_ram_roots roots;

	if (!enable || !ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd == MT6797_MD1_FS_CANCEL)
		return mt6797_fs_cancel_registration();
	if (cmd == MT6797_MD1_FS_REGISTER_RAM_ROOTS) {
		if (copy_from_user(&roots, (void __user *)arg, sizeof(roots)))
			return -EFAULT;
		if (roots.version != 1 || roots.flags || roots.reserved)
			return -EINVAL;
		return mt6797_fs_register_fds(roots.directory_fd, 3, true);
	}
	if (cmd != MT6797_MD1_FS_REGISTER && cmd != MT6797_MD1_FS_REGISTER_RAM)
		return -ENOTTY;
	if (copy_from_user(&registration, (void __user *)arg, sizeof(registration)))
		return -EFAULT;
	if (registration.version != 1 || registration.flags || registration.reserved)
		return -EINVAL;
	return mt6797_fs_register_fd(registration.directory_fd,
				    cmd == MT6797_MD1_FS_REGISTER_RAM);
}

static const struct file_operations mt6797_fs_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.unlocked_ioctl = mt6797_fs_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice mt6797_fs_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mt6797-md1-fs-provider",
	.fops = &mt6797_fs_fops,
	.mode = 0600,
};

static int __init mt6797_fs_init(void)
{
	return enable ? misc_register(&mt6797_fs_device) : 0;
}
device_initcall(mt6797_fs_init);
/* Built-in only. A consumed provider pins its authority until this boot ends. */

#if IS_ENABLED(CONFIG_MTK_MT6797_MD1_FS_VFS_KUNIT_TEST)
#include "mt6797-md1-fs-vfs-kunit.c"
#endif
