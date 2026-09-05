# C3 handshake boundary and bounded first-fatal evidence

Source base: `5abceee47842987bc15906f67aa182a3068b3e97`.
No hardware action or new boot authorization accompanies this change.

C3 handoff `cellular-c3-w5ObdBn9` records HS1/runtime queued at 7.642719 and
first terminal errno -110 at 23.645382. The AP handshake timer is 30*HZ from
probe completion at 7.642598, so this approximately 16-second event precedes
that timer. Both saved interrupt snapshots show CLDMA hwirq265 count2,
MD-WDT hwirq266 count2 and CCIF147 count0. The MD-WDT callback also uses
-ETIMEDOUT: modem WDT is consistent with the evidence, unlike an ordinary
30-second AP timeout. Existing logs cannot prove TX completion or identify
which raw status caused either CLDMA interrupt. HS1 reception proves RX
worked, not that the AP runtime reply was consumed or accepted.

## Diagnostic contract

The existing first-failure atomic latch still chooses one caller. It now
freezes diagnostics before logging, frontend changes or existing STOP/mask
writes. Non-orderly failure records distinguish `md-wdt-irq`,
`ap-handshake-timer`, and other `transport` failure. The old terminal marker
remains intact. Orderly shutdown and repeated failures add no diagnostics.

Saved state is limited to first and latest raw CLDMA TX/RX status/mask tuples
plus interrupt count (captured from the existing reads, before existing W1C),
Q0 software submit/reclaim counters and head/tail/used, and the first Q0 slot's
16 coherent descriptor bytes. Slot observation is sequential READ_ONCE, not
an atomic hardware snapshot. Slot memory persists through IRQ/work draining
and consumer teardown; diagnostic code takes no TX lock. TX producer and
consumer may nest the separate diagnostic lock, never the reverse.

The first HS1 query (88 bytes), prepared runtime block (at most 2048 bytes)
and AP response (116 bytes) are saved in device-owned RAM. The response is
finalized exactly once after TX header/sequence initialization and before
DMA mapping, indicated by `reply_finalized=1`. If failure occurs earlier,
the flag remains zero and the saved response is only the prepared response.
Neither finalized nor submitted implies device consumption. Runtime may
contain its existing random seed but no AT traffic or subscriber data.

All state writers use the same IRQ-safe lock and refuse mutation after freeze;
payload writes are bounded and first-only. Logging occurs after unlocking,
with offsets rather than kernel pointers, at most one summary plus 72 hex
lines (16+88+116+2048 bytes, 32 per line). Descriptor contents may contain DMA
addresses, deliberately required for this diagnostic; no CPU pointers are
printed. No additional MMIO read, W1C, reset, retry, timer change or transport
policy is introduced. Existing races between device DMA and software remain
observations, not claimed coherent hardware history.

Actual-helper host tests cover size/null refusals, immutable copies, finalized
reply, first/latest IRQ, descriptor/no-descriptor snapshots, counters,
saturation, post-freeze refusal and 32 concurrent writers/freezers. The
existing actual quiesce fixture verifies one diagnostic, quiet repeat/orderly
paths and unchanged four writes/one read/one crash/fatal action. Wiring tests
check cause labels, freeze-before-print/STOP, capture-before-W1C and finalized
header placement. Full integration, reset-admission and lease fixtures pass.
GCC12 AArch64 CCCI object compiles in
`/tmp/gemini-ccci-firstfatal-build-I43j9HWu`.

## MPU extent: real mismatch, not yet a safe expansion

Vendor provenance: `gemini-vendor-kernel-3.18@8cfe6596a503612e3332d9c26e292a19525a7f07`.
`drivers/misc/mediatek/eccci/modem_cldma.c:2904–2913` advertises AP/MD1 plus
MD1/MD3 size in `set_md_mpu_total_size`, with feature[1].version=1 describing
that combined extent. Current common reply also marks version1 but receives
`config.smem_size=1 MiB`. That is neither the validated 2 MiB AP/MD1 mapping
nor the vendor combined extent. Runtime addresses themselves match vendor:
AP runtime at mapped base+0x10000, two separate 0x800 AP/MD runtime areas.

Do not replace the mapping/resource size with total parent size. The published
layout exposes only the validated AP/MD1 slice; the 6 MiB parent also contains
other modem subdivisions. The current LK parser checks each subdivision's
parent bounds but not pairwise non-overlap/adjacency. Current MPU plan builds
shared region7 for AP/MD1 only; C3 readback/log range is 0x88000000–0x881fffff.
No captured MD1/MD3 permission proof justifies advertising a larger accessible
range. A proper follow-up must independently validate physical overflow,
AP/MD1→MD1/MD3 adjacency, exclusion of AP/MD3, and permissions/ownership before
publishing a separate advertised MPU extent. Test expected values must be
literal vendor-derived fixtures rather than copying implementation constants.
No speculative memory expansion or feature-version change is included here.
