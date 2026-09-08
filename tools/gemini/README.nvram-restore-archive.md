# Immutable restore authority preparation

`nvram_restore_archive.py` prepares a sealed memfd from a validated Backup.
It repeats checksum/group/record validation, requires an explicit supported
profile, maps only the installed MD1 Z/X/Y prefixes, and retains the first
eligible occurrence of each target. AP/other-modem records are excluded.
The result is a caller-owned descriptor with no filesystem publication.
The preparation CLI registers it before RAM roots, as described below.

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
registered by this code yet. Kernel consumption and copying into consumed bounded writable roots have
separate tests below; phone qualification remains pending.

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
mutable userspace buffer after validation. Registration independently checks file type/seals and owns the copied bytes
through the lifecycle described below.

`CC=<native-gcc> python3 tools/gemini/test-nvram-archive-kernel.py` compiles
and runs this exact kernel header with ASan/UBSan against a producer-generated
synthetic fixture and malformed variants. Cases include every truncated input
length, forged counts/offsets/sizes, duplicate targets, malformed Unicode,
path escapes, padding, empty files and unchanged failure outputs. Inclusion
in the provider translation unit also checks ARM64 compilation; the validator
is called by registration but has not been qualified on the phone.

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
phone; RESTORE dispatch and archive-to-RAM copy are tested below.

## RESTORE service and RAM copy

RESTORE0x1021 now requires exactly one decoded path and invokes the provider.
The installed library returns a boolean: provider success maps to vendor0,
all provider failures to general failure-18. Path decode and malformed argument
errors retain their ordinary wire statuses. Missing backup data never falls
back to mutable seeds or another drive.

The provider requires consumed writable roots and a registered archive. It
matches the exact drive/relative path before creating anything. The immutable
archive copy cannot be freed after consumption, so lookup/read lifetime is
stable without holding the spinlock over filesystem operations. Missing
parents are created through the confined namespace provider; an existing
parent must pass the confined directory open. The target is created/truncated
through the writable RAM provider and all recorded bytes are written with
short-write/error handling. Empty files are restored by creation/truncation.
A failure can leave newly created parents or a truncated/partial destination;
this is not an atomic transaction or rollback mechanism.

The UML suite exercises actual production operations on isolated tmpfs roots:
Z/X content, Y empty file, missing nested parents, repeat restore after a
longer modification, unchanged archive bytes, missing backup with no parent
creation, invalid alias, non-directory parent and disabled writable authority.
The service suite covers all three drives, exact argument count and boolean
error mapping. ARM64 provider/service objects compile with W=1. These tests
bypass archive registration in the UML fixture; admission has separate tests.
Phone preparation-service integration and hardware qualification remain pending.

## Boot preparation CLI

`mt6797_fs_restore_register.py --profile gemini-stock-ecci-md3-v1 --sha256 HASH`
requires an explicit lowercase SHA256 and the reviewed profile (layout0,
ECCI on, MD3 on, MD5 off, header offset0). It does not infer another profile
when validation fails. The captured properties support this profile; Android
runtime property overrides are not proven. The live backup's populated groups
are unaffected by the remaining MD5 ambiguity.

It opens only `/dev/mmcblk0p19`, read-only, CLOEXEC and NOFOLLOW, checks block
identity against sysfs, exact nvram/PARTN19 fields, 10240 sectors and ioctl
size5242880. It reads bounded chunks and checks the supplied hash before
reconstruction/sealing, then repeats source identity and the full read/hash
before publication. It opens the provider with NOFOLLOW, checks its character
device identity against sysfs and sends the16-byte f4 archive registration.
All FDs close on success or failure. It disables core dumps and emits only a
fixed success/failure message; no raw bytes, paths from the backup, or identity
fields are printed or saved. It does not register roots or trigger modem GO.
A source change after the final check is outside this snapshot check; boot
service source rechecks remain necessary. Python transient memory is not
explicitly scrubbed or locked against swap.

`test-fs-restore-register.py` executes the actual reconstruction, archive,
sealing, and preparation code against a synthetic5MiB backup with device/sysfs
adapters. It checks real memfd contents/seals at the ioctl boundary, short
reads, changed/hash-mismatched source, EOF, wrong type/size/partition (including
duplicate fields), wrong provider, explicit profile refusal, ioctl failure,
FD cleanup and unchanged input. This is not a real device ioctl test.
