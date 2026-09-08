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
