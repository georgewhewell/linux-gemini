# Installed MOVE service semantics

MOVE (0x100c) accepts exactly two UTF-16 paths and one u32 flags argument.
Valid flags are 1, 2, 3, 0x10001, 0x10002 and 0x10003. Both paths use the
existing Z/X/Y canonical decoder and retained provider roots.

The source is the installed Android25 ccci_fsd, SHA256
688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf.
Static ARM32 control flow, not just public flag names, defines the behavior:

- COPY (1) calls the bounded RAM copy provider. It can truncate an existing
  regular destination even without OVERWRITE. It never deletes the source.
- KILL (2) checks source read-only attributes. The installed branch at c47c
  only logs "kill ... NULL"; it does not unlink. KILL alone without a deleted
  destination returns general failure (-18).
- OVERWRITE (0x10000) checks destination read-only attributes, then unlinks
  an existing regular destination or XDELETEs a directory with flags 0x1c.
  Successful deletion precedes COPY. With no COPY bit, its deletion status
  (including a positive recursive count) is returned. A missing destination
  skips deletion and proceeds to the COPY-bit test.
- Read-only attributes return FS_READ_ONLY_ERROR (-45). The installed source
  branch bcb8 and destination branch bea8 join beac (mvn r6,0x2c). Attribute
  detection uses the existing installed mode mask, no bits 0333 and some 0444.
- Missing destination stat at bdb0 reaches c03c, then logging c370 and c380/
  c388, preserving the initial -18 unless subsequent copy succeeds.

The service deliberately refuses a root overwrite and a same-drive
ancestor destination before deletion. Canonically identical paths also
return parameter error (-2). Canonical paths and confined no-symlink/mount
traversal make the ancestor check meaningful for serialized modem requests;
it is not a transaction against concurrent host namespace changes.
Metadata errors other than ENOENT stop before mutation rather than copying
past the stock daemon's logged stat failures. Confinement and authority
errors must not authorize a later destructive step.

Every mutation still requires consumed, writable bounded tmpfs authority.
RO seed partitions are not mutation targets. COPY and XDELETE retain their
own size, traversal, resource and error limits. A failure after deletion can
leave no destination, and a write failure can leave a partial destination.
No atomic replacement, rollback or persistence guarantee is implied.

## Validation

`CC=<native-gcc> python3 tools/gemini/test-cellular-fs-service.py` executes
actual codec and service code against instrumented provider boundaries.
MOVE cases cover all six flags and nine drive pairs, decoded paths and one
signed reply, missing/existing regular/directory destinations, deletion
before copy, KILL preserving the source, positive deletion counts, -45,
canonical aliases, ancestor/root refusal, metadata/deletion/copy failures,
and malformed arguments. Also run with AddressSanitizer/UndefinedBehaviorSanitizer.
This suite checks service dispatch and effects requested, not real VFS effects.

The unchanged copy and XDELETE providers already passed real tmpfs UML
qualification and separate fault tests (see their READMEs). The service
object compiles for ARM64 with W=1. Full MOVE through the modem on the phone
has not yet been qualified. The accepted CI8 phone kernel predates this work.
