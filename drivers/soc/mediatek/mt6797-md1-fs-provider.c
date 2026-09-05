// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in, boot-lifetime MD1 Z-drive metadata authority. Never reads file data. */
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
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

static DEFINE_SPINLOCK(provider_lock);
static struct mt6797_fs_state provider_state;
static struct file *provider_file;
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

int mt6797_md1_fs_provider_require(struct device *cold)
{
	unsigned long flags;
	int ret;

	if (!enable)
		return 0;
	spin_lock_irqsave(&provider_lock, flags);
	ret = mt6797_fs_require(&provider_state);
	if (!ret && (provider_cold != cold || !mt6797_fs_readonly(provider_file)))
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
	ret = provider_cold == cold && mt6797_fs_readonly(provider_file) ?
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
	    mt6797_fs_readonly(provider_file)) {
		*stats = provider_stats;
		ret = 0;
	}
	spin_unlock_irqrestore(&provider_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_stats);

int mt6797_md1_fs_provider_getattr(const char *name, unsigned int *mode)
{
	struct file *file = NULL;
	struct dentry *child;
	struct path path;
	struct kstat stat;
	struct qstr qname;
	unsigned long flags;
	size_t length;
	int ret;

	might_sleep();
	if (!name || !mode)
		return -EINVAL;
	length = strnlen(name, 256);
	if (!length || length == 256 || !strcmp(name, ".") ||
	    !strcmp(name, "..") || strpbrk(name, "/\\:"))
		return -EACCES;
	spin_lock_irqsave(&provider_lock, flags);
	if (provider_state.stage == MT6797_FS_CONSUMED &&
	    mt6797_fs_readonly(provider_file))
		file = get_file(provider_file);
	spin_unlock_irqrestore(&provider_lock, flags);
	if (!file)
		return -ENODEV;
	qname = (struct qstr)QSTR_INIT(name, length);
	child = lookup_one_positive_unlocked(mnt_idmap(file->f_path.mnt),
					    &qname, file->f_path.dentry);
	if (IS_ERR(child)) {
		ret = PTR_ERR(child);
		goto put_file;
	}
	/* lookup_one never traverses a mount or follows a symlink. Reject even
	 * covered mountpoints and non-file objects rather than stat their backing.
	 */
	if (d_mountpoint(child) || d_is_symlink(child) ||
	    (!d_is_reg(child) && !d_is_dir(child)) ||
	    child->d_sb != file_inode(file)->i_sb || !mt6797_fs_readonly(file)) {
		ret = -EACCES;
		goto put_child;
	}
	path.mnt = file->f_path.mnt;
	path.dentry = child;
	ret = vfs_getattr(&path, &stat, STATX_MODE, AT_STATX_DONT_SYNC);
	if (!ret && !mt6797_fs_readonly(file))
		ret = -EACCES;
	if (!ret)
		*mode = stat.mode;
put_child:
	dput(child);
put_file:
	fput(file);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_md1_fs_provider_getattr);

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

static int mt6797_fs_register_fd(int fd)
{
	struct mt6797_md1_fs_stats stats;
	struct device *cold = NULL;
	struct file *file = NULL;
	struct kstatfs fs;
	unsigned long flags;
	int ret;

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
	if (!mt6797_fs_readonly(file) || (file->f_mode & FMODE_WRITE))
		goto fail;
	/* Local ext4 only: no unbounded network statfs or arbitrary backing type. */
	ret = -EINVAL;
	if (file_inode(file)->i_sb->s_magic != EXT4_SUPER_MAGIC)
		goto fail;
	ret = vfs_statfs(&file->f_path, &fs);
	if (ret)
		goto fail;
	ret = -EINVAL;
	if (fs.f_type != EXT4_SUPER_MAGIC)
		goto fail;
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
	ret = mt6797_fs_readonly(file) ? mt6797_fs_publish(&provider_state) : -EROFS;
	if (!ret) {
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
	if (cmd != MT6797_MD1_FS_REGISTER)
		return -ENOTTY;
	if (copy_from_user(&registration, (void __user *)arg, sizeof(registration)))
		return -EFAULT;
	if (registration.version != 1 || registration.flags || registration.reserved)
		return -EINVAL;
	return mt6797_fs_register_fd(registration.directory_fd);
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
