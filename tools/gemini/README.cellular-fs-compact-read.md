# Android25 composite read, C12 follow-up

C12 (ee11f08f...) boots and serves OPEN/CLOSE/GETATTRIBUTES, then drops a
128-byte channel14 request with opcode0x1022. HS2 times out. Evidence:
handoff/cellular-c12-clock-fixed-avcCii7v/dmesg-candidate.txt in the workspace.

Contract recovered statically from installed Android25 ccci_fsd SHA256
688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf.
The daemon was not executed. Disassembly at workspace
handoff/cellular-c12-research/work/text.compact.

0x6ab4 copies a 40-byte argument after the UTF16 path. Words at offsets:
0 operations; 4 completion output; 8 error output; 12 open flags;
16 file size output; 20 signed seek offset; 24 seek origin;
28 modem buffer pointer; 32 requested length; 36 read count output.
The embedded buffer pointer is overwritten by the daemon and MUST NOT be
used by the AP. Flags1/2/4/8/16 select open/size/seek/read/close, at
0x6b00, 0x6f24, 0x6fe0, 0x700c, 0x703c.

Reply at0xb368 has FOUR arguments: 8 bytes {completed flags, FS error},
4 bytes file size, 4 bytes count, count bytes data. No leading generic status.
0x4af4..0x4b18 set the stack pointers establishing these fields.
Success0x7908 sets completion and error0. Error paths0x9734 (open),
0xad24 (size),0xb0ac (seek),0xb2bc (read) set completed operations and
vendor FS error; temporary handles close even on size/seek/read failure.
Internal results -1..-7 are only diagnostic stage codes, not reply arguments.

The service uses the existing confined read-only provider and handle cleanup.
Unread output fields are zeroed, not copied from modem input. Missing CLOSE
also cleans up and reports general failure rather than echoing arbitrary
request completion/error words as the vendor's malformed path can do.
Shape must be exactly(path,40 bytes), bounded by existing READ_MAX. Unknown
operation flag bits have no effect, matching the daemon's bit tests.
Tests cover the observed0x1f/0x700 request shape, offsets, EOF, fragmentation,
write-intent refusal, missing files, malformed size/argc, fstat/read errors,
and unchanged handle counts after every failure.
