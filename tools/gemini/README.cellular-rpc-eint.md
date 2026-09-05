# MT6797 SIM EINT RPC service

Source base: dbf46a0e0f947 (reviewed C6 kernel 833a23 plus host-only decoder).
C6 boot 1605f91c-98bd-440e-a6f6-b7ab0319a918 captured a complete 68-byte,
channel-32, queue-1 RPC 0x4005 request with three arguments and request index
zero. It requests SIM1 EINT source-selector attribute 6. The request was
dropped before READY, followed by the AP handshake timeout without HS2.
This is direct evidence of a missing startup service, not proof this is the
last required service or that a reply will establish cellular connectivity.
Raw owner-restricted capture SHA-256:
f6eb539ba3f13b92db71c27e37361e090d6fc112fef8920f8a648ad27a79fcde.

## Compiled board provenance, not guessed GPIO values

The known-good working Gemian Image.gz-dtb has SHA-256
b53d191dc41d3f7364b0fa62b4bc920b1d013a1942b2e6b06727263fc56fcf4d.
Its appended DT starts at byte 8299080, size 130745, SHA-256
9e26929563f7682d1f7545d6007f0092c7e085a4edbd6e7be0ac8eaa5159b2f9.
Both SIM EINT nodes are present in that actual compiled artifact:

| Attribute | SIM1 vendor tuple | SIM2 vendor tuple | RPC values SIM1 / SIM2 |
| --- | --- | --- | --- |
| EINT number | interrupts=0,4 | interrupts=1,4 | 0 / 1 |
| Debounce | debounce=0,100000 | debounce=1,100000 | 100 / 100 ms |
| Polarity | trigger 4 (level high) | trigger 4 | 1 / 1 |
| Sensitivity | trigger 4 | trigger 4 | 0 / 0 (level) |
| Socket type | sockettype=0,0 | sockettype=1,0 | 0 / 0 |
| Dedicated enable | dedicated=0,0 | dedicated=1,0 | 0 / 0 |
| Source selector | src_pin=0,1 | src_pin=1,0 | 1 / 0 |

Vendor source commit 8cfe6596a503612e3332d9c26e292a19525a7f07 corroborates
the binary: `drivers/misc/mediatek/dws/mt6797/aeon6797_6m_n.dws` defines the
two SIM nodes; `tools/dct/obj/Md1EintObj.py` emits their tuples using
`tools/dct/config/MT6797.fig` SRC_PIN mapping. PAD_INT_SIM1/GPIO112 maps to
selector 1, PAD_INT_SIM2/GPIO113 maps to selector 0. Allowed selectors 0..2
come from that table, not a GPIO-number bound. The inspected working config
in the packaging repository's `research/vendor-kernel.config` identifies
aeon6797_6m_n as both the project and appended-DT name.

The new modem children are immutable board metadata with vendor-prefixed
scalar properties. They do not allocate AP IRQs, instantiate a pin controller,
drive GPIOs, configure debounce hardware or power a SIM. Parent modem remains
disabled by default. Probe caches each valid property independently and drops
OF references. Absent/disabled node returns QUERY_STRING=-11; a missing,
malformed or out-of-range requested property returns QUERY_TYPE=-12. The
vendor loader's accidental zero from uninitialized later properties after an
early failure is deliberately not replicated.

## Wire and lifetime contract

Vendor `eccci/port_rpc.c:rpc_msg_handler` and `port_rpc.h` define operation and
argument count after the 16-byte header, six maximum arguments and a 2048-byte
argument-buffer cap. Every length/padded span is checked against actual bytes;
no unsigned addition can wrap past the packet. Only GET_EINT_ATTR is handled.
It requires three arguments, terminal NUL with no embedded NUL, exact explicit
name length, and four-byte name-length/type arguments. Unknown node/type and
invalid semantic arguments receive the documented negative status, duplicated
in the two response arguments on failure. Unknown operations are not answered
as successes and remain in bounded pre-ready first-fatal capture.

The two-slot request index must be 0 or 1. Vendor's `>2` check is not copied:
offline interpretation explicitly documents its inconsistent index-2 boundary.
Successful responses are 40 bytes on channel 33, operation 0xffff4005, two
four-byte arguments. Actual TX preserves RPC data0 and request index while
assigning the existing per-channel sequence/assertion bits. The actual
dispatch-to-TX fixture checks a nonzero data0, request index 1 and sequence 5.

Replies execute synchronously in the existing NAPI RX owner, with one bounded
GFP_ATOMIC allocation, no new worker or queue. TX owns the response skb only
after successful publication; all failures free/unmap/rollback exactly once.
The original request is always released by existing dispatch. NAPI teardown
already drains dispatch before ring/DMA release. Reply result logs saturate
at 16 per boot and do not print raw request names or payloads.

Queue tx_lock nests a new TX-publication/STOP lock. DMA mapping/preparation,
allocation, diagnostic copies and printing remain outside the latter. A fresh
failed-latch check under it precedes descriptor publication. STOP takes only
this lock around existing mask/STOP writes, never queue tx_lock. Thus failure
before publication refuses/unwinds, while failure racing an admitted publish
waits for its resume before STOP; no new publication occurs after STOP. The
actual-code tests exercise both orders and quiesce from a held queue-lock
context. This does not claim a coherent snapshot of all device DMA state.

## Validation

Actual C `test-cellular-rpc-eint.py` covers the captured public protocol shape,
all fourteen independently literal board values, actual OF-cache field and
disabled-node handling, malformed/oversize/padding/index/type/name conditions,
unknown operation and negative-response framing. `test-cellular-rpc-transport.py`
executes actual dispatch, RPC, header initialization, TX and STOP bodies with
DMA/MMIO adapters, including allocation/map/prepare/publish failures and
deterministic concurrent terminal interleavings. Existing first-fatal and
terminal actual-code tests still pass. The transport fixture uses full
-Wall -Wextra -Werror without warning suppression.

ARM64 CCCI object and cellular DTBO compile with GCC 12.2 in
`/tmp/gemini-ccci-rpc-eint-build-uvsdqkJB`. dt-doc-validate passes the binding.
No device access, modem reset/retry, MPU/SMEM extent change, charging change or
hardware acceptance claim is included. Oneboot still must prove HS2 and AT OK.
