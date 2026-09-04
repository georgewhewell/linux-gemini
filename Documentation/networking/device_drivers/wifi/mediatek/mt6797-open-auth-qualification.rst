.. SPDX-License-Identifier: GPL-2.0-only

===========================================
MT6797 one-shot open-auth qualification gate
===========================================

Status
======

This document remains a design contract, not an implemented trigger.  The tree
contains the bounded AIS HIF open-auth transaction and its separately owned
passive-scan HIF precursor, but intentionally has no production caller.  It is
not a bootable open-auth candidate.

A caller must not be added directly to ``core.c``.  It must first satisfy the
candidate-only trigger and transcript contract below.

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

This precursor does not call ``mt6797_wifi_ais_hif_open_auth()``, expose a
userspace trigger, select an AP manifest, or make a bootable candidate.
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

After that precursor exists, the smallest acceptable production caller is a
candidate-only, write-once device attribute.  It must be compiled only under a
new ``CONFIG_MTK_MT6797_WIFI_OPEN_AUTH_QUALIFICATION`` option which defaults to
``n`` and depends on the built-in MT6797 endpoint.  No attribute is registered
when the option is disabled.  Merely probing the endpoint must never run the
gate.

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

Unknown, duplicate, empty, noncanonical, multicast, secret, PSK, passphrase,
key, association, retry, or data-path fields are rejected.  Kernel parsing is
bounded and must not log the SSID or BSSID.  The manifest SHA-256 is the only AP
identifier exported in results.

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

The kernel must expose a read-only, immutable result after the consumed
operation finishes.  It must not write files from kernel context.  Candidate
Stage2 reads that result once and creates, as root using a temporary file plus
atomic rename, ``/run/mt6797-wifi-open-auth.transcript``.  The file is not
group- or other-writable and has exactly these eleven ordered lines::

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
