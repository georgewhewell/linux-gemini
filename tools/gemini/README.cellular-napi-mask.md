# CCCI NAPI mask ownership across polls

Source base 885b656b6497f1e4b9f345d902c315fc1b1b7f00 (exact C7 kernel).
C7 boot 4b4621f6-5ff9-4921-b290-5ec2e23926cd progressed through nine EINT
RPC replies (six status zero, three QUERY_STRING=-11), all queued successfully,
then timed out before HS2. Its first-fatal record retained last IRQ TX=0x202,
RX=0, RX mask=0xff00ff02: queue-1 RX was masked at that sampled interrupt.
No unsupported packet was captured. The negative replies do not identify
their requested names and do not establish the cause of the stalled boot.

## Concrete lost-mask control path

The old poll atomically consumed pending TX/RX work into local variables.
The IRQ handler had masked the corresponding queues. After handling fewer
than 64 RX frames, a concurrent TX IRQ could add pending TX work. Poll then
returned the full budget to request another poll, without retaining its
local RX mask ownership. The next TX-only poll completed and cleared only
its new local TX mask; it cleared zero RX bits. The old RX mask was lost
permanently. The full-budget branch similarly retained RX work but lost
the old TX mask. These are reproducible source bugs, independently of what
the next modem request might have been.

## Change

Two atomic mask-ownership shadows accumulate work claimed by NAPI. They
survive all repolls independently of whether a queue still has pending work.
Pending-to-owned transfer uses the existing TX/STOP lock briefly, as does IRQ
mask-plus-pending publication. Ownership remains a shared union throughout
completion. A u64 generation advances under the same lock at poll entry and
IRQ publication (no practical per-boot wrap). Poll snapshots that generation
before calling napi_complete_done(), without holding the lock across GRO.

False completion does not change ownership. An earlier draft detached the
union and restored it on false; the actual-code negative control demonstrates
that this strands a mask if a deferred timer runs a newer poll to completion
before the older false return restores it. net/core/dev.c:napi_complete_done
can clear SCHED and arm that timer before returning false; there is no
guaranteed additional poll after the old restoration.

Successful completion acquires the TX/STOP lock, checks the terminal latch,
unchanged generation and empty pending work, then drains and rearms the entire
shared union. A newer successful completion can drain old ownership before
an older callback returns; the older callback cannot subsequently restore or
erase anything. An active newer generation prevents older rearming. Terminal
STOP cannot be followed by interrupt re-enable. No lock is held across GRO.
Budget-zero polling reclaims TX but retains RX work/masks and never completes,
including the terminal branch. No additional retry,
modem response, EINT value, memory permission, DT or deadline is introduced.

IRQ acknowledgement is narrowed to raw status & ~sampled mask, retaining
handled error bits as well as done bits. Vendor 3.18 commit
8cfe6596a503612e3332d9c26e292a19525a7f07, eccci/modem_cldma.c:1137–1139
filters status this way before ACK writes at 1169 and 1199. A concurrently
arriving masked RX event was not claimed by the IRQ decoder and must not be
cleared by an unrelated TX IRQ. The W1C fixture preserves that status until
unmask and actual RX handling; it does not assume hardware reassertion after
software has discarded a pending status.

## Validation

`test-cellular-napi-mask.py` executes actual IRQ, poll and STOP bodies. It
covers the exact nine-RX plus late-TX sequence, simultaneous queues exhausting
the full budget, zero budget, no-new-pending polls with old ownership, false
completion, concurrent IRQ, deferred false completion with a cross-CPU next
poll completing before the older return, two overlapping completions, an
active newer poll preventing older rearm, and terminal failure during
completion. Genuine unmasked queue IRQs and deferred timer polls are modeled;
the fixture does not force-clear a masked queue to fabricate an IRQ. A W1C
fixture adds RX after the final descriptor check during an unrelated TX IRQ.
All mask register effects are
checked through adapters; completion asserts that the hardware lock is not
held. Actual RPC transport, first-fatal and terminal tests remain passing.

ARM64 CCCI object cross-compilation uses GCC 12.2 and isolated output
`/tmp/gemini-ccci-napi-mask-build-gFBusSZT`. No hardware access or new boot was
performed by this source lane. Correct NAPI rearming is not yet hardware
evidence of HS2 or cellular registration; those remain required outcomes.
