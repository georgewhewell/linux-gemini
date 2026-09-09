# Disabled SA Query responder prerequisite

The station can now prepare and submit a protected response to an authenticated
SA Query request from its current AP. Production still never sets
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

Submission uses the existing PID owner, CPU queue credits, lifecycle lock and
TX-done codec. PID, WLAN index and sequence number must match. Completion can
arrive before send returns; a later completion cannot overwrite a latched
malformed-event failure. A matched failed TX status safely releases ownership.
Credit `-EAGAIN` means no submission and drops the response so the AP can retry.
Ambiguous transport, abort or missing completion after a bounded one-second
wait terminally fence ownership and the firmware lifetime. Radio stop/unregister
flush the management worker before releasing the radio.

`test-wifi-sa-query-host.py` exercises actual production RX, worker, frame and
descriptor construction, PID lifecycle, event matcher and error handling with
synthetic transport/workqueue primitives under ASan/UBSan. It imports the vendor
descriptor structure/protection mask to verify the offset and bit. It also runs
the production PMF integrity/replay and key-lifecycle tests. Association, join,
HIF association, radio worker/connect/key/link/data/admission regressions pass.
The modified radio, RX, management-TX and AIS-HIF objects compile for arm64 with
the accepted M25 kernel configuration. Host shims do not model IRQ concurrency,
kernel workqueue scheduling, firmware encryption or over-the-air delivery.

Source basis:

- Vendor GEN3 `mgmt/rsn.c:rsnSaQueryRequest()` echoes the transaction ID, sets
  the Protected frame bit and requests `MSDU_OPT_PROTECTED_FRAME`.
- Vendor `nic/nic_tx.c:nicTxFillDescByPktOption()` and
  `include/nic/nic_tx.h` set bit 7 in `ucHeaderPadding` (descriptor offset 6).
- Mainline `net/mac80211/rx.c:ieee80211_process_sa_query_req()` checks current AP,
  own unicast destination and minimum action size before echoing the ID. An
  accessible primary-source version is
  [Linux mac80211 RX](https://android.googlesource.com/kernel/common/+/b77587ac51d2fe4b9d5751662ddc083d19153662/net/mac80211/rx.c).

Remaining prerequisites include initiating SA Query for appropriate suspicious
plaintext disconnects, transaction-ID generation/matching, retry and overall
timeout policy, other robust-action handling, IGTK overlap/rekey/historical
reinstall policy, PMF negotiation/capabilities and SAE external authentication.
The deployed blob must still be shown to encrypt this management TX using the
current PTK and a monotonic PN, and to return the expected TX sequence metadata.
A TX-done event alone proves neither encryption nor AP acceptance. Earlier RX
MMIE retention and CCMP metadata assumptions remain unverified.
