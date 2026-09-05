# C8: interrupt progress exposes filesystem service request

C8 image e3f56dc095d8b019e67416d966d742fcd7aadf819bb34d690261eb9f5879fae6,
source b71187c5f02b8ee63225c65892a806e7f42f0a92, boot
c9c9263b-149f-461a-b6c2-e2458041a3c4. Source changes are the reviewed NAPI
mask-union/generation and filtered interrupt-ack fixes, with exact C7 DT.

Root inspected the owner-saved first-event log in
`handoff/cellular-c8-ZacNg6I4`. Twelve SIM EINT replies were queued successfully:
six status0 and six QUERY_STRING -11, versus nine replies in C7. At the
37.893184 AP-handshake timeout, ready remains0, IRQ count6, runtime636 bytes,
and the latest IRQ's RX mask is ff00ff00. This is an IRQ snapshot, not a new
fatal-time MMIO read. Q0's sole runtime reply is reclaimed. HS2 is not proven.

One complete44-byte queue1/channel14 FS_RX packet is now captured before
ready, with payload operation word100e. It is a filesystem-service packet,
not RPC4005 or an unknown RPC to answer with fabricated success. Its exact
vendor operation, backing semantics and safe provider are the next source
investigation. Owner-restricted raw evidence is not reproduced here.

The additional exchanges and newly reached FS request are hardware progress,
not cellular attachment/data or overall handshake success. At this document
boundary the original transaction is still owned; full evidence and exact
accepted-rescue/pstore/hash/network/lock closure remain required. No retry
or filesystem/provisioning mutation is authorized by this result.
