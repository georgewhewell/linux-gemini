# CCCI v2: preserve all 64 wire feature slots

Source base `bff0f949b395a4b9b394bdd5fd8955adce4c01c1`.
Vendor reference `gemini-vendor-kernel-3.18@8cfe6596a503612e3332d9c26e292a19525a7f07`:
`drivers/misc/mediatek/eccci/ccci_modem.c:629` loops to FEATURE_COUNT,
defined as64 in ccci_core.h:381. Lines842–846 append a zero-length record
even for non-existing slots. The named-ID enum ends at25; it is not the wire
record count. Current code mistakenly used that enum end, omitting slots25–63
and skipping negotiation of their modem query bytes.

The change sizes the AP feature table to the 64-slot wire count and emits all
64 records. Unnamed AP slots initialize to NOT_EXIST. Existing fail-closed
negotiation is retained: an unknown required feature returns EOPNOTSUPP,
optional/backward features not supported by AP become NOT_SUPPORT, and a
NOT_EXIST slot retains its requested version with zero data. Unlike the vendor
backward-compatible branch (which tests only version), we do not claim support
for unknown AP features; no fabricated payload is emitted.

The known C3-equivalent nine supported feature IDs0,1,5,7,8,9,10,19,20 carry
124 payload bytes. Their runtime length becomes636 (=64*8+124), rather than324
(=25*8+124). This is runtime shared-memory length; the queued response remains
116 bytes. All fit within the existing2048-byte AP runtime area. No resource,
MPU, firmware, queue, IRQ or watchdog policy changes. Whether this omission
caused the observed modem watchdog remains unproven pending captured evidence.

The host test compiles the complete actual common wire helper source. Literal
64-slot fixtures check every header through63, exact512 empty/636 populated
lengths, 116-byte response, unknown required63 rejection across16 versions,
optional/backward refusals across16 versions, retained NOT_EXIST versions,
unchanged output lengths on failure, short-buffer/append overflow and response
address overflow. KUnit's minimal fixture also checks literal64 and last ID63.
Neither test derives its expected record count from the changed array/enum.

## C4 correlation and compile receipt

Saved C4 `handoff/cellular-c4-16sE7tcp/dmesg-first-event.txt`, attributed boot
`60a3d930-8045-4933-9c96-1519270a7062`, decodes to a complete first-fatal record:
site=md-wdt-irq, Q0submitted1/reclaimed1/used0, reply_finalized1. Thus CLDMA
consumption/reclaim is evidenced, not just software enqueue. HS2 was not
observed; neither reclaim nor this fix proves modem payload acceptance.

Its exact 72-byte feature query is included literally in the actual-code test:
slot0 REQUIRED, slots1–27 OPTIONAL, slots28–63 NOT_EXIST. The old 324-byte
runtime ends at24 and therefore omits three actual optional requests as well
as the unused tail. Corrected output is636 bytes, with25–27 explicitly
NOT_SUPPORT and28–63 NOT_EXIST. The host test reconstructs the full response
from that exact query and checks every record and final length. First-event
host decoder also confirms the separate unfixed version1/1MiB MPU extent.

GCC12 AArch64 common.o compile passed, output
`/tmp/gemini-ccci-wire64-build-jnpWu19w` (handle10770 exit0).
No active/consumed C4 artifact or hardware was modified by this source work.

## C4 recovery closure

Original execution62117 closed at 05:22:07 on 2026-09-05 after the single
installed test returned no-HS2; no AT or acceptance publication was attempted.
The archive in that handoff has SHA256
`c436ffe1add814ac577a57f21b9b54501a060ec53a4d8080ed7ef48305c7c213`.
Pstore SHA256
`6fb0d8667bb5e0e8c074f6be49091407385fc616c57b5142049d1f8a4a39e9d3`
is attributable to C4, including its 168.670 recovery marker. Rescue boot
`766d69fd-6d18-4c09-8c80-728c02346b34` passed exact p22/p30/p31 readbacks,
USB/RJ45 health, CPUs0-7 and no failed units. Both locks were free and no
writer/supervisor remained. Full chronology and hashes are in `RESULT.md`.
At C4 closure, the corrected wire64 code had not yet been exercised.

## C5 hardware result: changed failure boundary, not HS2

C5 consumed candidate
`aba81399f07efdc89c96e03cda117dc9d70a9fcfb1ad0934786e3c4b3512db18`
once, using source `67ac10f8c60db237eac7d22ebb7daac6b6f4a4cc`.
Boot `8a5db888-5752-4d8a-8245-1e4e1bebd65e` accepted HS1 at 7.637182
and queued the 636-byte runtime representation. First-fatal evidence has
runtime_len636, reply_finalized1 and Q0 submitted1/reclaimed1/used0.
The unchanged modem query and expanded record count are independently
decodable from `handoff/cellular-c5-mOUB7Pe9/dmesg-first-event.txt`.

The fatal source changed from C4's modem watchdog at 23.639153 to the AP
handshake timer at 37.891790, with a later queue-1 RX interrupt. This is
measured progress past the previous failure boundary, not proof that the
modem semantically accepted every runtime field. No HS2 or AT occurred.
Queue status alone cannot establish the channel or request contents. The
next change captures bounded pre-ready drops; see README.cellular-preready-rx.md.
No unsupported service response or MPU permission expansion follows from C5.

Original execution19991 closed at 05:33:55 on 2026-09-05 after the single
installed test returned no-HS2. Full RAM archive SHA256:
`bb0f6d9ed0f6da03b1d0a85586c8208549ba86161f6ae36a7fb2fb530d1df90a`.
Pstore SHA256:
`c3daef42b77f6188a2871502c3153ef7af92ee77011a2b256836778f945ecaa5`,
attributed to C5 and its 167.577 recovery marker. Rescue boot
`c2e16ed7-941b-46cb-bfb2-1a08ecd1a9df` passed exact p22/p30/p31 hashes,
USB/RJ45, CPUs0–7 and no failed units. Locks were released and the original
supervisor/writer were gone. C5 is consumed, not a retry candidate.
