# NVRAM backup format primitives

`nvram_backup_format.py` implements the independently verified checksum and
140-byte record decoding used by the installed Android25 libnvram.so
(SHA256 1bf4a5590307384cb7587307d592f0c75f2505668060a84998b9e9aec95027ee).
These are host-side parser components, not a wired kernel RESTORE provider.
They do no device I/O, extraction or publication. No private backup is needed
for the synthetic tests:

```
python3 tools/gemini/test-nvram-backup-format.py
```

`Checksum.update(bytes)` accepts immutable chunks totaling at most 16 MiB.
The installed loop at 6b16..6b50 alternates XOR and modulo-32-bit addition
on little-endian words, starting with XOR. It adds the zero-extended trailing
bytes at 6c02 and XORs the total byte length at 6c80. `digest()` snapshots
without consuming partial-word state. `verify_checksum` compares against
AllMap's first word; it does not validate the map or authenticate its origin.

`decode_record(bytes, allfile_size)` requires exactly 140 bytes. It decodes
an opaque first word, data offset at +4, length at +8 and a NUL-terminated
128-byte name at +12. It requires a bounded extent within the supplied data
size and rejects empty/unterminated names. The returned name is opaque bytes:
this function neither validates a target path nor grants authority to write
it. The eventual publisher must translate only approved paths into bounded
volatile roots. Pathname-mode RESTORE copies the entire recorded length;
the LID-mode size+2 and decryption branches are separate in the vendor code.

Tests use hand-calculated checksum vectors, every chunk split, intermediate
snapshots, overflow and tail cases, exactly 16 MiB and refusal beyond it,
record boundary/overflow cases and malformed names. They do not prove raw
backup layout, actual backup validity, group selection or phone operation.

Remaining dependencies are the feature-dependent
AllMap group selector, stock fstab/source identity, and volatile publication.
RESTORE must continue returning failure until the whole validated path exists.


## Raw reconstruction

`reconstruct(raw, layout_version=0)` accepts immutable, whole-erase-block
input up to16MiB and returns in-memory AllMap/AllFile plus selected block
indices. It supports the installed128KiB eMMC geometry, log/reserved/DM
counts2/2/1, and the vendor layout0/1 arithmetic. Layout defaults are an
explicit installed-library profile, not auto-detection or source identity.
Returned payload fields are excluded from the object's repr to avoid logging
them accidentally. There is no device reader or extraction CLI.

The reverse scan requires three equal map copies followed by0xaabbccdd,
then selects the first zero map byte before the map block for the header.
AllMap must fit in that one block after the12-byte prefix; AllFile is assembled
from subsequent zero-marked blocks before the selected map. Only the recorded
length is retained, and checksum verification precedes return. Unsupported
geometry, invalid lengths, insufficient blocks and corruption fail without
returning a partial backup. An invalid candidate map is skipped as in stock;
a selected map with bad payload does not fall back to an older candidate.

The stock732c..7398 bound reduces to counting available blocks strictly
before the selected map index k: let N=map length, h=header index, and g=gap
to first data minus1. The scan counter is k-N+1. The equality test is
2-N+h+g == k-N+1, or h+g+1 == k (no data before map). The following loop
counts k-h-g-1 bytes starting at h+g+1. The bounded implementation expresses
that directly as zero-marked indices in range(header+1, map_block).

Synthetic reconstruction tests cover both layouts, a nonzero header index,
noncontiguous data blocks, partial/full/empty payloads, excluded tail blocks,
all three map-copy corruptions, wrong magic, candidate ordering, no fallback
after payload failure, invalid geometry/map/data lengths, out-of-range blocks
and checksum failure. This is format qualification using synthetic bytes;
no live backup or modem RESTORE has been qualified. Group/record validation
and confined publication remain necessary even after reconstruction succeeds.
