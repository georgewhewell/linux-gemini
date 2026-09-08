# MD1 RAM RENAME

The installed Android25 daemon dispatches RENAME (0x100b) with two UTF16
paths and calls libc rename at ELF VA 0x9f84. This operation now reaches
vfs_rename with flags zero on consumed volatile authorities. The service
requires exactly two arguments and retains both independently decoded paths
until the provider returns its real result, mapped to the vendor signed reply.

Both roots must be accessible, consumed writable tmpfs authorities. Different
mounts return EXDEV (vendor general failure); there is no copy fallback.
Neither root itself is a valid operand. Existing component traversal confines
both parents. start_renaming performs final lookup, permission checks, parent
locking and ancestor traps. Under these locks, the provider rechecks backing,
mountpoints, superblocks, source existence, and regular-file/directory types.
It then calls vfs_rename with replacement allowed. Same-name handling and
file/directory replacement errors are delegated to the actual VFS operation.

This kernel's start_renaming owns both final dentries and one extra old-parent
reference on success. end_renaming releases these and the locks. The provider
then drops its mount write access, both caller-owned parent references, both
path buffers and both root references. Failure before start_renaming succeeds
never calls end_renaming.

Validation:

- Actual codec/service test covers all nine drive pairs, both independent
  paths, exact argument count, malformed path refusal and signed error replies.
- test-cellular-fs-rename.py extracts the production parent helper and rename
  function and runs them with instrumented VFS boundaries under ASan/UBSan.
  It checks both root acquisitions, allocation/parent failures, mount mismatch,
  start_renaming failures, every final confinement refusal, missing source,
  replacement/same-name dispatch and cleanup on VFS failures. It reuses the
  existing namespace test's boundary shim without executing that test.
  Actual parent walking and VFS rename effects are mocked; these tests do not
  prove real tmpfs replacement or rename concurrency.
- ARM64 W=1 compilation covers provider and service objects. Existing namespace,
  codec/state, and attribute worker tests pass.

Hardware qualification remains outstanding; CI8 still uses the f8029 kernel.
MOVE, RESTORE, XDELETE and persistent writeback remain unimplemented. MOVE
must not alias RENAME: the installed daemon's KILL branch only logs and does
not unlink source. See the separate installed-daemon audit in the handoff.
