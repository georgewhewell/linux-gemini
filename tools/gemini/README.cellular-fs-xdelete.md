# MD1 XDELETE

XDELETE (0x1018) now accepts exactly one UTF16 directory path and one u32 flag
word. Nonzero subsets of FILE=4, DIR=8 and RECURSIVE=16 are valid. Successful
replies contain the number of entries deleted, not an unconditional zero.
A non-directory initial target returns vendor parameter error; ordinary VFS
errors use the existing vendor mapping.

Installed Android25 source: handler0x6764 calls0x10660. At0x10894 it requires
nonzero flags&0x1c;0x108a0 clears those bits and rejects any remaining bits.
It stats the supplied directory, calls helper0x11ed8, and additionally removes
the starting directory only when DIR|RECURSIVE is set (0x109f4..0x10a14).
The helper isolates FILE at0x11fc0, uses DIR/RECURSIVE at0x11f3c/0x11f44,
descends at0x120ac, removes child directories at0x120d0 and regular files at
0x120f4, accumulating a count returned at0x12390. Source is the already verified
installed ccci_fsd binary; it was not executed.

The provider implements iterative post-order traversal. Storage is bounded by
PATH_MAX/2+1 frames, one bounded path and one directory entry. It visits at
most4096 entries, matching the registered tmpfs inode budget. It does not use
recursive kernel calls. Retained roots must be consumed, writable volatile
backing. Contents of a root may be selected, but requests that would remove
the retained root itself are rejected before any effects.

All lookups and mutations use the existing confined provider operations.
The existing iterator skips symlinks, mountpoints and special/foreign entries;
it never follows them. If such an entry prevents requested directory removal,
that removal fails. On any traversal or mutation error the operation stops,
releases every open directory and buffer, and returns the error. Earlier
successful deletions are not rolled back. This explicit first-error behavior
may differ from stock's logging/continuation after some nested errors; it
never claims a complete count after a failed operation.

Validation:

- Actual service/codec test: all seven flag combinations, Z/X/Y routing,
  positive deletion counts, single signed reply, malformed request refusals.
- Actual provider with ASan/UBSan and instrumented boundaries: all allocation
  failures, open/read/mutation errors, deep-path bound,4096-entry cutoff,
  partial-error return, and every file/buffer cleanup path.
- Real UML/tmpfs test: mixed two-level trees for all seven flag combinations,
  count/survivor checks for FILE and recursive FILE, complete recursive removal,
  non-empty DIR refusal, invalid flags/root removal/traversal/non-directory.
- ARM64 W=1 provider and FS service object compilation.

Phone/modem qualification remains outstanding. This completes the implemented
XDELETE service path, not full filesystem bringup: MOVE integration, RESTORE's
backup parser and persistence remain separate required work.
