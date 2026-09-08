# Immutable restore authority preparation

`nvram_restore_archive.py` prepares a sealed memfd from a validated Backup.
It repeats checksum/group/record validation, requires an explicit supported
profile, maps only the installed MD1 Z/X/Y prefixes, and retains the first
eligible occurrence of each target. AP/other-modem records are excluded.
The result is a caller-owned descriptor; there is no filesystem publication,
provider registration, ioctl or modem RESTORE integration yet.

The archive is an internal versioned format, not the stock backup format.
All integers are little-endian. Header16 bytes:

| Offset | Field |
| --- | --- |
| 0 | Eight magic bytes `MD1BKP1\0` |
| 8 | u32 entry count (1..4096) |
| 12 | u32 total archive bytes (at most16MiB) |

Each entry is140 bytes immediately after the header:

| Entry offset | Field |
| --- | --- |
| 0 | u32 ASCII drive Z/X/Y |
| 4 | u32 absolute payload offset |
| 8 | u32 payload length |
| 12 | 128-byte NUL-terminated, zero-padded UTF-8 relative path |

Payloads follow the complete index contiguously in entry order, with no
alignment padding and no external paths. This handles empty files without
inventing content. Duplicate targets retain the first selected source;
source extents are already bounds-checked. The total-size limit is checked
before archive allocation, including cases where overlapping source extents
would expand the archive beyond16MiB.

The preparation routine creates a memfd with CLOEXEC and ALLOW_SEALING,
writes all bytes with partial-write handling, adds WRITE/GROW/SHRINK/SEAL
seals, verifies those seals, rewinds and returns it. Every post-creation
failure closes the descriptor. The consumer must independently verify the
magic, exact length, all counts/extents/paths/duplicates, and seals before
retaining it. A trusted preparation process and protected source identity
are still required; a seal guarantees immutability, not authenticity.

Validation:

```
python3 tools/gemini/test-nvram-restore-archive.py
```

Tests check a literal three-drive archive, exclusion and first-match behavior,
traversal refusal, total-size expansion, real Linux seal enforcement and
CLOEXEC, short writes, and descriptor cleanup on write/seal errors. These are
host tests with synthetic contents. No private backup has been archived or
registered by this code yet. Kernel consumption and copying into the consumed
bounded writable roots remain required to implement RESTORE.

## Kernel format validation

`drivers/soc/mediatek/mt6797-md1-fs-archive.h` provides the kernel-side
validator. It requires the exact version magic/length, a bounded count and
complete index, approved drives, strict UTF-8 canonical relative paths and
zero padding. Payload extents must be contiguous in entry order, start after
the complete index, and end exactly at the archive length. Duplicate targets
are rejected; identical paths on different drives are distinct. Empty files
are valid. Failure leaves the output view unchanged.

Validation runs in process context with no provider lock; duplicate checking
is bounded by4096 entries and yields between entries. The returned view borrows
an immutable buffer owned by the caller. It is not sufficient to retain a
mutable userspace buffer after validation. File-type/seal checking, descriptor
retention, registration lifecycle and RESTORE dispatch still need integration.

`CC=<native-gcc> python3 tools/gemini/test-nvram-archive-kernel.py` compiles
and runs this exact kernel header with ASan/UBSan against a producer-generated
synthetic fixture and malformed variants. Cases include every truncated input
length, forged counts/offsets/sizes, duplicate targets, malformed Unicode,
path escapes, padding, empty files and unchanged failure outputs. Inclusion
in the provider translation unit also checks ARM64 compilation; the validator
is not yet called by a registration path or qualified on the phone.

## Registration lifecycle

`MT6797_MD1_FS_REGISTER_ARCHIVE` is ioctl0xf4 using the existing16-byte
registration structure: version1, `directory_fd` carrying the archive memfd,
flags/reserved0. It requires the provider enable flag, initial-user-namespace
CAP_SYS_ADMIN and enable_volatile. Register it before writable roots; it does
not trigger cold probe. Older clients may still register roots without one.

Registration requires a readable regular tmpfs file with all four seals,
size16..16MiB, and a complete read. It validates a private kernel copy, then
publishes only while the provider remains EMPTY. The input FD reference is
released after copying; userspace may close its descriptor. The kernel copy
is not exposed for mutation. An in-flight archive load blocks root registration
with EBUSY; an installed archive disallows legacy read-only root registration.
A second archive or late registration fails. Loading failure permits retry
unless cancellation has made the provider terminal.

CANCEL before consumption detaches and clears the archive, frees its bytes
with kvfree_sensitive outside the spinlock, and blocks late publication.
Once roots are consumed, cancellation remains EBUSY and the archive lives for
this boot. File reads/validation/allocations run without the provider spinlock.
As with root registration, cancellation immediately after successful publication
may revoke it before the registering call returns; success is not modem GO.

`test-nvram-archive-registration.py` executes the actual loader with ASan/UBSan
and instrumented kernel file boundaries. Existing registration regressions also
cover ioctl routing, root ordering and archive cancellation. The real memfd
seal test is in the producer suite. ARM64 provider compilation passed with
MEMFD_CREATE enabled. This registration path has not yet been exercised by the
phone; RESTORE dispatch and archive-to-RAM copy remain unimplemented.
