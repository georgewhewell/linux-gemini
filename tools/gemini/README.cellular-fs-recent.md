# Recent filesystem metadata at first fatal

C15 advances beyond the size-only COMPACT_READ rejection but ends at modem
watchdog, before HS2. The per-request printk cap of128 hides later operations.
Do not infer that the last printed request is the last serviced request.

The driver now keeps16 numeric request entries and a total counter. The ordered
FS worker records request start before allocation/service, and completion after
TX. The first fatal event freezes this ring under its own irqsave spinlock;
printing occurs after releasing the lock. No queue/STOP lock is acquired while
holding the diagnostic lock. No new per-request printk is added.

CCCI_FS_RECENT reports chronological sequence, operation, parser error, whether
COMPACT_READ shape was valid, its flags and requested length, completion state,
FS status, transport/service return and bytes sent. No path, file contents,
file handle, or modem pointer is retained. requested is the raw control field;
for size-only flags0x13 it is not an actual read request.

done=0 means first-fatal interrupted the request before diagnostic completion;
status/result/sent then remain zero and must not be interpreted as success.
The total counts requests started by the worker, not frames admitted by NAPI.
The existing pending count includes queued/in-flight jobs. An internal TX fatal
can freeze before the worker returns its error; done=0 is correct in that case.
Requests beyond first-fatal cannot mutate the snapshot. Removal drains the
worker as before; the ring belongs to the existing device allocation.

Tests execute the actual header and FS worker:140 requests/last16 chronology,
freeze between begin/end, immutable post-freeze writes, compact numeric shape,
malformed short input, and existing allocation/TX/STOP/concurrent admission
cases. ARM64 W=1 compile passes. Hardware result remains pending.

C16 hardware captured1132 requests, pending0; final two OPEN operations returned
vendor-18 with successful32-byte replies. All prior retained reads completed.
To distinguish write-intent OPEN from other general failures, the next revision
adds open_valid/open_flags for shape-valid OPEN/OPENHINT and COMPACT_READ.
These are numeric flags only. No access policy changes. Host tests check the
literal C11 OPEN0x900 and composite read0x100 flags; short malformed inputs do
not set open_valid. No next-revision hardware result yet.


## Read-only live snapshot after CI3 radio stalls

The modem can remain AT-responsive with SIM busy without entering FIRST_FATAL.
A root-only `fs_recent` attribute on the CCCI platform device now provides a
chronological snapshot of the existing 16-entry numeric ring. Reading copies
under its IRQ-safe lock, then formats after releasing it; it does not freeze,
clear, or otherwise change the ring, touch MMIO, or submit modem work. The
snapshot is detached from subsequent completions and preserves the current
in-flight row. Ready/failed/pending are separate atomic samples, not a promise
that all transport fields were sampled in the same instant as the ring.

The driver core adds/removes the device attribute group with driver binding.
Mode 0400 permits root reads; there is no store callback. It contains operation,
result and count metadata, ASCII drive numbers and numeric open/MOVE flags.
It never contains path strings, file data, file handles or modem addresses.
MOVE flags are captured only for exactly three parsed arguments with a
four-byte final flag argument; the validity bit distinguishes absent metadata.
They are also included in the existing fatal printout.

Host tests execute the actual snapshot and sysfs formatter. They check ring
wrap order, detached/in-flight/frozen copies, absence of ring mutation, output
fitting one sysfs page for sixteen maximum-width entries, allocation failure,
and the existing worker/admission/freezing cases. An ARM64 W=1 compile checks
kernel APIs. This diagnostic is not yet packaged or hardware-qualified; CI3
continues using its pinned earlier kernel until a new candidate is built.


The separate root-only `rx_rejected` attribute covers RPC/FS requests refused
before worker admission and post-HS2 channels the transport does not handle.
These previously could disappear without reaching the FS worker ring. It
retains sixteen chronological numeric rows (channel, queue, byte count, error,
and an operation word only for RPC/FS when at least20bytes are present).
Header-only unhandled channels are recorded before data-payload validation.
No request is newly answered or admitted; rejection behavior remains intact.

Its IRQ-safe ring and detached snapshot are independent of the FS worker ring,
so a NAPI rejection cannot displace an in-flight worker's completion slot.
Host tests execute capture, concurrent snapshots and the actual sysfs formatter,
with truncated and unknown-channel operation fields, wrap order, immutable
copies, full-width output bounds and allocation failure. Wiring checks verify
both RPC/FS rejection paths and header-only unhandled-channel ordering.
