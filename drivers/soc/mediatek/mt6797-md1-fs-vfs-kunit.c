// SPDX-License-Identifier: GPL-2.0-only
/* Included by the provider only in an explicitly selected UML test build.
 * Publish isolated test authority directly; cold-owner admission is covered
 * separately. All path, VFS and data operations below are production code.
 */
#include <kunit/test.h>
#include <linux/slab.h>

struct mt6797_fs_vfs_fixture {
	struct vfsmount *mounts[3];
	struct file *roots[3];
	struct file *files[3];
	bool published;
};

static void mt6797_fs_vfs_cleanup(void *data)
{
	struct mt6797_fs_vfs_fixture *f = data;
	unsigned long flags;
	int i;

	if (f->published) {
		spin_lock_irqsave(&provider_lock, flags);
		provider_file = NULL;
		provider_extra[0] = provider_extra[1] = NULL;
		provider_state = (struct mt6797_fs_state){};
		provider_volatile = false;
		spin_unlock_irqrestore(&provider_lock, flags);
	}
	for (i = 0; i < 3; i++) {
		if (!IS_ERR_OR_NULL(f->files[i]))
			fput(f->files[i]);
		if (!IS_ERR_OR_NULL(f->roots[i]))
			fput(f->roots[i]);
		if (!IS_ERR_OR_NULL(f->mounts[i]))
			kern_unmount(f->mounts[i]);
	}
}

