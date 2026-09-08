# MD1 RAM namespace operations

CREATEDIR (0x1007), REMOVEDIR (0x1008), and DELETE (0x100f) now
perform mkdir, empty-directory rmdir, and regular-file unlink on consumed,
explicitly enabled volatile Z/X/Y authorities. Read-only registrations still
fail before any mutation. Each request has exactly one UTF-16 path and returns
one signed vendor status, mapped from the actual provider result.

Installed Android25 daemon evidence (SHA256
688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf):
CREATEDIR dispatch 0x562c reaches mkdir at 0x7af8 with mode 0x1f8 (0770).
REMOVEDIR dispatch 0x580c reaches rmdir at 0x7cbc. DELETE dispatch 0x6274
calls 0x10164, reaching unlink at 0x103a0. These operations are not recursive.
Existing errno mapping preserves ENOENT=-9, EACCES=-16, other common failures
(including EEXIST, ENOTEMPTY, ENOSPC and EROFS)=-18. The daemon was not executed.

The provider retains the chosen root, validates the leaf, walks the parent
with the existing component confinement, takes mount write access and locks
the parent before final lookup. Root mutation, symlinks, covered mountpoints,
foreign superblocks and special-file deletion are refused. No recursive walk
or persistent provisioning write is added. Existing file descriptors remain
subject to ordinary VFS unlink lifetime rules.

This kernel's vfs_mkdir consumes its input dentry on both success and failure
and can return a replacement dentry. The provider releases only the returned
successful reference. Rmdir and unlink retain the caller's reference. All
failure paths release parent locks, mount write access, and root references.

Verification:

- test-cellular-fs-service.py executes the actual codec/service with a mock
  provider: all three drives, exact argument count, path refusal, actual
  provider errors, signed reply shape and existing FS regressions.
- test-cellular-fs-mutations.py executes the actual provider mutator against
  instrumented VFS boundaries with ASan/UBSan. It checks authority denial,
  callback selection, lookup/allocation/mount failures, confinement refusals,
  directory/type errors, mkdir reference consumption/replacement, and cleanup.
  Parent traversal is mocked; this does not prove real-kernel tmpfs behavior.
- ARM64 W=1 compilation covers the actual provider and service objects.

Hardware qualification is outstanding. CI8 continues to run the earlier
f8029 kernel. Rename, move, restore, recursive delete and persistence still
need implementation and qualification; this is not a complete FS claim.
