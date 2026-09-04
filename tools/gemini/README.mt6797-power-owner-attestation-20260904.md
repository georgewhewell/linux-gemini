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

The current offline-qualified A53 package is
`b1fa9539561dee6674fa6a839d7f2f4851f623f2`, with bundle
`/nix/store/x5m20gna2w7mzalywikvk92bs16g26hg-gemini-a53-thermal-preflight-p22-bundle`
and candidate SHA-256
`baf0f1fdcb581f19f6dc55bf21f6f86b5ac11ba5a54e5b8ff7a35c645d2a90c0`.
Its exact power policy is:

```
power_preflight_policy=owner-attested-20260904
```

It has an independent offline PASS.  Hardware was not run, and the package
has no specific p22 authorization or schedule.

The current offline-qualified G3 package is
`f51d661e823d9ece2011c5688141ddaa7e3c44b6`, with bundle
`/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle`
and candidate SHA-256
`d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985`.
It carries the same exact owner-attested policy token and has an independent
offline PASS.  Its distinct review/authorization, post-watchdog activation,
durable-scanner, possible-physical-reset, observe-always-rollback, and
no-retry conditions remain in force.  Hardware was not run, and the package
has no specific p22 authorization or schedule.

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
`hardware-proof=none-offline-build-only`.  The package was built and tested
offline, but this ledger treats it as pending root review.  It becomes
offline-qualified only after root confirms the final predicate, policy,
bundle identity, and tests.  It is not hardware-authorized or scheduled.

The exact token means explicit owner authority with no machine power probe.
Near misses or generic `owner-attested` values are not equivalent, and the
token must never be treated as a BQ- or USB-derived result.  It changes no
watchdog, rollback, transcript, hash, supervisor-ownership, mutual-exclusion,
or no-retry rule.

The old A72 packages still use an obsolete package policy and are closed
pending a fresh reviewed remediation.  The owner statement does not make
those immutable bundles eligible.  All superseded hashes, including the old
A72 provider and admission candidates, are explicitly closed/no-retry in the
managed queue ledger.

This update changes documentation only.  No package, source, hardware, USB,
radio, or remote-host action was performed.
