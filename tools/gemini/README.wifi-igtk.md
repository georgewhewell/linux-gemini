# IGTK overlap and association rekey prerequisite

This change retains both host BIP-CMAC-128 receive keys, IDs 4 and 5, instead of
replacing one host slot on every install. Each ID has its own key and 48-bit
replay counter. The complete MMIE ID selects exactly one key before software
CMAC verification; neither the default-management-key selection nor a valid MIC
under the other key can substitute for that identity.

PMF negotiation and SAE remain disabled in production. This is host lifecycle
support behind the existing unreachable PMF gate, not evidence of firmware
support or working WPA3. No device deployment or hardware test was performed.

Key callbacks hold `connection_lock`; RX and publication use `radio->lock`.
All IGTK commands share the current AP group WTBL, so a common update flag gates
both BIP receive keys while an add/remove command is in flight. The existing
transport failure path fences ambiguous firmware state before forgetting keys.
There is still no IGTK-specific command completion proving installation.

The association keeps an exact history of up to 32 distinct 16-byte IGTKs,
including currently installed keys. A duplicate of the key currently installed
under the same ID is a no-op and does not resend a firmware command or change
its replay counter, even with a different supplied receive sequence. Other-ID
aliases, A-to-B-to-A replacement, and reinstall after deletion return
`-EKEYREJECTED` before any command. Fresh keys at history capacity return
`-ENOSPC`, preserving the live keys and authorization; further rekeying requires
a new association. The history is never evicted or reset by key deletion.
The fixed 512-byte history retains retired secret material for exact comparison
and is explicitly wiped with the live keys on teardown or a terminal transport
failure. Nothing logs or exposes its contents.

Both keys and their history share one captured association owner and full AP
BMC WTBL handle. Ownership checks remain active after the last installed key is
deleted, preventing a changed generation, cookie, peer or WTBL from silently
adopting the prior association's history. Fresh associations receive fresh
zero-initialized state.

Deleting an installed ID removes and wipes only its live host slot. Deleting
an absent ID returns `-ENOENT`. A surviving IGTK keeps the authorized port and
SA Query service available; deleting the last key deauthorizes and cancels the
query. An active SA Query retains its deadline/transaction IDs across a
successful IGTK overlap and retirement because its authentication uses the
unchanged PTK. Local default-management-key selection records ID 4 or 5 and is
cleared only when that selected ID is deleted. Station robust unicast TX uses
the PTK, so default selection emits no separate firmware command and does not
restrict BIP RX to one key.

Teardown detaches the RX sink before removing each installed ID, still using
the owned WTBL. Failure on either removal fences the runtime and prevents
subsequent key commands. Both live slots, history, owner snapshots, and default
selection are then wiped before the connection is released.

Validation uses the real production key callbacks, command codecs, owner
validators, PMF RX decoder/CMAC formatting, and SA Query callbacks under
AddressSanitizer and UndefinedBehaviorSanitizer. New cases cover both ID orders,
independent IPNs during overlap, default selection, old-ID retirement, same-ID
replacement, no-eviction history exhaustion, stale ownership after all live keys
are deleted, both teardown failures, and transport abort/failure with both keys
present. Independent OpenSSL CLI CMAC vectors exercise two simultaneously
installed keys, retired-key rejection, and wrong-ID/correct-other-key rejection.
Injected RX during add/remove verifies the common update gate. SA Query tests
verify continued response service and active-query completion after old-key
retirement. Transport, locks and scheduling remain synthetic and do not model
real IRQ concurrency or firmware cryptography. Existing association/join/HIF
join and radio worker/connect/key/link/data/admission regressions pass.
`radio.o` compiles for ARM64 with the accepted M25 kernel configuration.

Firmware enablement blockers remain:

- Vendor GEN3 `common/wlan_oid.c:wlanoidSetAddKey()` sets cipher 6, keeps the
  full IGTK ID, and uses `prStaRec->ucBMCWlanIndex`. It does not prove that the
  deployed blob stores IDs 4 and 5 simultaneously or independently of the GTK.
  The dedicated `WTBL_AIS_BIP_IDX` branch in `mgmt/privacy.c` is inactive
  preprocessor code and is not a basis for allocating another WTBL.
- Vendor removal preserves the full key ID but provides no proof that deleting
  either IGTK leaves the other IGTK and GTK intact. Current host commands encode
  the owned AP group WTBL and requested ID; their slot isolation must be tested
  against the actual blob before enabling this path.
- Software CMAC cannot recover a frame that firmware drops or marks with an
  integrity error because it retained only the newest key. Both-key RX delivery,
  intact MMIE retention, independent IPNs, same-ID replacement, and add/delete
  isolation therefore need controlled firmware evidence. This implementation
  never bypasses RX error flags to simulate overlap.
- Prior CCMP management metadata, protected TX encryption/PN/sequence evidence,
  other robust-action handling, PMF negotiation/capabilities and SAE external
  authentication are still outstanding.

Linux's [mac80211 key implementation](https://android.googlesource.com/kernel/common/+/b77587ac51d2fe4b9d5751662ddc083d19153662/net/mac80211/key.c)
provides the independent reference for per-ID key ownership and suppressing
identical key replacement. The bounded association-wide history here is a
conservative driver policy, not a claim that mac80211 supplies that history or
that the GEN3 firmware has equivalent storage.
