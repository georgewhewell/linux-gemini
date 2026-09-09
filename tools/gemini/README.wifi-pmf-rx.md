# Protected management RX prerequisite

This slice adds bounded BIP-CMAC-128 and CCMP robust-management receive handling.
It does not enable PMF negotiation or SAE admission; `pmf_negotiated` remains
false in production. It has not been exercised on the Gemini firmware/device.

`mgmt_rx.c` parses the real GEN3 RX descriptor and requires a complete native
management header, supported cipher, clear error status, and explicit replay
metadata. Group BIP requires the intact trailing MMIE and its full key ID 4/5
and 48-bit IPN. Software AES-CMAC verifies the IEEE 802.11 authenticated bytes;
the truncated RX descriptor key field and group1 PN do not identify the IGTK.
Unicast CCMP requires a protected frame and group1 PN and uses a management
replay counter distinct from the sixteen data counters.

The production callback checks AP/destination/BSS/WLAN identity, current key,
association ownership and WTBL generation before advancing replay state. Valid
deauthentication/disassociation queues the existing disconnect path. Once PMF
is selected, rejected frames cannot fall through to the legacy unauthenticated
disconnect path. Robust actions are classified and checked, but their delivery
and SA Query processing remain unimplemented.

Validation uses `test-wifi-pmf-rx-host.py` with actual production parser, BIP
formatting, callbacks, ownership helpers and replay mutation. OpenSSL substitutes
only for the kernel AES-CMAC primitive; RFC4493 and independently generated BIP
vectors check the host primitive/formatting. Tests cover both IGTK IDs, AAD/body/
MMIE/MIC corruptions, all descriptor group combinations, offsets, missing metadata,
truncation, PN bounds/replay, owner/key/WTBL mismatch and plaintext rejection.
The harness runs with AddressSanitizer and UndefinedBehaviorSanitizer. Existing
worker/connect/key/link/data/admission and RX/key codec regressions pass.
`radio.o`, `mgmt_rx.o` and `lib/crypto/aes.o` compile for arm64 with the accepted
M25 kernel configuration and the newly selected AES-CMAC library option.

Firmware assumptions still require controlled evidence before enabling PMF:

- The deployed blob must preserve the complete BIP MMIE. A stripped MMIE cannot
  be recovered from the descriptor's two-bit key ID and is rejected.
- CCMP robust management must expose the decrypted body, group1 PN and integrity
  status in the expected GEN3 format. Current vendor auth/assoc and nic_rx sources
  support this interpretation, but synthetic tests cannot verify the blob.
- IGTK overlap/rekey and historical key-reinstallation policy, robust-action
  TX/RX delivery, SA Query, PMF negotiation/capabilities and the SAE external-auth
  bridge remain separate prerequisites. This is not working WPA3 support.

Source basis: vendor GEN3 `include/nic/nic_rx.h`, `nic/nic_rx.c`,
`mgmt/saa_fsm.c`, `mgmt/auth.c` and `mgmt/assoc.c`; this kernel's
`net/mac80211/wpa.c`, `net/mac80211/rx.c`, `include/linux/ieee80211.h` and
`include/crypto/aes-cbc-macs.h`. Linux documents the PMF RX interface in
[mac80211 advanced](https://cdn.kernel.org/doc/html/latest/driver-api/80211/mac80211-advanced.html).
