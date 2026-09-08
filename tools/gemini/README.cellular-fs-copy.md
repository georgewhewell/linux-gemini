# Bounded RAM file-copy primitive

copy_drive supplies the data-copy portion needed for installed-daemon MOVE.
It is not yet connected to the wire operation: MOVE flag handling, directory
overwrite/XDELETE and the installed unsupported KILL behavior remain separate
work. In particular this primitive never unlinks the source.

Source and destination use the existing confined RAM open operations. The
source must be a regular file no larger than the existing 16 MiB per-file
limit. Its initially reported byte range is read into bounded storage before
opening or modifying the destination. Premature EOF, read failure or allocation
failure abort without destination effects. Reads and writes use at most64 KiB
per call, handle short positive results, and yield between chunks.

The destination is opened without truncation, compared against the source
inode, then truncated and written. Same-name and same-inode copies are refused
before truncation. Cross-root file copy is allowed. Empty files truncate the
destination correctly. New destination files inherit the provider's private
0600 mode; existing destination permissions are retained. Source permissions
are not cloned. This differs from stock creat's0777 request but preserves the
modem-visible writable regular-file attribute and keeps RAM backing private.

A failure after destination truncation can leave partial destination contents;
it returns the real failure, never success. This is not atomic replacement or
a transaction. The buffered source is not an atomic snapshot against concurrent
host-side edits. The modem's ordered filesystem worker supplies serialization
for modem operations; hostile concurrent host modifications are not qualified.
No persistent partition or backup region is used.

Verification:

- Production copy function with fault-injected I/O under ASan/UBSan: full,
  empty and16 MiB boundary; oversize; short reads/writes; allocation failure;
  premature EOF; read/write errors; identity alias; policy refusal; cleanup.
- Real UML/tmpfs suite: cross-root copy, replacement visible through an already
  open destination FD, source retained, identical-path/missing-source/traversal
  refusals, alongside prior namespace and live-space tests.
- ARM64 W=1 provider object compilation.

This primitive does not establish complete MOVE or phone qualification.
