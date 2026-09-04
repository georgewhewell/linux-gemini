# MT6797 p22 owner power attestation

Date: 2026-09-04

Status: offline scheduling record only.  This note does not authorize or
schedule a flash, boot, reset, USB operation, remote-host operation, or any
other hardware action.

## Current decision

The device owner has now stated: **"power is fine, there is no power
blocker"**.  For the current MT6797 gate queue, that explicit, current owner
attestation resolves the common power-sufficiency blocker recorded by
`ddf21df1f963941e29a30af9f7f8b56bd35a93a2` and the separate conservative
power audit at `5f41246cb5dcc323ecf764f9c3f50a13b821708e`.

The earlier conclusion that no gate was eligible solely because fresh battery
telemetry was unavailable is therefore obsolete.  The owner statement is the
authority for this scheduling input; it is not presented as a fuel-gauge,
voltage, capacity, charger, or connector measurement.  No such values are
inferred from BQ status, USB enumeration, SSH reachability, or the empty
accepted-Stage-A `/sys/class/power_supply` result.

This resolves only the shared power blocker.  It does **not** select a
candidate, authorize a p22 write, waive package review, or weaken any
candidate-specific watchdog, timeout, rollback, readback, mutual-exclusion,
or no-retry rule.  A transaction still needs a separate explicit owner
authorization naming the exact candidate and must start from the exact
accepted Stage-A p22 SHA-256
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`.

Contrary new physical evidence, a later owner report that power has changed,
or loss of the recovery power path invalidates this scheduling input and
closes the queue until the owner reassesses it.  Ordinary candidate failure or
timeout is never permission to reinterpret or reuse the attestation for a
retry of that hash.

## Current package-policy audit

The previously offline-qualified A53 package is
`b1fa9539561dee6674fa6a839d7f2f4851f623f2`, with bundle
`/nix/store/x5m20gna2w7mzalywikvk92bs16g26hg-gemini-a53-thermal-preflight-p22-bundle`
and candidate SHA-256
`baf0f1fdcb581f19f6dc55bf21f6f86b5ac11ba5a54e5b8ff7a35c645d2a90c0`.
Its exact power policy is:

```
power_preflight_policy=owner-attested-20260904
```

It had an independent offline PASS, but its subsequent hardware collector
failed.  Candidate `baf0f1fd...` is therefore retired and closed/no-retry; the
offline review must not be represented as a hardware pass or production
readiness.  Exact failure and Stage-A recovery evidence is recorded in the
managed queue ledger.  It has no further p22 authorization or schedule.

The independently reviewed G3 package is
`f51d661e823d9ece2011c5688141ddaa7e3c44b6`, with bundle
`/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle`
and candidate SHA-256
`d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985`.
It carries the same exact owner-attested policy token and has an independent
offline PASS.  Its distinct review/authorization, post-watchdog activation,
durable-scanner, possible-physical-reset, observe-always-rollback, and
no-retry conditions remain in force.  Hardware was not run, and the package
has no specific p22 authorization or schedule.  A subsequent readiness audit
found that its exact-model validation rejects the retained runtime model
`MT6797X` and that its pstore helper invokes absent `/bin/busybox`.  Candidate
`d1d188f0...` is consequently not hardware-ready and is closed/no-retry
pending a newly hashed and independently reviewed package.

The final cellular candidate bundle is
`/nix/store/b1kgbvpa2rlkp7w8d0kzh618m6j173kd-gemini-md1-cldma-readonly-observer-postaudit-p22-bundle`,
candidate SHA-256
`777c182860863009174817e1d57970fc9fa8eb6f0bcc416803ba7ea8952a3213`.
Its unchanged source is
`8faeb854c00524a2a39450f9c72aaa3601e1c2a5`; package lineage is
`c56ea7a569a8ce7a29f602fdb1c8ca1f0fb4690f` ->
`718b9d1849d632b2135a2da23d43a337a1efab59` ->
`5b45870571e2dfbf0dd06731001ce9a624ad085e` ->
`7194ebaec4b36cf5d46075cc4d1836254c2b196d`.

That final cellular manifest uses the same exact owner-attested token, and
retains `scheduling=none`, `specific-p22-authorization=none`, and
`hardware-proof=none-offline-build-only`.  Independent offline review
reproduced the bundle, verified every embedded checksum, and passed the full
acceptance/recovery harness plus an additional adversarial predicate matrix.
The final predicate and exact policy token are offline-qualified.  It is not
hardware-authorized or scheduled.

The exact token means explicit owner authority with no machine power probe.
Near misses or generic `owner-attested` values are not equivalent, and the
token must never be treated as a BQ- or USB-derived result.  It changes no
watchdog, rollback, transcript, hash, supervisor-ownership, mutual-exclusion,
or no-retry rule.

The current offline-qualified A72 package is
`7f528b23b8a10a5338229449c26d7da1cc18a6c8`, with source
`301a95567a799bbb3c81eba6653acc7491050b88` and package contract
`fe16911ccbd55801a3c79a26e117f5d3a91b169a`.  The provider bundle is
`/nix/store/r7wnz7m86739dj5l8wjlwinhj3z0d8z5-gemini-a72-provider-resource-preflight-p22-bundle`,
candidate SHA-256
`da66b64a37bc3e8c9695f0181fde45e8084179aacab55b5e4aae36c315ae98a5`.
The separate CPU8-admission bundle is
`/nix/store/iqb6k8mvra3gsjj47kmhzlv1x0n3xan4-gemini-a72-cpu8-gate-preflight-p22-bundle`,
candidate SHA-256
`fd2c457aee5aedd3d736acd3f68463c20c0a07db9334d81d0376b2e3ef164dd3`.
Both carry the exact owner-attested policy token and passed independent
offline review of their immutable hashes, watchdog-before-access,
supervisor-owned recovery, strict 21-sample/at-least-60-second parsers, and
exact Stage-A p22/p30/p31 rollback contract.  The CPU8 gate mechanically
requires a successful provider result bound to the exact provider candidate,
source, package contract, and rollback identity.  Neither bundle has been run
on hardware, specifically authorized, or scheduled.

The superseded A72 provider and admission candidates remain closed/no-retry
in the managed queue ledger.  The owner statement does not make those old
immutable bundles eligible.

This update changes documentation only.  No package, source, hardware, USB,
radio, or remote-host action was performed.
