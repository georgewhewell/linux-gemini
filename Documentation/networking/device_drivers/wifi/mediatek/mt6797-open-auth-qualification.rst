.. SPDX-License-Identifier: GPL-2.0-only

===========================================
MT6797 one-shot open-auth qualification gate
===========================================

Status
======

This tree contains the bounded AIS HIF transaction, the separately owned
passive-scan HIF precursor, and a dormant qualification caller.  The caller is
compiled out by default, probe never invokes it, and this source change is not
a bootable open-auth candidate.

No AP identity or credential is built into the driver.  Candidate packaging
must still supply an independently reviewed AP manifest, exact source
provenance, Stage2 transcript handling, and the accepted rollback supervisor.

Implemented precursor boundary
==============================

``mt6797_wifi_ais_hif_start()`` accepts only a generation- and cookie-valid
``mt6797_wifi_scan_target``.  That target can currently be populated only by
``mt6797_wifi_scan_target_observe()``, which validates the complete firmware RX
packet, including the RX descriptor and group-3 RCPI metadata.

``struct mt6797_wifi_rx_view`` now carries both the stripped ``payload`` and a
synchronous borrowed ``packet`` pointer to the complete firmware RX packet.
The pointer is valid only through the runtime sink call and is never retained.
``struct mt6797_wifi_scan_hif`` owns one passive-scan transaction and one scan
target owner.  It attaches before command publication, accepts only the
explicit key and channel, copies the validated target, arbitrates completion
versus timeout under one lock, sends at most one best-effort cancel, and
detaches before returning.

The precursor itself does not call ``mt6797_wifi_ais_hif_open_auth()``, expose
a userspace trigger, select an AP manifest, or make a bootable candidate.
Constructing a target from command-line BSSID/channel fields, or writing the
private ``selected`` member directly, remains prohibited because it would
replace received evidence with reviewed intent and bypass the anti-stale
contract.

Precursor invariants
====================

The separately reviewable scan-HIF boundary:

1. preserves a synchronous borrowed view of the complete RX packet through
   the runtime sink, or supplies equivalently validated owned metadata;
2. owns exactly one ``mt6797_wifi_scan_transaction`` and one
   ``mt6797_wifi_scan_target_owner``;
3. attaches its sink before publishing the passive-scan command, scans only
   the reviewed band/channel, and accepts only the reviewed BSSID and nonempty
   SSID;
4. uses a completion plus a short lock to arbitrate scan-done, target frame,
   timeout, terminal abort, and detach;
5. sends at most one best-effort scan-cancel on timeout, then fences ambiguous
   state; and
6. returns an owned target snapshot only while its generation, cookie, and age
   remain valid.

Its fixtures cover a target frame before/after scan-done, foreign and hidden
frames, malformed metadata, timeout versus late completion, terminal abort,
sink-detach synchronization, and stale target rejection.  ARM64 ``W=1`` must
compile both its production and KUnit objects before integration.

Candidate-only trigger ABI
==========================

The caller is compiled only under
``CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION``, which defaults to ``n`` and
depends on the built-in MT6797 endpoint.  No qualification attribute exists
when it is disabled.  Enabling it also requires
``CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_SOURCE_REVISION`` to contain the exact
40-character lowercase hexadecimal candidate source commit; otherwise probe
fails before acquiring WLAN resources.  Merely probing a valid candidate
never runs the gate.

The candidate exposes the write-only ``open_auth_qualification`` and read-only
``open_auth_qualification_result`` device attributes.  The write accepts only
this exact ordered, newline-terminated format::

   schema=mt6797-open-auth-ap-v1
   ap_manifest_sha256=<64-lowercase-hex, not all zero>
   authentication=open
   credential=none
   ssid_hex=<2..64 lowercase hex digits, not an all-zero SSID>
   bssid=<lowercase unicast xx:xx:xx:xx:xx:xx>
   band=<2ghz|5ghz>
   channel=<canonical decimal channel>
   basic_rate=<canonical 0x1..0xfff lowercase hex>
   scan_timeout_ms=<canonical decimal 1..30000>
   auth_timeout_ms=<canonical decimal 1..4000>

