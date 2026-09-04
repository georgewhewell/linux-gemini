# MT6797 next-hardware-gate queue

Date: 2026-09-04

Status: offline scheduling decision only.  This note does not authorize or
schedule a flash, boot, reset, USB operation, remote-host operation, or
hardware attempt.

## Decision: no gate is eligible now

Every reviewed package sets `power_preflight_policy=none`.  Accepted Stage A
proves the p22 recovery bytes, CPUs 0--7, the serviceability path, and the
watchdog, but it does not expose a qualified battery or charger reading.  The
current root-owned, read-only Stage-A sysfs check found no device under
`/sys/class/power_supply`; it obtained only the accepted command line and
produced no authoritative capacity, voltage, charge-state, or external-power
datum.  It is therefore not qualifying power evidence.

The full cellular candidate records the earlier broken-connector state, but
the owner later reported reconnecting it and historical battery telemetry
corroborated that reconnection.  Its current state is unknown, not proved
unrepaired.  The read-only BQ observer explicitly forbids treating its
identity/status fields as battery-capacity or external-power proof.  The
reconciled power decision is in
`tools/gemini/README.mt6797-power-sufficiency-20260904.md`.  Relevant retained
evidence is:

- `/mnt/Home/src/nixos-gemini/handoff/restore-stagea-20260904/device-feature-baseline.txt`
- `/mnt/Home/src/nixos-gemini/handoff/restore-stagea-20260904/device-baseline-and-pstore.txt`
- `/mnt/Home/src/nixos-gemini/repo-g3-p22/research/mt6797-bq25896-routing-evidence/README.md`
- `/mnt/Home/src/nixos-gemini/repo-cellular-reconstruction/campaign/06-cellular-modem.md`

Keep every gate closed until the battery connector/power condition is freshly
qualified (and physically repaired if that check fails) and a separate,
independently trustworthy check establishes sufficient battery and stable
external power for the entire candidate, timeout/reset, and Stage-A
rollback/readback interval.  Reconfirm exact accepted Stage-A p22 SHA-256
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`
immediately before any later transaction.  Absence or ambiguity is a stop, not
permission to infer power from USB enumeration.

## Conditional queue after the common power prerequisite

This ranking compares only already-built, unscheduled, isolated packages.  It
is not a schedule.  Each entry still needs a separate review and explicit
authorization at that later time.

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

### 3. MD1 post-vector CCIF/CLDMA read-only observation

Candidate SHA-256
`34689b1814e2a3ce12eb248d7e6594c57e9c661d8a9e1e3b38f9942bf30d184c`
is conditionally third.  Source
`5a7cc6e2e962e25dcde3a8fba42332cd11b78523` adds CCIF and AP-side CLDMA
status reads after hardware-proven lifecycle
`18d8b4adb111540ef4a880f4880ee258bcf59ea0`.  The incremental observer is
read-only, but the inherited lifecycle still performs the already-reviewed
MD1 power/reset and sole boot-vector GO sequence; that makes it a larger power
exposure than the CPU registration/provider observations above.

Acceptance is one complete 5,000-poll transcript, a 30-second window,
90-second watchdog lease, and unconditional Stage-A rollback/readback.  CCCI,
IRQ ownership, DMA preparation, shared memory, firmware load, SIM, RF and
eUICC remain absent and closed.  Evidence:

- `/mnt/Home/src/nixos-gemini/repo-md1-cldma-readonly/research/MT6797_MD1_CLDMA_READONLY_PACKAGE_20260904.md`
- `/mnt/Home/src/nixos-gemini/repo-md1-cldma-readonly/result/PROVENANCE`
- `/mnt/Home/src/nixos-gemini/repo-md1-cldma-readonly/result/ACCEPTANCE`
- `/mnt/Home/src/nixos-gemini/repo-md1-cldma-readonly/result/recovery/p22-one-shot.manifest`

The later full cellular source/package tips
`9b501ccd5347019e056f7b3153ba95957bbcd6c0` /
`d2c9a6adafbf2eebe54671b588b475d21714da40` are not an alternative gate.
Firmware identities are unset, hardware is untested, current power sufficiency
is unqualified, and the candidate adds transport, IRQ/NAPI, firmware,
SIM/raw-IP and optional eUICC surfaces.  Keep it closed.

### 4. G3 fixed-low Panfrost no-job gate

Candidate SHA-256
`af5f45a155317ab829e73d96711403c61d4a23a5cd2e34e597e357132b203bde`
is conditionally fourth.  Source
`ec191faadb50d01a709fcb7786c4ea530839b7d8` is the direct child of passed G2
`2bfbf91f1cfc74003c23564c4e5741bb487225eb`.  It performs one post-watchdog
`Panfrost=m` bind at the single 238 MHz, 846700/850000 uV OPP and submits no
job.  It rejects the shader/tiler/L2 transition timeouts seen in earlier
Panfrost binds.

This remains last because active MFG-domain/Panfrost entry has the strongest
specific hard-hang history.  Its kernel nowayout watchdog is armed before the
single bounded module load, but there is no userspace petter; its package
explicitly makes no recovery claim before successful watchdog probe/arm.  The
90-second observation and 120-second supervisor lease sit inside a
1,800-second durable scanner window, and one physical reset may still be
needed to expose preloader for rollback.  Evidence:

- `/mnt/Home/src/nixos-gemini/linux-gemini-g3-audit-decision/tools/gemini/README.mt6797-gpu-g3-audit.md`
- `/nix/store/9znd9qx40zjx23fhd2bm1jg6b6awmf1w-gemini-g3-low-opp-nojob-p22-bundle/MANIFEST`
- `/nix/store/9znd9qx40zjx23fhd2bm1jg6b6awmf1w-gemini-g3-low-opp-nojob-p22-bundle/p22-one-shot.manifest`
- `/nix/store/9znd9qx40zjx23fhd2bm1jg6b6awmf1w-gemini-g3-low-opp-nojob-p22-bundle/REVIEW-NOTICE`

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
