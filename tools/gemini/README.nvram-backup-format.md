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

Remaining dependencies are raw block reconstruction, the feature-dependent
AllMap group selector, stock fstab/source identity, and volatile publication.
RESTORE must continue returning failure until the whole validated path exists.
