# PMF firmware qualification plan

**No hardware was accessed for this task.** Production PMF/SAE stays disabled.
The host protocol work ends at `83d207ce3d911cb3c4dd7f4ace6658281341dc3c`.
This document and the adjacent observer add no kernel/firmware commands or
protocol layer. Future candidate boots, AP changes, key operations and injected
frames require a separately authorized, isolated qualification transaction.

## What the accepted WPA2 connection can establish now

These observations can be collected without scanning, sending test traffic,
changing keys, reassociating or rebooting. They establish provenance and a
baseline; **none resolves PMF cryptography or IGTK coexistence**.

| Cached observation | Available interface | What it establishes / limit |
| --- | --- | --- |
| Accepted boot identity and system | `/proc/.../boot_id`, kernel release, current-system link | Compare with the parent's accepted manifest; release string alone is not an image hash. |
| Current WPA2/CCMP association | supplicant `STATUS`, `iw dev wlan0 link` | Current peer, frequency and negotiated status. Matching endpoints cannot exclude an intermediate reconnect. |
| AP advertised RSN/PMF | supplicant `BSS <BSSID>` | Existing cached flags/IEs and cache age; no new scan. AP capability does not establish station/firmware PMF support. |
| Driver-advertised capabilities | `iw phy <phy> info` | Accepted driver exposes station mode and CCMP; this is host policy, not a firmware BIP capability query. |
| Firmware provenance | existing boot logs; hash of `/lib/firmware/WIFI_RAM_CODE_6797` if present | Startup reports product/version and prior group1 configuration. File hash is disk provenance, not live firmware readback. |
| Existing errors and generic traffic counters | read-only `dmesg`; netdev sysfs statistics | Correlate errors/counter changes with observations. Drops are not PMF failures or measured end-to-end loss. |
| Service/qdisc state | `systemctl show` properties; `tc -s qdisc show` | Cached lifecycle and queue metadata; no restart or qdisc mutation. |

The verified loader pins firmware SHA-256
`a69383d74d829430487c39eef6b5e281b25f901595c903a632a10aa8631426dd`
and size 411632 bytes. Prior accepted evidence reported product `0x6797`,
firmware `0x0403`; this task did not re-observe the device. Missing files/logs
remain missing evidence. The parent's existing accepted-image guard is separate.

## Observer usage and boundary

`wifi-pmf-observe.py collect` is a stdout-only target observer; `compare` reads
saved JSON offline on the workstation. It requires Python 3 already available
on the target. `iw`, `systemctl`, `tc` and `dmesg` are optional; unsupported reads
are recorded, never replaced by driver probing. The script never invokes SSH.
After a separate target-read authorization, an operator can stream it using the
approved control connection and an independently recorded boot ID:

```sh
ssh -- "$approved_target" python3 -B - collect --expected-boot-id "$approved_boot_id" \
  < tools/gemini/wifi-pmf-observe.py > before.json
# Repeat later only when desired; the collector has no polling/traffic loop.
python3 -B tools/gemini/wifi-pmf-observe.py compare before.json after.json > comparison.json
```

`--cached-bssid 02:11:22:33:44:55` optionally selects an existing cached BSS;
otherwise it requests the current peer's cache entry. The collector sends only
`STATUS` and `BSS <address>` over AF_UNIX. Its local socket uses Linux's abstract
namespace, so no socket file is created. It does not attach for events, read
`wpa.conf`/saved passwords/PMKSA, or open IP/raw/packet sockets. Output retains
only allowlisted supplicant fields and known kernel metadata log messages.
It includes public peer identifiers and IEs. All target file opens are reads;
normal scheduling/logging/accounting by the OS is outside the observer's control.

Exit 0 means boot/status endpoint guards match and both status samples show
completed WPA2-PSK/CCMP. Optional command failures still need inspection. It never
reports PMF qualified. The comparison withholds counter deltas on changed or
missing identity, identifies counter resets, and cannot prove uninterrupted
association or completeness of rate-limited/wrapped kernel logs.

