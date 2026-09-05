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
mask-plus-pending publication. Before calling napi_complete_done(), poll
atomically snapshots and resets the old ownership shadows. This matters
because successful completion releases SCHED and can admit a new poll before
the previous callback returns. No post-completion clear can erase that new
poll's shadow ownership.

Every false completion atomically ORs the old snapshot back. This covers busy
poll, MISSED and deferred-hard-IRQ completion; the latter can release SCHED
and arm a timer before returning false (net/core/dev.c:napi_complete_done).
Atomic OR preserves ownership acquired by a concurrent newer poll. Completion
is never called with the TX/STOP lock held: it can flush GRO and re-enter TX.

Successful completion acquires the TX/STOP lock, checks the terminal latch,
and rearms the accumulated snapshot minus any bits now pending or owned by
a newer poll. Thus terminal STOP cannot be followed by an interrupt re-enable,
and a previous completion does not unmask queues newly owned by a new poll.
Budget-zero polling reclaims TX but retains RX work/masks and never completes,
including the terminal branch. No additional IRQ acknowledgement, retry,
modem response, EINT value, memory permission, DT or deadline is introduced.

## Validation

`test-cellular-napi-mask.py` executes actual IRQ, poll and STOP bodies. It
covers the exact nine-RX plus late-TX sequence, simultaneous queues exhausting
the full budget, zero budget, no-new-pending polls with old ownership, false
completion, concurrent IRQ, deferred false completion with a cross-CPU next
poll, and terminal failure during completion. All mask register effects are
checked through adapters; completion asserts that the hardware lock is not
held. Actual RPC transport, first-fatal and terminal tests remain passing.

ARM64 CCCI object cross-compilation uses GCC 12.2 and isolated output
`/tmp/gemini-ccci-napi-mask-build-gFBusSZT`. No hardware access or new boot was
performed by this source lane. Correct NAPI rearming is not yet hardware
evidence of HS2 or cellular registration; those remain required outcomes.
