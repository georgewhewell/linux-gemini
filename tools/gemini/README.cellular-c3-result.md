# C3: HS1 received, runtime queued, no HS2

Evidence directory: `handoff/cellular-c3-w5ObdBn9` in the parent workspace.
Candidate image SHA256:
`128648c711c5317ab961e2d082bb1aab2bddf92ee78da7d156836d20f134dfb2`.
Kernel source: `764d3e6adeb3de30e2136a2989d787c21880d2fb`.
Candidate boot: `e1952036-f471-4b90-9459-7062037623e1`.

## Observed boundary

The saved `dmesg-first-event.txt` records:

- 7.640563: post-GO AP admission passed, IP_BUSY=1 observed-only,
  UL/SO/TXIRQ/RXIRQ and all queue heads zero.
- 7.642598: seven CLDMA queues owned, waiting for HS1 v2.
- 7.642719: HS1 v2 accepted; 324-byte runtime reply queued.
- 23.645382: terminal failure reason=-110.

The installed test ran once and returned `CELLULAR_FAIL no-HS2`. No AT,
host acceptance publication, retry or reprobe occurred. Queueing is a
software-side result, not proof of TX descriptor consumption or modem
acceptance of the runtime payload. HS1 establishes an actual control-RX
milestone, not attach or data service.

Both saved interrupt snapshots show two CLDMA interrupts (hwirq 265),
two MD-WDT interrupts (266), and zero AP-CCIF interrupts (147). The driver
uses the same -ETIMEDOUT reason for its MD-WDT handler and its 30*HZ AP
handshake timer. The terminal event only about 16 seconds after startup,
together with the MD-WDT counters, points to the modem-watchdog path, not
expiry of the AP handshake timer. The generic terminal log alone does not
identify the caller or establish why the modem watchdog fired. No saved
TX HWO/reclaim snapshot proves the runtime message reached the modem.

The next source/evidence checks are vendor runtime wire compatibility,
TX start/resume and completion handling, and terminal-cause discrimination.
Do not increase the handshake timeout or bypass the watchdog on the basis
of this capture. No new candidate is authorized by this document.

## Evidence and recovery closure

Full selected RAM archive SHA256:
`3b124aee964a4c8a5383c6106593451e5e7038d232d39402b9db2a549ce711b4`.
Live DT SHA256:
`b5dec948a0fa206ade6121b181e9686f5a85adedda7261860d1ce72ce9ff7133`.
Pstore SHA256:
`a2d18bb87915f0fc6451d0b7a9cf52b20bc46b03ee4db6f4c0287b9eadb2548c`.
Pstore is attributable to C3, including its recovery marker at 158.830.

One candidate write was supervised before the writer started. Original
execution 40838 terminated at 04:42:48 on 2026-09-05, observation failed
and StageA recovered with pstore captured. Rescue boot
`a0848113-f837-40af-84ef-e27bf29f457e` passed USB/RJ45 health, CPUs0-7,
no failed units and exact p22/p30/p31 readbacks. Both transaction locks
were free; no writer or supervisor remained. `RESULT.md` contains the
full partition hashes and chronology. Thermal-final is a single snapshot,
not a paired thermal test or proof of protection.
