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
The writer permits AllMap plus its12-byte prefix to span at most two erase
blocks, using the next zero-marked block for a continuation. AllFile is assembled
from zero-marked blocks after the complete map and before the selected map. Only the recorded
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


## Writer-side two-block map correction

The installed libfile_op.so SHA256
bc44372fc6cb950d427955e66c0579353c4ccb845fb78db11851ae58e8de19d2
exports FileOp_BackupToBinRegion_All at3654. It rounds map.size+12 up to
write-page size at3856..386a and rejects only sizes above twice the erase
size at386e/3872. It writes the first erase block at3ace, finds the next
zero-marked block at3b02..3b14, and writes the remainder at3b50..3b60.
Data follows this last map block. This explains the live143132-byte AllMap;
the earlier one-block assumption came from the incomplete single-file reader.

Synthetic tests now include one-block capacity, the first byte beyond it,
143132 bytes, exactly two-block capacity, a gap between map blocks, and
insufficient subsequent data authority. A guarded read of the original5MiB
p19 backup passed reconstruction and checksum with mapblock33, headerblock0,
AllMap143132 bytes, AllFile1947576 bytes, and15 data blocks. Only numerical
metadata was reported; no payload was persisted or published. Source hashes
were checked before/after and on the host. This proves raw reconstruction
for that captured source identity, not complete record selection or RESTORE.
