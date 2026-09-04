# MT6797 next-hardware-gate queue

Date: 2026-09-04

Status: offline scheduling decision only.  This note does not authorize or
schedule a flash, boot, reset, USB operation, remote-host operation, or
hardware attempt.

## Decision: owner resolved the common power blocker; A53 remains first

The owner has now explicitly stated **"power is fine, there is no power
blocker"**.  That current owner attestation resolves the common power blocker
for scheduling purposes and supersedes this note's earlier "no gate is
eligible" conclusion.  The retained absence of accepted-Stage-A power-supply
telemetry remains true, but it is no longer the authority for holding every
lane closed.  The exact scope and package-policy audit are in
`tools/gemini/README.mt6797-power-owner-attestation-20260904.md`.

This is not a hardware measurement and does not infer capacity, voltage,
charge state, connector condition, or external-power state from BQ identity,
USB enumeration, or SSH.  It also does not choose or authorize any p22 write.
Each candidate below still requires a separate exact-candidate review and
explicit owner authorization.  Relevant retained historical evidence is:

- `/mnt/Home/src/nixos-gemini/handoff/restore-stagea-20260904/device-feature-baseline.txt`
- `/mnt/Home/src/nixos-gemini/handoff/restore-stagea-20260904/device-baseline-and-pstore.txt`
- `/mnt/Home/src/nixos-gemini/repo-g3-p22/research/mt6797-bq25896-routing-evidence/README.md`
- `/mnt/Home/src/nixos-gemini/repo-cellular-reconstruction/campaign/06-cellular-modem.md`

