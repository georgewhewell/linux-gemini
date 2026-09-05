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
not cellular attachment/data or overall handshake success. The single installed
test returned no-HS2; no AT, host validator or success marker ran.
Complete selected RAM archive SHA256
98db8587ec4e048a9213a68cf0c4f01beee85b27b22156ee554b957eb082486a
was saved before petter stop at139.67 uptime. Live DT SHA256
f34b3aa47fd6e392daf14cca6eaa37aee8ce46d9cec8ecc10902c03406e5da47.

Recovery fully closed at07:13:49 with exact accepted rescue boot
671e5fa7-d61d-477d-bfc5-f0465df05182 on USB and RJ45, all three partition
hashes matching, no failed units, and all transaction processes/locks released.
Attributed pstore SHA256
f72c12919fd75ef5226c860eb00002648c10e6616ca93d343d1f05ac24c30c67
matches the harness archive and contains the139.712286 candidate marker.
The image is consumed. No retry or filesystem/provisioning mutation is
authorized by this result.

Accepted rescue's independently captured watchdog baseline is mtk-wdt,
inactive, NOWAYOUT0, timeout31, no FD owner/service, and systemd runtime
watchdog disabled. Do not impose the experimental C/R active-NOWAYOUT
petter contract on this accepted rescue. No watchdog setting was changed.
