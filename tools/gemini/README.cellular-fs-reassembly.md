# Channel-14 request reassembly

CI6 first activation returned EFUN OK and CPIN READY. It subsequently rejected
an op 0x1004 channel-14 frame of 3476 bytes (-EPROTO), then parsed a 560-byte
frame as a standalone request (-EPROTO, no response). The radio off command
never returned OK, and a later activation was refused despite EFUN? reporting
zero. The matching sizes and missing implementation identify fragmentation as
a concrete transport defect; hardware causality is still to be tested.

The installed Android25 ccci_fsd SHA256
688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf
provides the receive-side evidence (local disassembly
handoff/cellular-c12-research/work/text.compact):

- 0xcf08 reads at most 0xd94 bytes. 0xcf34 selects request index from header+12;
  0xcf40..0xcf60 branches on the sign bit of data0 (more fragments).
- 0xcfc8..0xcfe8 copies the entire first frame into its per-index buffer.
  0xcf80..0xcfa0 appends later nonfinal frames excluding their 20-byte prefix.
- 0xd1ac..0xd1cc appends the final fragment excluding the same prefix;
  0xd3a0 copies its header and 0xd3a8 resets assembly state before dispatch.
- The per-index stride is 0x4014 (0xcf48); GetPackInfo bounds the argument
  stream, including argc, to 0x4000 (0xd468..0xd4c0). Thus the complete buffer
  bound is 20 + 0x4000, not the 3584-byte transport receive buffer size.

Implementation keeps one partial job per five vendor indices, owned by the
single NAPI producer. The first frame claims the existing index and pending
budget; all continuations append into its fixed-capacity allocation. They must
agree on channel, index and operation and have valid lengths/flag bits. The
final combined stream must pass the strict existing argument parser before it
is queued. The internal header is normalized to describe the complete stream;
transport sequence remains separate from request identity. No partial request
can call a filesystem provider. Payloads and provisioning paths are not logged.
A numeric completion log records only operation and combined size.

Partial jobs and queued jobs share the existing six-job bound (five active
indices plus one published callback finishing). Missing final fragments retain
a bounded slot until teardown; this change adds no retry or fragment timeout.
Malformed appended streams quiesce transport instead of treating their next
tail as a new operation. NAPI is disabled before teardown frees partial jobs,
drains queued work and releases provider handles. Final reply publication
continues to release its request index inside the existing TX/STOP lock.

Tests execute the actual codec and admission/worker/destroy code: a synthetic
3972-byte WRITE gives exactly 3476+560-byte frames; five indices interleave;
every valid two-frame split is checked, including argument length boundaries.
Maximum stream capacity, one-byte final tail, changed operation/index/channel,
invalid flags/lengths, overflow, malformed final arguments, allocation failure
and teardown are exercised. Existing service and TX/STOP/RPC tests also run.

This does not implement persistent provisioning writes, missing directory and
move operations, IPC services, network registration or data configuration.