Offline validation: `python3 -B tools/gemini/test-wifi-pmf-observe.py`. Tests use
fake files, commands and sockets, verify wrong-boot/driver rejection before any
query, allowed verbs, no filesystem socket, secret-field filtering, missing
interfaces, endpoint changes and conservative counter comparison. They contact
neither this workstation's wireless interfaces nor Gemini.

## Interfaces that cannot answer the firmware questions safely

Source inspection covers accepted Wi-Fi base `9db37781bf97565373737183a8f5f4a47729235b`
and the isolated prerequisite tree. `radio.c` has no get-station, key-readback,
monitor interface, vendor/testmode command or ethtool register-dump API. The
accepted driver cannot negotiate PMF or install an IGTK. An Ethernet capture on
`wlan0` does not expose GEN3 RX descriptors or driver-consumed management frames.

`core.c` exposes only the optional open-auth qualification trigger/result pair.
Reading an already completed result is passive but says nothing about PMF;
writing its trigger performs an active exchange and is outside this plan's
accepted-system observations. No runtime raw-PMF/WTBL/per-key replay telemetry
exists. Enabling dynamic debug or adding trace hooks changes state and is not
part of the collector.

Vendor `nic_cmd_event.h` lists NIC capability `0x80`, link quality `0x81`,
statistics `0x82/0x85`, register access `0xc0`, debug `0xc4` and memory dump
`0xc6`. Those command numbers do not supply a safe existing runtime interface.
STA statistics explicitly has `ucReadClear`/`ucLlsReadClear`. The implemented NIC
capability exchange changes IRQ/session ownership and belongs to startup; it
must not be rerun against a live association. Feature/compile words are parsed
internally but expose no established IGTK-overlap guarantee. HIF WHISR has
read-clear behavior and RX/TX-credit ports are consumed by the IRQ pump. Do not
use `/dev/mem`, live memory/key dumps, `iwpriv`, arbitrary nl80211/vendor commands,
or register reads to bypass ownership. No such tooling is included.

## Future isolated qualification: prerequisites and evidence

Use a controlled AP and a separate monitor adapter, with a synthetic lab
credential, a single test station, an AP/fixture capable of retaining both
IGTKs and controlling protected frames, and the existing supervised recovery
procedure. Start with **WPA2-PSK/CCMP plus required PMF**, not the private WPA3 AP:
this separates firmware PMF from the still-missing SAE bridge. The reviewed AP
configuration would use `wpa=2`, `wpa_key_mgmt=WPA-PSK`, `rsn_pairwise=CCMP`,
`ieee80211w=2`, and `group_mgmt_cipher=AES-128-CMAC`; channel and legacy-rate
settings must match the candidate. No AP configuration is installed here.

The current isolated kernel still rejects PMF. Before any future boot, prepare
and review a qualification candidate with explicit test admission, metadata
observability and its exact image/DTB/config/firmware/source manifest. Do not
turn on PMF/SAE in the accepted image to obtain evidence. Stock AP rekeying alone
may not retain the old IGTK long enough to test overlap; qualify the AP fixture's
actual behavior before testing Gemini. Synthetic keys stay in the isolated test
fixture; reports contain key labels/IDs, not key material.

Minimum candidate observations, not implemented by this task: bounded records
of association/WTBL generation, command sequence/operation/key ID/HIF outcome;
management RX descriptor format, status/cipher/BSS/WLAN/group bitmap, relevant
PN and MMIE presence/ID/IPN, host MIC verdict and replay counter before/after;
management TX action/transaction ID, PID/SN and matched completion. Capture RX
rejection metadata before the generic parser discards integrity-error frames.
Record no key command body, arbitrary firmware memory or user payload. Pair
these records with AP semantic results and independent over-air capture;
TX-done/Protected-bit alone is not proof of encryption or successful decryption.

