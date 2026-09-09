# Disabled SA Query prerequisite

The station can initiate an SA Query after an applicable suspicious plaintext
disconnect, match authenticated responses, and answer authenticated SA Query
requests from its current AP. Production still never sets
`pmf_negotiated`, so PMF/SAE advertisement and admission remain disabled. No
firmware/device qualification or working WPA3 connection is claimed.

The production receive path requires unicast CCMP integrity metadata, current
AP/BSS/WLAN/key/WTBL ownership, a fresh management PN, category 8, action 0/1,
and both transaction-ID bytes. Only a request schedules a response. There is
one pending ID plus at most one response in flight; a queued ID is not replaced
by a burst, while a new request during TX can schedule the next invocation.

The management worker pins connection/key lifetime with `connection_lock` and
revalidates PMF, key readiness and association/WTBL ownership before sending.
It echoes the two-byte ID in a 28-byte unicast SA Query response. The existing
GEN3 management encoder now has an explicit protected-SA-Query mode: both the
802.11 Protected bit and the descriptor protection bit are required/set.
Generic authentication and association mode still rejects protected actions.
Association explicitly zero-initializes the extended TX parameter structure.

A separate plaintext classifier admits only native unicast deauthentication or
disassociation with reason 6/7 and cipher mode zero. It allows the GEN3
cipher-mismatch indication, while other descriptor errors, protected frames,
fragments and incomplete bodies are rejected. The owner additionally requires
the current AP/BSS/WLAN/destination, PMF, installed ready keys and an authorized
port. This input can start validation; it never directly authenticates a peer
disconnect or advances a replay counter.

Initiation records the association/WTBL owners, a new query generation and a
fixed 1000 TU (1024 ms) deadline. Repeated triggers cannot extend it. At most five
attempts occur, separated by 201 TU (205824 us), rounded up to workqueue timer
ticks. A ten-second start cooldown follows the supplicant's denial-of-service
limit. IDs come from `get_random_u32()` and are unique within the bounded query,
including the unlikely case of repeated random candidates. A response may match
any submitted ID in the current generation; it must first pass the production
CCMP integrity/owner/replay path and arrive before the deadline. IDs are installed
before submission to handle early responses, and removed on proved-not-sent
failures. Expired or completed generations cannot submit a stale retry after
waiting for the transport lock.

Submission uses the existing PID owner, CPU queue credits, lifecycle lock and
TX-done codec. PID, WLAN index and sequence number must match. Completion can
arrive before send returns; a later completion cannot overwrite a latched
malformed-event failure. A matched failed TX status safely releases ownership.
Credit `-EAGAIN` means no submission and drops the response so the AP can retry.
For our own query, that result removes the unsent ID and schedules the next
bounded attempt. A normal query deadline expires through local disconnect with
reason `PREV_AUTH_NOT_VALID`, not the unauthenticated trigger's asserted reason.
Ambiguous transport, abort or missing TX completion terminally fence ownership
and the firmware lifetime. TX completion waits are bounded by one second and,
for an initiating request, by the remaining query time. An authenticated response
does not substitute for TX-done when retiring the firmware PID.

Timer state and matching use `radio->lock`; each timer invocation acquires
`connection_lock` before looking up the current connection. Key removal,
deauthorization, explicit/protected disconnect, HIF abort and cleanup retire the
query and cancel delayed work without blocking while holding those locks. A
callback already running then sees current connection/state after taking the
mutex. Radio stop/unregister synchronously cancel delayed work outside that
mutex and flush management work before releasing the radio. Scheduler/lock
delays can defer processing, but the absolute deadline still gates response
acceptance and new submissions.

`test-wifi-sa-query-host.py` exercises actual production RX, worker, frame and
descriptor construction, PID lifecycle, event matcher and error handling with
synthetic transport/workqueue primitives under ASan/UBSan. It imports the vendor
descriptor structure/protection mask to verify the offset and bit. It also runs
the production PMF integrity/replay and key-lifecycle tests. Deterministic time,
randomness and transport boundaries exercise both trigger subtypes/reasons,
all rejected status bits, fixed deadlines, five bounded attempts, ID collisions,
earlier/early/expired responses, no-credit rollback, timeout policy, key/owner
changes, cleanup, and an obsolete worker returning after a new query starts.
Association, join,
HIF association, radio worker/connect/key/link/data/admission regressions pass.
The modified radio, RX, management-TX and AIS-HIF objects compile for arm64 with
the accepted M25 kernel configuration. Host shims do not model IRQ concurrency,
kernel workqueue scheduling, firmware encryption or over-the-air delivery.

Source basis:

- Vendor GEN3 `mgmt/rsn.c:rsnSaQueryRequest()` echoes the transaction ID, sets
  the Protected frame bit and requests `MSDU_OPT_PROTECTED_FRAME`.
- Vendor `mgmt/saa_fsm.c:saaChkDeauthfrmParamHandler()` and its disassociation
  counterpart select unicast reasons 6/7; `rsnStartSaQueryTimer()`,
  `rsnCheckSaQueryTimeout()` and `rsnSaQueryAction()` provide retry/timeout and
  earlier-ID matching evidence. Fixed storage replaces the vendor's allocation
  on every retry.
- The primary
  [supplicant SME source](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/9839ecd75c832023d4d13fd2917a8c28261ff668/wpa_supplicant/sme.c)
  supplies the completed-connection gate, ten-second start limit and local
  timeout reason, consistent with its 201/1000 TU retry/deadline policy.
- Vendor `nic/nic_tx.c:nicTxFillDescByPktOption()` and
  `include/nic/nic_tx.h` set bit 7 in `ucHeaderPadding` (descriptor offset 6).
- Mainline `net/mac80211/rx.c:ieee80211_process_sa_query_req()` checks current AP,
  own unicast destination and minimum action size before echoing the ID. An
  accessible primary-source version is
  [Linux mac80211 RX](https://android.googlesource.com/kernel/common/+/b77587ac51d2fe4b9d5751662ddc083d19153662/net/mac80211/rx.c).

Remaining prerequisites include other robust-action handling, IGTK overlap/rekey/historical
reinstall policy, PMF negotiation/capabilities and SAE external authentication.
The deployed blob must still be shown to encrypt this management TX using the
current PTK and a monotonic PN, and to return the expected TX sequence metadata.
A TX-done event alone proves neither encryption nor AP acceptance. Earlier RX
MMIE retention and CCMP metadata assumptions remain unverified. Plaintext trigger
delivery with the expected BSS/pairwise-WLAN metadata also requires validation;
this driver does not infer a missing WTBL index from untrusted addresses.
