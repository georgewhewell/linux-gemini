# C10 next service: confined GETATTRIBUTES

Based on C10 source `b40165b6b30b93cf68f641733cc81c554239f237`.
C10 boot `7d01a34f-14d1-4636-95d7-a7bc0403ca52` enumerated eMMC,
mounted real nvdata read-only without journal replay, registered authority,
and queued a successful 120-byte GETDISKINFO reply at90.499997. It next sent
a complete68-byte queue1/channel14 GETATTRIBUTES request (`0x1010`) for a
direct child of Z. No HS2 arrived; the existing AP timer fired121.856432.
Only attributed dmesg was inspected. The accidentally overbroad C10 archive
and its provision subtree must not be traversed or used for this work.

## Installed vendor contract

Installed Android25 `ccci_fsd` SHA256
`688e9f4d3bca872b8d6db883fa4b268e57e40e212fe4d6cdc292da3c8b836cdf`
was inspected statically, never executed. Dispatcher table base0xd54c,
slot15 at0xd588 targets0x62f4. The Z branch723c selects root0 and889c
joins the configured root and supplied child. At8988 it calls11980:
stat119a4 reads metadata, masks219/292 at119b8/119c0 identify read-only
permissions. The type helper11b08 is called8998–89a8; directory type0x4000
adds attribute0x10. Negative results remain signed. Result89c0→a9c4→d7c0
is **one four-byte argument**, not status plus a second attribute field.
Response channel15/op`0xffff1010`, total32 bytes, original request index.

At11a8c–11ab0 errno24/13/2 map to -5/-16/-9; other errors map to -18.
Successful attributes are bit0 iff `(mode & 0333)==0 && (mode & 0444)!=0`,
plus bit0x10 for a directory. The provider returns actual mode, not an
invented ENOENT or an attribute inferred merely from the RO mount.

## Bounded authority and concurrency

The parser supports only printable-ASCII UTF16LE direct children of `Z:\`:
nonempty basename at most255 bytes; no embedded NUL, separators, colon,
dot/dot-dot, alternate drive or non-ASCII encoding. Nested or otherwise
unsupported requests stay in the existing bounded capture; no canned reply.

NAPI copies the validated request to an ordered workqueue. The provider takes
a file reference to the consumed, real RO `/md` authority under its lock,
then performs `lookup_one_positive_unlocked` and `vfs_getattr` in process
context with no provider spinlock held. A single component cannot escape
the root or follow a mount/symlink. Covered mountpoints, symlinks, special
inodes and other superblocks are refused. RO is rechecked after lookup/stat.
No file contents, directory listing, writes, provisioning or repair are used.

Five active request-index bits reject ambiguous duplicates, including a
GETDISKINFO collision with pending GETATTRIBUTES. Successful TX releases the
index under `tx_stop_lock` after HWO publication and before RESUME. Incoming
index observation/acquisition uses the same lock, so it cannot see the HWO
midpoint. Worker success cleanup does not clear a newly reused index.
Failures release only their un-published ownership. At most six job objects
exist: five active wire slots plus a published callback finishing.

STOP's failed latch prevents worker replies through the existing TX lock.
Teardown disables NAPI, issues orderly STOP, then drains/destroys the queue
before devres frees rings. Queued stopped work performs no lookup; an already
running metadata lookup may finish but cannot publish after STOP. As with
existing synchronous teardown, stuck backing I/O has no invented software
completion guarantee; original watchdog and absolute recovery limits remain.
The literal30-second queue-start timer, DT and firmware MPU policy are unchanged.

## Actual tests

- `test-cellular-fs-getattributes.py`: literal68/32 byte shapes, malformed and
  escape cases, all32768 low mode combinations, errno mappings, real getter
  body with VFS adapters, retained-reference/error/RO/mount/symlink cleanup.
- `test-cellular-fs-attr-worker.py`: actual queue/worker/destroy bodies,
  immutable RX copy,32 concurrent admissions/duplicate refusals, failure
  cleanup, stopped/drained work and immediate same-index reentry. TX and VFS
  are deterministic adapters here, not a claim of hardware execution.
- `test-cellular-rpc-transport.py`: separate actual TX/STOP bodies with
  pthread lock adapters,32-byte response/sequence/index preservation,
  publish-only release, shared FS collisions and concurrent incoming index
  observation inside publication; existing40/120-byte paths retained.

Existing FS registration/start-gate, first-fatal, terminal and NAPI tests pass.
ARM64 provider/CCCI object compilation passed. This is source validation,
not a claim of HS2/AT success and not authorization for another device boot.