| Order / unknown | Controlled operation, future candidate only | Required observation / pass criterion |
| --- | --- | --- |
| 1. PMF handshake/initial IGTK | Join the lab WPA2/PMF AP with one IGTK | Association exchange and AP/station agree on PMF and AES-CMAC. Correct owned key command appears. HIF submission alone is insufficient. |
| 2. CCMP management RX | AP sends protected SA Query request | Over-air CCMP is valid; DUT gets decrypted action/ID, cipher 4, group1 PN and clean integrity metadata with expected owner/BSS/WLAN. Replay does not advance the management counter; data-TID counters remain independent. |
| 3. Protected management TX | Obtain at least two DUT SA Query responses | AP authenticates/decrypts the echoed IDs under the current PTK; monitor confirms protected CCMP and nonrepeating/increasing PN within the management domain. Correlate exact TX PID/SN/completion; do not infer this from data TX. |
| 4. BIP RX format/integrity | Send controlled broadcast robust actions under IGTK 4, then replay/corrupt a MIC | Complete MMIE retained, full ID 4 and IPN preserved, cipher/status usable; valid MIC advances only that key's IPN, replay/corruption never advances it. No acceptance based only on firmware cipher metadata. |
| 5. Functional overlap | Keep key 4 at IPN 100, install distinct key 5 at IPN 7; interleave valid 4/101, 5/8, 4/102 | Both keys are delivered and independently accepted without reverting software/default selection. Preserve the exact association and WTBL generation. This establishes functional overlap, not an internal firmware slot architecture. |
| 6. Delete isolation | Delete 4, then send old 4/103 and valid 5/9; repeat opposite order in a fresh association | Retired ID rejects, surviving ID passes; unchanged GTK broadcast and PTK unicast test traffic still decrypts. Absence of a crash is insufficient. |
| 7. Replacement/reinstall | Fresh material under same ID; duplicate install, retired-material reinstall and changed supplied IPN | Fresh key works, old-key MIC rejects, other ID survives. Duplicate emits no firmware key command or PN reset; retired material fails before transport. Association history survives deleting both live keys. |
| 8. Plaintext trigger and SA Query | AP sends plaintext unicast deauth/disassoc reason 6/7; once answer query, once withhold response | Observe actual cipher-mismatch/BSS/WLAN metadata. Valid response stops the query; withheld response gives bounded retries and local timeout. A missing/255 WLAN index is a failed current assumption, not permission to trust addresses. |
| 9. Teardown/failure | Isolated reconnect and planned command failure under supervised recovery | No stale key/WTBL ownership or replay state crosses associations; uncertain transport fences before reuse. Never inject a failure on the accepted live connection. |

Use a controlled protected vendor-specific robust action (category 126) with a
known test body for repeated BIP RX observations: the current host path verifies
it without implementing an application action. This tests authentication/replay,
not general robust-action interoperability. Valid deauth/disassoc intentionally
ends an association and belongs in a separate terminal case.

Stop qualification on a runtime HIF fault, ambiguous command ownership, unexpected
disconnect, missing essential metadata, surviving-key/GTK regression or failed
MIC/replay policy. Preserve evidence and follow the separately approved recovery
transaction. If firmware drops/flags old-key frames, software CMAC cannot recover
them; do not bypass error checks or infer overlap from host tests. A frame absent
at the DUT with only an over-air transmission is not by itself proof of a firmware
drop. Missing evidence remains unknown. No PMF/SAE enablement follows automatically
from completing this matrix; the separate negotiation/SAE work still needs review.

Source basis: local `core.c`, `radio.c`, `firmware.c`, `capability.c`,
`hif_runtime.c`, `hif_regs.h`, vendor GEN3 `nic_cmd_event.h` and
`wlan_oid.c`. The [supplicant control API](https://w1.fi/wpa_supplicant/devel/ctrl_iface_page.html)
defines STATUS and cached BSS queries; the [hostapd configuration reference](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/9839ecd75c832023d4d13fd2917a8c28261ff668/hostapd/hostapd.conf)
defines the proposed WPA2/PMF settings.
