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