static void mt6797_fs_vfs_operations(struct kunit *test)
{
	struct mt6797_fs_vfs_fixture *f;
	struct file_system_type *type;
	struct mt6797_md1_fs_stats before, written, unlinked, closed;
	struct mt6797_md1_fs_attr attr;
	struct path path;
	char options[] = "size=4m,nr_inodes=128";
	char *page, result[8] = {};
	unsigned long flags;
	loff_t pos;
	int i;

	KUNIT_ASSERT_FALSE(test, enable);
	KUNIT_ASSERT_PTR_EQ(test, provider_file, NULL);
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mt6797_fs_vfs_cleanup, f), 0);
	for (i = 0; i < 3; i++) {
		strscpy(options, "size=4m,nr_inodes=128");
		type = get_fs_type("tmpfs");
		KUNIT_ASSERT_NOT_NULL(test, type);
		f->mounts[i] = vfs_kern_mount(type, SB_KERNMOUNT, "md1-vfs-test", options);
		put_filesystem(type);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->mounts[i]));
		path.mnt = f->mounts[i];
		path.dentry = f->mounts[i]->mnt_root;
		f->roots[i] = dentry_open(&path, O_RDONLY | O_DIRECTORY, current_cred());
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->roots[i]));
		KUNIT_ASSERT_EQ(test, mt6797_fs_validate_root(f->roots[i], true, &before), 0);
	}
	spin_lock_irqsave(&provider_lock, flags);
	provider_file = f->roots[0];
	provider_extra[0] = f->roots[1];
	provider_extra[1] = f->roots[2];
	provider_volatile = true;
	provider_state.stage = MT6797_FS_CONSUMED;
	f->published = true;
	spin_unlock_irqrestore(&provider_lock, flags);

	for (i = 0; i < 3; i++) {
		u8 drive = "ZXY"[i];

		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_mutate_drive(drive, "dir", MT6797_MD1_FS_MKDIR), 0);
		KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_mutate_drive(drive, "dir", MT6797_MD1_FS_MKDIR), -EEXIST);
		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_getattr_drive(drive, "dir", &attr), 0);
		KUNIT_EXPECT_TRUE(test, S_ISDIR(attr.mode));
		f->files[i] = mt6797_md1_fs_provider_open_ram_drive(drive, "dir/file", true, true, false);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->files[i]));
		pos = 0;
		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_write(f->files[i], &drive, 1, &pos), (ssize_t)1);
		KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_mutate_drive(drive, "dir", MT6797_MD1_FS_RMDIR), -ENOTEMPTY);
		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_rename_drive(drive, "dir/file", drive, "dir/moved"), 0);
		KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_getattr_drive(drive, "dir/file", &attr), -ENOENT);
		pos = 0;
		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_read(f->files[i], result, 1, &pos), (ssize_t)1);
		KUNIT_EXPECT_EQ(test, result[0], (char)drive);
	}
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_rename_drive('Z', "dir/moved", 'X', "dir/cross"), -EXDEV);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_rename_drive('Z', "dir", 'Z', "dir/inside"), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_rename_drive('Z', "dir/moved", 'Z', "dir/moved"), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_mutate_drive('Z', "../escape", MT6797_MD1_FS_MKDIR), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_mutate_drive('Z', "", MT6797_MD1_FS_RMDIR), -EINVAL);

	/* Cross-root copy preserves source and replaces destination contents. */
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_copy_drive('Z', "dir/moved", 'Y', "copied"), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_getattr_drive('Z', "dir/moved", &attr), 0);
	KUNIT_EXPECT_EQ(test, attr.size, (u64)1);
	fput(f->files[2]);
	f->files[2] = mt6797_md1_fs_provider_open_drive('Y', "copied", false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->files[2]));
	pos = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_read(f->files[2], result, 1, &pos), (ssize_t)1);
	KUNIT_EXPECT_EQ(test, result[0], 'Z');
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_copy_drive('Z', "dir/moved", 'X', "dir/moved"), 0);
	pos = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_read(f->files[1], result, 1, &pos), (ssize_t)1);
	KUNIT_EXPECT_EQ(test, result[0], 'Z');
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_copy_drive('Z', "dir/moved", 'Z', "dir/moved"), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_copy_drive('Z', "missing", 'Y', "copied"), -ENOENT);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_copy_drive('Z', "dir/moved", 'Y', "../escape"), -EINVAL);

	/* Replace an existing inode while retaining the source FD. */
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_rename_drive('Z', "dir/moved", 'Z', "target"), 0);
	fput(f->files[0]);
	f->files[0] = mt6797_md1_fs_provider_open_ram_drive('Z', "source", true, true, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->files[0]));
	pos = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_write(f->files[0], "new", 3, &pos), (ssize_t)3);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_rename_drive('Z', "source", 'Z', "target"), 0);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_getattr_drive('Z', "target", &attr), 0);
	KUNIT_EXPECT_EQ(test, attr.size, (u64)3);

	page = kunit_kzalloc(test, 4096, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_stats_drive('Z', &before), 0);
	pos = 4096;
	for (i = 0; i < 32; i++)
		KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_write(f->files[0], page, 4096, &pos), (ssize_t)4096);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_stats_drive('Z', &written), 0);
	KUNIT_EXPECT_LT(test, written.free_blocks, before.free_blocks);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_mutate_drive('Z', "target", MT6797_MD1_FS_UNLINK), 0);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_getattr_drive('Z', "target", &attr), -ENOENT);
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_stats_drive('Z', &unlinked), 0);
	KUNIT_EXPECT_EQ(test, unlinked.free_blocks, written.free_blocks);
	pos = 0;
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_read(f->files[0], result, 3, &pos), (ssize_t)3);
	KUNIT_EXPECT_MEMEQ(test, result, "new", 3);
	fput(f->files[0]);
	f->files[0] = NULL;
	/* fput may defer final inode release; flush before checking free space. */
	flush_delayed_fput();
	KUNIT_ASSERT_EQ(test, mt6797_md1_fs_provider_stats_drive('Z', &closed), 0);
	KUNIT_EXPECT_GT(test, closed.free_blocks, unlinked.free_blocks);
	KUNIT_EXPECT_EQ(test, mt6797_md1_fs_provider_mutate_drive('Z', "dir", MT6797_MD1_FS_RMDIR), 0);
}

static struct kunit_case mt6797_fs_vfs_cases[] = {
	KUNIT_CASE(mt6797_fs_vfs_operations), {}
};
static struct kunit_suite mt6797_fs_vfs_suite = {
	.name = "mt6797-md1-fs-vfs", .test_cases = mt6797_fs_vfs_cases,
};
kunit_test_suite(mt6797_fs_vfs_suite);
