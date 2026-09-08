// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in, boot-lifetime MD1 Z-drive authority: confined read-only metadata,
 * directory enumeration and file reads below one retained directory.
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
static struct file *provider_file;
static bool provider_volatile;
static struct device *provider_cold;
static struct mt6797_md1_fs_stats provider_stats;

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

int mt6797_md1_fs_provider_require(struct device *cold)
{
	unsigned long flags;
	int ret;

	if (!enable)
		return 0;
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_require(&provider_state);
	if (!ret && (provider_cold != cold || !mt6797_fs_accessible(provider_file)))
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
	ret = provider_cold == cold && mt6797_fs_accessible(provider_file) ?
		mt6797_fs_claim(&provider_state) : -EPERM;
	spin_unlock_irqrestore(&provider_lock, flags);
	return ret;
}

int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats)
{
	unsigned long flags;
	int ret = -ENODEV;

	spin_lock_irqsave(&provider_lock, flags);
	if (provider_state.stage == MT6797_FS_CONSUMED &&
	    mt6797_fs_accessible(provider_file)) {
		*stats = provider_stats;
		ret = 0;
	}
	spin_unlock_irqrestore(&provider_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_stats);

/* The retained, consumed, still read-only root; NULL if none is owned. */
static struct file *mt6797_fs_root_get(void)
{
	struct file *file = NULL;
	unsigned long flags;

	spin_lock_irqsave(&provider_lock, flags);
	if (provider_state.stage == MT6797_FS_CONSUMED &&
	    mt6797_fs_accessible(provider_file))
		file = get_file(provider_file);
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

int mt6797_md1_fs_provider_getattr(const char *path,
				   struct mt6797_md1_fs_attr *attr)
{
	struct dentry *dentry;
	struct file *root;
	int ret;

	might_sleep();
	if (!attr)
		return -EINVAL;
	root = mt6797_fs_root_get();
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
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_getattr);

struct file *mt6797_md1_fs_provider_open(const char *path, bool directory)
{
	struct dentry *dentry;
	struct file *root, *file;
	struct path target;
	int flags = O_RDONLY | O_LARGEFILE;

	might_sleep();
	root = mt6797_fs_root_get();
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
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_open);

/* Only a retained tmpfs root can reach creation or truncation. Parent lookup
 * never follows symlinks or mountpoints; final lookup uses that exact mount.
 * O_TRUNC is deliberately not passed to name lookup or dentry_open.
 */
struct file *mt6797_md1_fs_provider_open_ram(const char *path, bool writable,
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
	root = mt6797_fs_root_get();
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
	struct file *file = NULL;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_cancel(&provider_state);
	if (!ret) {
		file = provider_file;
		cold = provider_cold;
		provider_file = NULL;
		provider_cold = NULL;
	}
	spin_unlock_irqrestore(&provider_lock, flags);
	if (file)
		fput(file);
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

static int mt6797_fs_register_fd(int fd, bool ram)
{
	struct mt6797_md1_fs_stats stats;
	struct device *cold = NULL;
	struct file *file = NULL;
	struct kstatfs fs;
	unsigned long flags;
	int ret;

	if (ram && !enable_volatile)
		return -EPERM;
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_begin(&provider_state);
	spin_unlock_irqrestore(&provider_lock, flags);
	if (ret)
		return ret;
	file = fget(fd);
	ret = -EBADF;
	if (!file)
		goto fail;
	ret = -ENOTDIR;
	if (!d_is_dir(file->f_path.dentry))
		goto fail;
	ret = -EROFS;
	if (!mt6797_fs_backing_matches(file, ram) || (file->f_mode & FMODE_WRITE))
		goto fail;
	/* Local ext4 only: no unbounded network statfs or arbitrary backing type. */
	ret = -EINVAL;
	if (file_inode(file)->i_sb->s_magic != (ram ? TMPFS_MAGIC : EXT4_SUPER_MAGIC))
		goto fail;
	ret = vfs_statfs(&file->f_path, &fs);
	if (ret)
		goto fail;
	ret = -EINVAL;
	if (fs.f_type != (ram ? TMPFS_MAGIC : EXT4_SUPER_MAGIC))
		goto fail;
	/* Require finite tmpfs limits: at most 64 MiB and 4096 inodes. */
	if (ram && (fs.f_bsize <= 0 || !fs.f_blocks ||
		    fs.f_blocks > (64ULL * 1024 * 1024) / fs.f_bsize ||
		    !fs.f_files || fs.f_files > 4096)) {
		ret = -EFBIG;
		goto fail;
	}
	ret = mt6797_fs_stats_convert(&fs, &stats);
	if (ret)
		goto fail;
	cold = mt6797_fs_find_cold();
	if (IS_ERR(cold)) {
		ret = PTR_ERR(cold);
		cold = NULL;
		goto fail;
	}
	/* Keep a separate local target reference across cancellation/reprobe. */
	get_device(cold);
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_backing_matches(file, ram) ? mt6797_fs_publish(&provider_state) : -EROFS;
	if (!ret) {
		provider_volatile = ram;
		provider_file = file;
		provider_cold = cold;
		provider_stats = stats;
		file = NULL;
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
	if (file)
		fput(file);
	put_device(cold);
	mt6797_fs_cancel_registration();
	return ret;
}

static long mt6797_fs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mt6797_md1_fs_registration registration;

	if (!enable || !ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd == MT6797_MD1_FS_CANCEL)
		return mt6797_fs_cancel_registration();
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
