# MT6797 pre-ready receive evidence

Source base: 67ac10f8c60db237eac7d22ebb7daac6b6f4a4cc. This is diagnostic
capture, not a new modem response, reset, retry, permission change, or HS2 fix.

C5 boot 8a5db888-5752-4d8a-8245-1e4e1bebd65e sent the corrected 636-byte,
64-record runtime response. The saved first-fatal record showed one Q0 TX
submitted and reclaimed, a later RX queue-1 interrupt, and the AP handshake
timer expiry rather than C4's MD watchdog IRQ. Queue-1 interrupt status does
not identify the packet channel, opcode, or why the modem did not send HS2.
The existing dispatch drops non-control traffic before READY.

## Ownership and bounds

The first four pre-ready non-control drops (plus malformed header/channel
boundaries) retain at most 1024 bytes each, with original length (maximum
3584), explicit truncation, queue, decode error and drop reason. Drop reasons
are 1 bad header, 2 bad channel, 3 before READY, 4 bad sequence. A valid header
means the existing header decoder succeeded; it does not mean the service
payload has been validated. `payload_word0` is merely bytes 16..19 decoded
little-endian, never an inferred operation for an invalid/short header.
Ordinary CONTROL_RX remains in the existing handshake capture, not this cap.

RX buffers come from the linear netdev skb allocator. `rx_poll` unmaps their
DMA_FROM_DEVICE mapping before setting length and dispatching; dispatch owns
the skb through capture and its existing release. No device-owned DMA buffer
is dereferenced by this new capture. All metadata and byte copies use the
existing IRQ-safe diagnostic lock; no allocation or register reads occur.
First-fatal freezing precedes logging and destructive STOP. A bad-sequence
packet is copied before that path freezes the record. Subsequent capture or
failure calls cannot change or print it again. Diagnostic code never takes
the TX lock. Existing teardown IRQ/work draining and devres lifetime apply.

The added output is one count line, at most four metadata lines and 128 hex
lines (4096 retained packet bytes), once on terminal failure; it fits well
inside the oneboot 1-MiB log budget. It does not promise whole-boot log
retention. These bytes may contain modem RPC, filesystem or provisioning
data: retain captures as owner-restricted evidence; do not publish raw logs.
No extra render, modem request, MMIO access, W1C, retry or reset is introduced.

## Vendor bootstrap routing: what a captured request can establish

Evidence is the checked-out vendor kernel commit
8cfe6596a503612e3332d9c26e292a19525a7f07, under
`drivers/misc/mediatek/eccci/`:

* `port_cfg.c` routes FS, RPC, UART and IPC-related ports through queue 1;
  queue 1 alone cannot establish RPC. `ccci_core.h` defines FS_RX=14 and
  RPC_RX=32. FS uses character-port operations (`ccci_fs`), not the kernel
  RPC worker. `port_char.c` accounts for an additional FS word when stripping
  its header. Filesystem success or provisioning data cannot be fabricated.
* `port_rpc.c:rpc_msg_handler` interprets a 16-byte CCCI header followed by
  operation and parameter count, then length-prefixed arguments padded to
  four bytes. `port_rpc.h` gives two request indices, six maximum arguments,
  2048-byte RPC buffer and response operation mask 0xffff0000. A future
  implementation must validate every actual packet bound and arithmetic;
  these vendor constants are not permission to copy unsafe parser behavior.
* `port_rpc_recv_match` routes ordinary RPC operations to the kernel worker;
  selected optional LGE operations go to userspace. The worker changes the
  response channel to RPC_TX and updates its full length before sending.
* `port_rpc.h` identifies GPIO value 0x4006, ADC value 0x4007, RF clock buffer
  0x4008 and combined GPIO/ADC 0x4009. GPIO/ADC/name requests need genuine
  platform providers, not canned success. Secure operations 0x2001/0x2002
  and filesystem/provisioning traffic similarly need their real ownership.
* `port_rpc.c` operation DSP_EMI_MPU_SETTING (0x400b) also invokes
  `port_proxy_set_md_dsp_protection(..., 1)`. It is not just a harmless reply:
  any support needs independently validated DSP memory/permission ownership.

Until a captured channel and bounded payload establish the actual request,
no particular service or response is justified. The separately documented
version-1/1-MiB AP MPU advertisement mismatch remains unfixed here; this
candidate does not expand the MD1-MD3 carveout or change its permissions.

## Validation

`test-cellular-first-fatal.py` executes the actual capture helpers and the
actual pre-ready dispatch prefix plus header decoder. It covers malformed
15-byte headers without reading uninitialized channel state, control
exclusion, invalid channel, raw payload word, pre-freeze sequence failure,
3584/3585 boundary, truncation, null/queue/cause refusal, four-slot limit,
immutable copies and concurrent capture/freeze. Existing first-fatal and
terminal-latch tests remain required. ARM64 `mtk_mt6797_ccci.o` cross-compiles
with GCC 12.2. No hardware test has been performed for this change.