Reconfirm exact accepted Stage-A p22 SHA-256
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`
immediately before any later transaction.  Contrary new power evidence or a
later owner report that the condition changed closes the queue again; it is
not permission to infer power from USB enumeration.

## Current technical queue after the owner attestation

This is a risk/sequence ranking of isolated gates, not a schedule.  "Technical
queue" means the shared power objection has been resolved; it does not mean a
write is authorized.  The currently executable order is A53, then the A72
provider, then G3.  The remediated cellular observer has the lower-risk slot
before G3, but it cannot enter that slot until the small policy-only repackage
described below makes its immutable harness executable.  Every entry still
needs a separate review and explicit authorization.

### 1. A53 registration/calibration observation

Candidate SHA-256
`8f65e46a94e288b2fed585647822cab6b356493b19c4b7de9edc4612deee1139`
is the first conditional choice.  Source
`7f4805e446a0afb40ba966e620c3c06737cf6625` and package
`d27a31469f817142b3e4c070d293b57ae91316a0` compose the exact accepted
Stage-A DT plus only the CPU-clock, one-OPP A53 cpufreq, shared BUCKA/Vsram,
calibrated thermal, and cooling-registration delta.  The collector is
read-only and requires both policies to remain at their single current
frequency; it does not exercise a frequency transition, cooling action, load,
or shutdown.

The earlier broad-DT candidate
`50f3cca869b43cadd32e5dd97b1d848004080ff93234da8962817093db5fd5b0`
was written once, produced no candidate entry evidence, and must not be
retried.  The later common entry-isolation gate passed, removing only the
production-kernel handoff uncertainty; it does not convert `50f3...` into a
retryable image.  Evidence:

- `/mnt/Home/src/nixos-gemini/handoff/a53-thermal-p22-20260904T122333+0200/RESULT.md`
- `/mnt/Home/src/nixos-gemini/repo-a53-thermal-preflight/research/A53_DVFS_THERMAL_SOFTWARE_PREFLIGHT_20260904.md`
- `/mnt/Home/src/nixos-gemini/repo-a53-thermal-preflight/result/p22-one-shot.manifest`

Runtime boundary: 60-second acceptance inside a 90-second initramfs watchdog
lease, followed by unconditional exact Stage-A rollback.  A pass qualifies
registration/calibration only.  A lower-OPP transition remains a new package
and later transaction.

### 2. A72 provider observation, then the distinct admission gate

The provider/resource candidate
`6ddc55bfb8d79e8442644812582dc58643d87eb8223007982a821f1d67e0a803`
is conditionally second.  It reads the DA9214 BUCKB, secure SRAM selector,
MP2-clock, reset and SPM state with CPU8/9 disabled, no CPU_ON, no OPP,
cpufreq, OCP, or frequency request.  Its helper is read-only.

Only after that exact candidate passes and Stage A is restored/read back may
the separate CPU8-admission candidate
`cc8b3ab8864766138a9697ffae79b2c86204cef086eabdeabfca6179b2ca57e0`
be reconsidered.  That second image publishes the reviewed CPU4 prerequisite,
r0p1 CPU8-only admission owner, and CPU9 block, but still leaves CPU8 offline
and makes no CPU_ON request.  These are two mutually exclusive boots, never a
combined run.  Neither result authorizes the later one-request CPU8-online
transaction.

Both use a 60-second acceptance window, 90-second watchdog lease, and
observe-always-rollback.  Evidence:

- `/mnt/Home/src/nixos-gemini/repo-a72-cpu8-preflight/research/A72_CPU8_PREFLIGHT_PACKAGE_20260904.md`
- `/nix/store/74svv37859pfvsy033rhv2srkvhy7z0b-gemini-a72-provider-resource-preflight-p22-bundle/p22-one-shot.manifest`
- `/nix/store/rs5vnqrs12qm5gab164j3flf0a47mhlk-gemini-a72-cpu8-gate-preflight-p22-bundle/p22-one-shot.manifest`

### Pending insertion at 3: MD1 post-vector CCIF/CLDMA read-only observation

Candidate SHA-256
`edb4876686c6c18d534b3966948490280299d02fbd141f08e4f8fa4457f0f6cf`
is third by risk and dependency.  Source
`8faeb854c00524a2a39450f9c72aaa3601e1c2a5`, a direct child of observer
source `5a7cc6e2e962e25dcde3a8fba42332cd11b78523`, adds the mandatory in-kernel
proof that the 31-second reset-mode watchdog is already running before the
first EMI/lifecycle write.  Package
`44f6496588ecab4425b31ad09e995689502e1145` adds strict ordered transcript
parsing, unique timestamps, at least five seconds and exactly 5,000 polls,
kernel-fault rejection, supervisor permission before the candidate scanner,
and unconditional exact rollback/readback.

The post-vector CCIF/AP-side CLDMA observer is register-read-only, but the
inherited lifecycle still writes EMI-MPU, CCIF SRAM/ACK, PMIC, power, clock,
reset, modem-watchdog, and the sole boot vector.  Linux installs no CCCI,
modem IRQ/DMA/GPD/HWO, firmware-loading, SIM, RF, or eUICC control path; it
does inspect LK metadata and executes firmware already loaded by LK.

Acceptance is one complete strict 5,000-poll transcript, a 30-second window,
90-second watchdog lease, observe-always-rollback, and exact restored Stage-A
p22 plus unchanged p30/p31 readback.  The immutable bundle is still
mechanically non-executable because it records
`hardware_eligibility=blocked-power-proof-required`.  The owner attestation
resolves the underlying reason but cannot rewrite those bytes.  A separately
reviewed policy-only repackage using
`power_preflight_policy=owner-attested-20260904` and
`hardware_eligibility=eligible` is required before this entry can be selected.
Evidence:

- `/mnt/Home/src/nixos-gemini/repo-md1-cldma-remediation/research/MT6797_MD1_CLDMA_READONLY_PACKAGE_20260904.md`
- `/nix/store/mz2l76bb1wclq1vs5g1sn2lpmw0x36i8-gemini-md1-cldma-readonly-observer-remediated-p22-bundle/PROVENANCE`
- `/nix/store/mz2l76bb1wclq1vs5g1sn2lpmw0x36i8-gemini-md1-cldma-readonly-observer-remediated-p22-bundle/ACCEPTANCE`
- `/nix/store/mz2l76bb1wclq1vs5g1sn2lpmw0x36i8-gemini-md1-cldma-readonly-observer-remediated-p22-bundle/recovery/p22-one-shot.manifest`

The earlier candidate
`34689b1814e2a3ce12eb248d7e6594c57e9c661d8a9e1e3b38f9942bf30d184c`
and package `45344769d98639709bc0de11388aa9a84382bffe` are superseded by the
watchdog/parser/supervisor remediation and must not be scheduled or reused.

The later full cellular source/package tips
`9b501ccd5347019e056f7b3153ba95957bbcd6c0` /
`d2c9a6adafbf2eebe54671b588b475d21714da40` are not an alternative gate.
Firmware identities are unset, hardware is untested, and the candidate adds
transport, IRQ/NAPI, firmware, SIM/raw-IP and optional eUICC surfaces.  The
owner power attestation changes none of those blockers.  Keep it closed.

### 3 now, 4 after cellular remediation: G3 fixed-low Panfrost no-job gate

Candidate SHA-256
`d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985`
is currently third and moves to fourth only after the cellular policy-only
repackage is reviewed.  Source
`ec191faadb50d01a709fcb7786c4ea530839b7d8` is the direct child of passed G2
`2bfbf91f1cfc74003c23564c4e5741bb487225eb`.  It performs one post-watchdog
`Panfrost=m` bind at the single 238 MHz, 846700/850000 uV OPP and submits no
job.  Package `30933885e9530dc181dbc9e6a8f6f653559355f3` hardens exact
module provenance, helper execution, supervisor permission, transaction
ownership, timeout handling, and post-rollback pstore capture.  It rejects the
shader/tiler/L2 transition timeouts seen in earlier Panfrost binds.

This remains last because active MFG-domain/Panfrost entry has the strongest
specific hard-hang history.  Its kernel nowayout watchdog is armed before the
single bounded module load, but there is no userspace petter; its package
explicitly makes no recovery claim before successful watchdog probe/arm.  The
90-second observation and 120-second supervisor lease sit inside a
1,800-second durable scanner window, and one physical reset may still be
needed to expose preloader for rollback.  Evidence:

- `/mnt/Home/src/nixos-gemini/linux-gemini-g3-audit-decision/tools/gemini/README.mt6797-gpu-g3-audit.md`
- `/nix/store/nb30c4aa8n38ws1h7qm7098kdihj6q6h-gemini-g3-low-opp-nojob-p22-bundle/MANIFEST`
- `/nix/store/nb30c4aa8n38ws1h7qm7098kdihj6q6h-gemini-g3-low-opp-nojob-p22-bundle/p22-one-shot.manifest`
- `/nix/store/nb30c4aa8n38ws1h7qm7098kdihj6q6h-gemini-g3-low-opp-nojob-p22-bundle/REVIEW-NOTICE`

Its `power_preflight_policy=none` is not an execution block, but a future
reviewed package should replace it with the exact owner-attestation token for
truthful provenance.  The superseded package candidate
`af5f45a155317ab829e73d96711403c61d4a23a5cd2e34e597e357132b203bde`
is retired and must not be substituted for `d1d188...`.

No GPU job, other OPP, DVFS, thermal, suspend, or render-node experiment is
eligible before this exact no-job rung passes and Stage A is restored.

## Mutual exclusion and stop rules

All entries contend for the same physical device, p22 target, USB/preloader
catcher, watchdog, and Stage-A rollback path.  At most one reviewed candidate
may own them.  A later transaction must use a fresh nonexistent capture
directory, arm the independent rollback owner before the write, perform one
candidate write, and restore Stage A after every result.  Exact restored p22
and unchanged p30/p31 readbacks are prerequisites for considering the next
queue entry.

Do not batch candidates, combine overlays, carry an accepted candidate into a
second test, or interpret a timeout/reset as permission to retry.  Any missing
marker, ambiguous power state, unexpected write, watchdog/rollback failure,
hard hang, or need for manual recovery closes that exact hash.  Re-rank from
new retained evidence; do not advance automatically.
