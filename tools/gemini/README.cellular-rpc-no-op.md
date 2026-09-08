# Complete unsupported RPC requests with the vendor error reply

CI5's bounded EFUN3 test on boot2fd7c607-1a58-4226-8818-572c7765427b
still reports SIM busy and refuses EFUN0. Unlike earlier fatal-only captures,
the numeric timeline shows the next unanswered RPC: channel32/queue1,
operation0x400a, 32 bytes, error-EOPNOTSUPP. The FS worker finishes all14
new requests (including X/Y reads and Z writes), total2370/pending0/failed0.
OPEN status1 is a successful file handle, not an error. No MOVE request
appears in this interval. Provisioning source hashes remain unchanged.

Vendor source8cfe6596a503612e3332d9c26e292a19525a7f07 identifies0x400a as
IPC_RPC_USIM2NFC_OP. Its handler is under FEATURE_INFORM_NFC_VSIM_CHANGE;
there is no definition of that macro in the checked-out headers/Makefiles,
and the saved Gemini config disables CONFIG_NFC and CONFIG_MTK_NFC.
The vendor rpc_msg_handler default case returns FS_NO_OP=-1 in ONE u32
argument. It does not silently discard an unsupported operation. The reply
is32 bytes (CCCI16 + operation/count8 + length/value8), with operation
OR0xffff0000. This is an explicit failure response, not simulated NFC success
or permission to touch VSIM/NFC power.

The common RPC codec now validates the entire bounded request and builds
that32-byte error response for operations lacking a provider. It rejects
response IDs, invalid slots, truncated/overflowing arguments and trailing
bytes. The worker still uses its bounded immutable queue and publication
ownership; supported EINT/RF operations retain their real handlers. Unknown
operations are logged with status-1 and actual TX result, preserving evidence
that the feature remains unsupported while allowing the modem to consume
its protocol response.

The non-EINT diagnostic log has a separate rate-limit call site so startup
EINT messages cannot suppress the first RF-clock result. CI5's missing4008
rejection is observed, but its successful reply was not directly printed
because the old shared rate limiter suppressed it. Do not turn that absence
alone into a direct observation of an RF reply.

Actual codec tests compare a literal NFC-disabled32-byte request/reply,
all truncations and capacity limits, argument overflow, response-ID refusal,
zero through six arguments, padding and absence of provider effects. The
actual worker/TX fixture confirms unsupported requests publish the error
reply and release their index. Existing EINT and RF tests pass. ARM64 W=1
CCCI object compiles. This change has not been packaged or booted yet.

The test also records unhandled channel38 (IPC_UART_RX) and channel34
(IPC_RX) traffic. Earlier idle rejections are channel57 (IMSA_DL) and channel4
(PCM_RX). Their requirements remain to investigate if SIM initialization
still stalls after the error reply. No payload from those channels is logged.
