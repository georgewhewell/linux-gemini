# AP CLDMA post-GO admission versus reset-time IP_BUSY

This correction follows the C2 slice fix b0d2d8951b831, based on integration
40e3b806287c83567da52b7cb66689e64d28de9a. No hardware was accessed. C2 saved
evidence is handoff/cellular-c2-Dye0a5HI/dmesg-first-event.txt, boot
2e265918-7126-496e-af40-4b376468d10a: after MD1 GO, the passive final observation
has CLDMA IP_BUSY=1 while AP UL/SO and L2 status remain zero.

Both the supplier and consumer read the same AP CLDMA PDN + 0x04b4 register.
Vendor source gemini-vendor-kernel-3.18 at
8cfe6596a503612e3332d9c26e292a19525a7f07 establishes these distinct phases:

* eccci/mt6797/cldma_reg.h:253 calls CLDMA_AP_CLDMA_IP_BUSY half-DMA busy status.
* eccci/modem_cldma.c:2154 pulses AP CLDMA reset before md_cd_power_on().
  Line 2167 releases MD1. It then calls cldma_reset() (configuration, not a
  hardware-reset pulse or IP_BUSY acknowledgement), then cldma_start().
  Neither function requires IP_BUSY zero before installing AP descriptors.
* Lines 619-621 and 1205-1208 acknowledge the observed IP_BUSY bits while
  handling RX, explicitly describing this as the CPU-wakeup case.
* Lines 2220-2250 wait for MD TX to drain and IP_BUSY to clear during shutdown.
  That is a drain boundary, not initial post-GO AP descriptor ownership.

Consequently MD-driven busy/wakeup state after GO is not equivalent to an
unclean AP reset or existing AP descriptor ownership. C2's value is consistent
with that role; this does not decode undocumented individual bits or establish
a completed handshake. No early W1C acknowledgement is added.

The supplier's PRE-power reset callback still requires IP_BUSY=0, together with
zero queue/IRQ status, before publishing reset_completed. The exclusive reset
controls, managed supplier lifetime, single-consumer AP mapping lease,
reset_completed flag, GO/passive evidence and live memory contract remain
mandatory. The consumer's POST-GO fresh snapshot still rejects every nonzero
UL/SO, TX/RX interrupt or any of fourteen AP queue heads before its first write.
Only IP_BUSY changes from a reset-time predicate to a retained diagnostic at
that later boundary. The log explicitly calls it observed-only after MD1 GO.
The AP start sequence and IRQ/descriptor handling are otherwise unchanged.

The actual-code test executes the unchanged PRE-power reset callback and proves
busy=1 is rejected without reset_completed. It executes the actual POST-GO
snapshot with busy=0 and busy=1, verifies the logged value, and injects each of
the other eighteen reads with both busy states: every active queue, head and
IRQ is refused. The extracted check has no write operations. Source-order
assertions retain GO/passive, lease/reset proof, and checks before start writes.
This corrects an incompatible lifecycle predicate, not a hardware success claim.

Both changed CCCI and CLDMA core objects cross-compiled successfully with
AArch64 GCC 12 in /tmp/gemini-ccci-postgo-build-SWYGxJkj. The actual lease
tests also pass, including 32 competing consumers and terminal teardown.
