/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD1_FS_PROVIDER_H
#define _MT6797_MD1_FS_PROVIDER_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct file;
struct mt6797_md1_fs_stats {
	u32 block_size;
	u32 blocks;
	u32 free_blocks;
};

/* Slash separated path relative to the retained root; bounded by the vendor
 * daemon's own path limit.
 */
#define MT6797_MD1_FS_PATH_MAX 1024

struct mt6797_md1_fs_attr {
	unsigned int mode;
	u64 size;
	s64 atime;
	s64 mtime;
};

struct mt6797_md1_fs_entry {
	char name[256];
	struct mt6797_md1_fs_attr attr;
};

enum mt6797_md1_fs_mutation {
	MT6797_MD1_FS_MKDIR,
	MT6797_MD1_FS_RMDIR,
	MT6797_MD1_FS_UNLINK,
};

#if IS_ENABLED(CONFIG_MTK_MT6797_MD1_FS_PROVIDER)
/* Returns a nonnegative deletion count or negative errno. */
int mt6797_md1_fs_provider_xdelete_drive(u8 drive, const char *path, u32 flags);
/* Bounded RAM file copy; does not unlink source or process MOVE flags. */
int mt6797_md1_fs_provider_copy_drive(u8 old_drive, const char *old_path,
				    u8 new_drive, const char *new_path);
int mt6797_md1_fs_provider_rename_drive(u8 old_drive, const char *old_path,
				      u8 new_drive, const char *new_path);
/* Process-context namespace mutations, restricted to consumed RAM roots. */
int mt6797_md1_fs_provider_mutate_drive(u8 drive, const char *path,
				      enum mt6797_md1_fs_mutation operation);
/* Explicit ASCII drive; only registered Z/X/Y roots are addressable. Paths
 * remain relative and undergo the same confinement checks as the legacy API.
 */
/* Process context: live statfs, including current RAM free space. */
int mt6797_md1_fs_provider_stats_drive(u8 drive, struct mt6797_md1_fs_stats *stats);
int mt6797_md1_fs_provider_getattr_drive(u8 drive, const char *path,
				       struct mt6797_md1_fs_attr *attr);
struct file *mt6797_md1_fs_provider_open_drive(u8 drive, const char *path, bool directory);
struct file *mt6797_md1_fs_provider_open_ram_drive(u8 drive, const char *path,
					bool writable, bool create, bool truncate);
bool mt6797_md1_fs_provider_enabled(void);
int mt6797_md1_fs_provider_require(struct device *cold);
int mt6797_md1_fs_provider_claim(struct device *cold);
int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats);
/* Process context only. Every path is confined below the retained root:
 * no absolute, dot, dot-dot, symlink, mountpoint or foreign-superblock step.
 */
int mt6797_md1_fs_provider_getattr(const char *path,
				   struct mt6797_md1_fs_attr *attr);
struct file *mt6797_md1_fs_provider_open(const char *path, bool directory);
struct file *mt6797_md1_fs_provider_open_ram(const char *path, bool writable,
					  bool create, bool truncate);
ssize_t mt6797_md1_fs_provider_write(struct file *file, const void *buf,
				     size_t count, loff_t *pos);
int mt6797_md1_fs_provider_fstat(struct file *file,
				 struct mt6797_md1_fs_attr *attr);
ssize_t mt6797_md1_fs_provider_read(struct file *file, void *buf,
				    size_t count, loff_t *pos);
loff_t mt6797_md1_fs_provider_llseek(struct file *file, loff_t offset,
				     int whence);
int mt6797_md1_fs_provider_next_entry(struct file *dir,
				      struct mt6797_md1_fs_entry *entry);
void mt6797_md1_fs_provider_close(struct file *file);
#else
static inline int mt6797_md1_fs_provider_xdelete_drive(u8 drive, const char *path, u32 flags)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_copy_drive(u8 old_drive, const char *old_path,
				    u8 new_drive, const char *new_path)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_rename_drive(u8 old_drive, const char *old_path,
				      u8 new_drive, const char *new_path)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_mutate_drive(u8 drive, const char *path,
				      enum mt6797_md1_fs_mutation operation)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_stats_drive(u8 drive, struct mt6797_md1_fs_stats *stats)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_getattr_drive(u8 drive, const char *path,
				       struct mt6797_md1_fs_attr *attr)
{
	return -ENODEV;
}
static inline struct file *mt6797_md1_fs_provider_open_drive(u8 drive, const char *path, bool directory)
{
	return ERR_PTR(-ENODEV);
}
static inline struct file *mt6797_md1_fs_provider_open_ram_drive(u8 drive, const char *path,
					bool writable, bool create, bool truncate)
{
	return ERR_PTR(-ENODEV);
}
static inline bool mt6797_md1_fs_provider_enabled(void) { return false; }
static inline int mt6797_md1_fs_provider_require(struct device *cold) { return 0; }
static inline int mt6797_md1_fs_provider_claim(struct device *cold) { return 0; }
static inline int mt6797_md1_fs_provider_stats(struct mt6797_md1_fs_stats *stats)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_getattr(const char *path,
						 struct mt6797_md1_fs_attr *attr)
{
	return -ENODEV;
}
static inline struct file *mt6797_md1_fs_provider_open(const char *path,
						       bool directory)
{
	return ERR_PTR(-ENODEV);
}
static inline struct file *mt6797_md1_fs_provider_open_ram(const char *path,
				bool writable, bool create, bool truncate)
{
	return ERR_PTR(-ENODEV);
}
static inline ssize_t mt6797_md1_fs_provider_write(struct file *file,
				const void *buf, size_t count, loff_t *pos)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_fstat(struct file *file,
					       struct mt6797_md1_fs_attr *attr)
{
	return -ENODEV;
}
static inline ssize_t mt6797_md1_fs_provider_read(struct file *file, void *buf,
						  size_t count, loff_t *pos)
{
	return -ENODEV;
}
static inline loff_t mt6797_md1_fs_provider_llseek(struct file *file,
						   loff_t offset, int whence)
{
	return -ENODEV;
}
static inline int mt6797_md1_fs_provider_next_entry(struct file *dir,
						    struct mt6797_md1_fs_entry *entry)
{
	return -ENODEV;
}
static inline void mt6797_md1_fs_provider_close(struct file *file)
{
}
#endif
#endif
