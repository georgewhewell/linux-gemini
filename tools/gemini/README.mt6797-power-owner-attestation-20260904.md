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

The A53 and A72 manifests use `power_preflight_policy=none`.  In their shared
harness that value means no machine-generated power preflight is claimed; it
does not itself reject `--execute`.  The current owner attestation therefore
removes their documentation-level power blocker without changing their
candidate bytes.

The remediated G3 package is
`/nix/store/nb30c4aa8n38ws1h7qm7098kdihj6q6h-gemini-g3-low-opp-nojob-p22-bundle`,
candidate SHA-256
`d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985`.
Its p22 manifest also uses `power_preflight_policy=none`; that is not a
mechanical execution block.  Its `MANIFEST` says only that the package cannot
make a power-preflight claim.  All of its distinct review/authorization,
post-watchdog activation, durable-scanner, possible-physical-reset,
observe-always-rollback, and no-retry conditions remain in force.

The remediated cellular observer package is
`/nix/store/mz2l76bb1wclq1vs5g1sn2lpmw0x36i8-gemini-md1-cldma-readonly-observer-remediated-p22-bundle`,
candidate SHA-256
`edb4876686c6c18d534b3966948490280299d02fbd141f08e4f8fa4457f0f6cf`.
Unlike G3, its manifest has the deliberate mechanical barrier
`hardware_eligibility=blocked-power-proof-required`; the bundled harness
rejects `--execute` before preflight, SSH, its supervisor, or either scanner.
The owner statement does not silently alter that immutable bundle.  It remains
non-executable until a separately reviewed policy-only package is produced.

## Smallest future manifest update

Use one explicit policy token in both future reviewed packages:

```
power_preflight_policy=owner-attested-20260904
```

The harness schema should accept that exact value and treat it as an explicit
owner authority with no machine power probe; it must not alias it to a BQ or
USB-derived result.  The value should be exact rather than a generic
`owner-attested` token so a later physical-state change requires a new review.

For G3, that manifest value plus the corresponding two parser branches is the
entire power-policy change; it does not relax any other gate.  For cellular,
the same value is accompanied by the one mechanical change
`hardware_eligibility=eligible`.  The cellular parser already accepts
`eligible`; all strict transcript, watchdog-before-first-lifecycle-write,
supervisor-before-candidate-scanner, always-rollback, and exact p22/p30/p31
checks remain unchanged.  Both regenerated packages must have new bundle
identities and must still state `hardware-proof=none` or `not-run`.

No package worktree was changed by this audit.