For ``2ghz``, channels are 1..14.  For ``5ghz``, this bounded firmware ABI
accepts 15..199.  No leading zero is accepted in a multi-digit decimal or hex
field.  No additional line, byte, or embedded NUL is accepted.

The attribute consumes one complete command.  An attempted write atomically
changes the per-boot state from ``idle`` to ``consumed`` before parsing; every
later write returns ``-EALREADY``, including after malformed input or transport
failure.  This makes a typo fail closed and makes retry require a new boot.

The command is generated from an independently reviewed manifest and contains:

* schema ``mt6797-open-auth-ap-v1`` and its 64-hex SHA-256;
* open authentication and ``credential=none``;
* a nonempty 1..32-byte SSID encoded as lowercase hex;
* a lowercase unicast BSSID;
* one band and one channel;
* one evidenced basic rate; and
* bounded scan and authentication timeouts.

Unknown, duplicate, reordered, empty, noncanonical, multicast, secret, PSK,
passphrase, key, association, retry, or data-path fields are rejected.  Kernel
parsing is bounded and must not log the SSID or BSSID.  The manifest SHA-256 is
the only AP identifier exported in results.

One consumed command performs this sequence exactly once:

::

   passive scan -> validated target snapshot -> AIS start (STA_STATE1)
       -> one mt6797_wifi_ais_hif_open_auth() call -> AIS stop

AIS stop is mandatory after both a clean success and a clean AP rejection.  A
terminal or ambiguous HIF error remains fenced.  No association request,
controlled-port/key operation, cfg80211 registration, netdev registration, or
data frame is part of this state machine.

Result and transcript ABI
=========================

After the synchronous consumed operation finishes, the result attribute is
immutable for the rest of the boot.  Before that it returns ``-ENODATA``.  The
kernel does not write files.  Candidate Stage2 reads the result once and
creates, as root using a temporary file plus atomic rename,
``/run/mt6797-wifi-open-auth.transcript``.  The file is not group- or
other-writable and has exactly these eleven ordered lines::

   schema=mt6797-open-auth-transcript-v1
   ap_manifest_sha256=<64-lowercase-hex>
   source_revision=<40-lowercase-hex>
   library_revision=2597a1100741e9f87debd4e80038371c7c7ecd17
   attempts=1
   operation=open-auth
   retry=none
   association=not-attempted
   key_install=not-attempted
   data_path=not-attempted
   result=success errno=0 tx_done=yes response_received=yes

A failed operation uses the same ordering with ``result=failure``, its negative
errno, truthful ``yes``/``no`` completion fields, and ``attempts=0`` when the
authentication call was never reached.  A command rejected during parsing uses
``ap_manifest_sha256=invalid`` rather than presenting unvalidated bytes as an
AP identity.  Stage2 must accept only ``attempts=1``, a valid 64-hex AP digest,
and the exact success line above.

``retry=none`` means this caller never resubmits the scan or authentication
transaction.  It does not assert or qualify lower firmware/MAC retransmission
behavior.

The source revision comes from candidate build provenance, not a writable
runtime parameter.  Stage2 creates the success transcript only when the
immutable kernel result proves one transmitted request, its owned TX
completion, and its matching successful open-auth response.  All other
results fail the hardware gate and remain diagnosable in the candidate-side
log; they must not be rewritten as success.

Recovery and nonclaims
======================

The only permitted hardware use is an explicitly scheduled p22
``observe-always-rollback`` transaction whose supervisor is already armed.
The rollback image is exactly ``/mnt/Home/src/a72-known-good-p22.img``, size
16777216, SHA-256
``1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc``.

Passing this gate would claim only one open-auth request/response exchange
against the reviewed AP.  It would not claim association, key installation,
WPA/WPA3, a controlled port, a data path, DHCP, general connectivity, or retry
correctness.
