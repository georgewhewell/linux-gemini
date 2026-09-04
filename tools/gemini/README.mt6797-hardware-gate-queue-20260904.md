# MT6797 managed hardware-gate queue

Date: 2026-09-04

Status: offline qualification and queue ledger only.  Nothing in this note
authorizes or schedules a flash, boot, reset, USB operation, remote-host
operation, radio operation, or hardware attempt.

## Authority and common boundary

The device owner stated exactly **"power is fine, there is no power
blocker"**.  For this queue, that current owner attestation is represented by
the exact manifest token:

```
power_preflight_policy=owner-attested-20260904
```

This is owner authority for the common power preflight only.  It is not a
battery, voltage, capacity, charger, cable, or connector measurement, and it
must not be inferred from BQ identity, USB enumeration, or SSH reachability.
Contrary physical evidence, loss of the recovery power path, or a later owner
report that the condition changed closes the queue until the owner reassesses
it.

No candidate below has specific execution authorization or a scheduled
hardware window.  Offline qualification is not permission to write p22.  A
future transaction still requires separate owner authorization naming one
exact candidate SHA-256 and must reconfirm the accepted Stage-A rollback
SHA-256 immediately before the write:

`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`

The detailed authority record and package-policy audit are in
`tools/gemini/README.mt6797-power-owner-attestation-20260904.md`.

## Current reviewed artifact identities and disposition

These entries are immutable identities, not a run order or schedule.

| Lane | Source commit | Package commit | Candidate SHA-256 | Offline status |
| --- | --- | --- | --- | --- |
| MD1 post-vector observer | `8faeb854c00524a2a39450f9c72aaa3601e1c2a5` | `c56ea7a569a8ce7a29f602fdb1c8ca1f0fb4690f` -> `718b9d1849d632b2135a2da23d43a337a1efab59` -> `5b45870571e2dfbf0dd06731001ce9a624ad085e` -> `7194ebaec4b36cf5d46075cc4d1836254c2b196d` | `777c182860863009174817e1d57970fc9fa8eb6f0bcc416803ba7ea8952a3213` | Independently reviewed: PASS |
| G3 fixed-low Panfrost no-job | `ec191faadb50d01a709fcb7786c4ea530839b7d8` | `f51d661e823d9ece2011c5688141ddaa7e3c44b6` | `d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985` | Offline source/contract review: PASS; package is not hardware-ready and requires repackaging |
| A72 provider/resource observation | `301a95567a799bbb3c81eba6653acc7491050b88` | `7f528b23b8a10a5338229449c26d7da1cc18a6c8` | `da66b64a37bc3e8c9695f0181fde45e8084179aacab55b5e4aae36c315ae98a5` | Independently reviewed: PASS |
| A72 CPU8-admission observation | `301a95567a799bbb3c81eba6653acc7491050b88` | `7f528b23b8a10a5338229449c26d7da1cc18a6c8` | `fd2c457aee5aedd3d736acd3f68463c20c0a07db9334d81d0376b2e3ef164dd3` | Independently reviewed: PASS; provider pass plus exact rollback required first |

All current packages carry
`power_preflight_policy=owner-attested-20260904`.  They retain no specific
p22 authorization and no schedule.  The cellular manifest additionally
retains `scheduling=none`, `specific-p22-authorization=none`, and
`hardware-proof=none-offline-build-only`.

### Cumulative-tree replay boundary

The package rows above intentionally retain the exact reviewed lane source
identities.  The managed cumulative tree replays the same defensive A53
source delta, complete A72 transaction series, and G3 lineage's unique
retained-entry token plus final source delta.  Those integration commits
compose the independently developed changes without pretending to be the
source revisions embedded in the immutable bundles; their replay hashes may
differ from the immutable lane hashes and are recorded by integration
history, not as package inputs here.

The cellular source remains in its separate post-audit source lineage.  Its
final watchdog-gate commit depends on the earlier cold-owner and CCIF/CLDMA
observer files that do not exist in the cumulative tree, so importing that
tip alone would be incomplete.  The exact external source and package pin is
therefore deliberate, not an alternate source identity or permission to
substitute a cumulative build.

### Retired A53 registration/calibration observation

The exact previously offline-qualified bundle was:

`/nix/store/x5m20gna2w7mzalywikvk92bs16g26hg-gemini-a53-thermal-preflight-p22-bundle`

Its source was `d772bb04b727101ae089012c253753ce70d6221a`, its package was
`b1fa9539561dee6674fa6a839d7f2f4851f623f2`, and its candidate was
`baf0f1fdcb581f19f6dc55bf21f6f86b5ac11ba5a54e5b8ff7a35c645d2a90c0`.
Independent review passed the source, package, hash, execution-owner,
watchdog, rollback, and strict observation contract offline.  The subsequent
hardware collector failed, so this exact candidate is retired and
closed/no-retry.  The earlier offline PASS is not a hardware pass or evidence
of production readiness.

The retained capture is
`/mnt/Home/src/nixos-gemini/handoff/a53-thermal-p22-d772bb04-baf0f1fd-one-shot-20260904T211500+0200`.
Its local recovery record is
`/mnt/Home/src/nixos-gemini/repo-a53-thermal-remediation/artifacts/p22-recovery-20260904-210458-1216393`,
mirrored at
`/mnt/Home/src/nixos-gemini-flash-p22-20260829/p22-recovery-20260904-210458-1216393`.
All 15 primary validations stopped at the exact `grep 'Gemini'` model check;
`dmesg-pre` records `Machine model: MT6797X`.  The retained pstore console at
lines 216--218 records the `mt6797_mp0` clock-provider `-EINVAL`/`-22`, lines
269--274 record deferred DVFSP, thermal, and cpufreq probes, and line 276
records `capture=fail`.  It contains no panic, Oops, or BUG marker.

Recovery completed with Stage A running.  `pstore-post.status` is `complete`,
and `recovered-partition-hashes` records exact p22
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`,
p30 `122c0f461e94d3439016e80339c7ee96f865e3591a3e2fbd163a7769dc0ac159`,
and p31 `1fa78de9f8744a6818bcef2f6773737939f84364de982413910d4958d6d21513`.
The bundle's pstore helper incorrectly addressed `/bin/busybox`; recovery
therefore required a resume-only in-memory path substitution.  This is a
recorded package defect, not permission to reuse or retry the candidate.

Evidence:

- `/mnt/Home/src/nixos-gemini/repo-a53-thermal-remediation/research/A53_DVFS_THERMAL_SOFTWARE_PREFLIGHT_20260904.md`
- `/nix/store/x5m20gna2w7mzalywikvk92bs16g26hg-gemini-a53-thermal-preflight-p22-bundle/p22-one-shot.manifest`

### MD1 post-vector CCIF/CLDMA read-only observation

The final candidate bundle is:

`/nix/store/b1kgbvpa2rlkp7w8d0kzh618m6j173kd-gemini-md1-cldma-readonly-observer-postaudit-p22-bundle`

Source remains exactly
`8faeb854c00524a2a39450f9c72aaa3601e1c2a5`.  Package remediation progressed
through the complete lineage recorded in the table: initial hardened package
`c56ea7a...`, shared-predicate remediation `718b9d18...`, conservative
CCCI/radio alias hardening `5b458705...`, and exact owner-policy manifest
update `7194ebae...`.  The resulting candidate SHA-256 is
`777c182860863009174817e1d57970fc9fa8eb6f0bcc416803ba7ea8952a3213`.

The package is locally built and tested offline.  Its shared transcript
predicate accepts the required explicit no-transport line while rejecting
affirmative CCCI, SIM, eUICC, RF, radio, driver, interface, and service
activation forms; the same table-driven fixtures exercise the safety
executable and full acceptance helper.  It retains the supervisor-owned
launch, under-lock rehash, watchdog-before-first MD1/EMI write, strict ordered
5,000-poll/at-least-five-second grammar, fatal-kernel rejection, and exact
p22/p30/p31 rollback contract.

Independent review reproduced the exact bundle with local offline builders,
verified all 15 bundle checksums, passed the package's full acceptance and
recovery harnesses, and passed an additional adversarial predicate matrix.
It remains unscheduled and unauthorized for hardware.  Its owner token is
exact, while
`scheduling=none`, `specific-p22-authorization=none`, and
`hardware-proof=none-offline-build-only` remain explicit.

### G3 fixed-low Panfrost no-job gate blocked for repackaging

The exact independently reviewed owner-policy bundle is:

`/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle`

Its source/package/candidate triple is the G3 row above.  Independent review
passed the source, package, hash, execution-owner, watchdog, durable scanner,
rollback, and no-job acceptance contract offline.  Hardware has not been run
by that review.  It binds Panfrost once at the fixed-low OPP and submits no
job.  No GPU job, other OPP, DVFS, thermal, suspend, or render-node experiment
is eligible from this result.

A later hardware-readiness audit found two packaging defects shared with the
retired A53 bundle: the primary validator expects model substring `Gemini`
while the retained runtime reports `Machine model: MT6797X`, and the recovery
harness's pstore helper directly invokes `/bin/busybox`, which is absent in
that environment.  The source/contract review remains useful offline, but
candidate `d1d188f0...` is not hardware-ready, must not be scheduled, and is
closed/no-retry.  Repackaging must produce and independently review a new
immutable candidate identity before this lane can re-enter the queue.

Evidence:

- `/mnt/Home/src/nixos-gemini/linux-gemini-g3-audit-decision/tools/gemini/README.mt6797-gpu-g3-audit.md`
- `/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle/MANIFEST`
- `/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle/p22-one-shot.manifest`

## A72 provider and CPU8-admission observations

The remediated, independently reviewed provider bundle is:

`/nix/store/r7wnz7m86739dj5l8wjlwinhj3z0d8z5-gemini-a72-provider-resource-preflight-p22-bundle`

The distinct CPU8-admission bundle is:

`/nix/store/iqb6k8mvra3gsjj47kmhzlv1x0n3xan4-gemini-a72-cpu8-gate-preflight-p22-bundle`

Independent review verified both exact NARs, every embedded checksum, strict
21-sample/at-least-60-second parsers, watchdog-before-access enforcement, the
46-case recovery harness, immutable supervisor ownership, and exact Stage-A
p22/p30/p31 plus service/watchdog/pstore proof.  The CPU8 gate mechanically
requires successful provider evidence bound to candidate `da66b64a...`,
source `301a9556...`, package contract `fe16911c...`, and the exact completed
Stage-A rollback before it is even eligible for separate consideration.

The provider and admission observations remain separate, mutually exclusive
boots.  Neither requests CPU_ON, enables CPU9, changes an OPP, or exercises
cpufreq.  Both remain `hardware_run=not-run`, `authorization=none`, and
`scheduling=none`.

The old provider candidate `6ddc55bf...` and old admission candidate
`cc8b3ab8...` remain closed in the no-retry registry below; remediation does
not reopen those identities.

## Wi-Fi open-auth lane is closed before assembly

The current clean software pins are source
`e908db73076dbe95bd72da9ee85512241a809f77` and package
`dbbb2f88fa60a382d7ad8af5847daac99798c1f1`.  Their object-level qualifier,
scan-HIF path, single open-auth caller, timeout arbitration, and recovery
packaging are retained offline.

No independently reviewed AP identity or manifest, matching full candidate
bundle, or retained qualification-result directory was found in the recovered
session, local worktrees, known build directories, or Nix store.  The only AP
file is `qualification/mt6797-wifi-ais-auth-hif/AP-MANIFEST.schema`, which is a
schema and explicitly forbids guessed values.  The assembler fails closed
unless an external reviewed manifest and matching candidate provenance are
provided.  Therefore this lane has no candidate SHA-256, authorization, or
schedule; assembling one now would require inventing identity.

## Closed/no-retry candidate registry

Every candidate hash below is superseded, completed historical evidence, or
rejected.  It is closed and must not be scheduled, retried, or substituted
for a current identity.

### A53 retired candidates

- `baf0f1fdcb581f19f6dc55bf21f6f86b5ac11ba5a54e5b8ff7a35c645d2a90c0`
- `50f3cca869b43cadd32e5dd97b1d848004080ff93234da8962817093db5fd5b0`
- `e0566d596d4b0be3e92b77215b0269251734da8da62cfadadf1a7eed5991362c`
- `8f65e46a94e288b2fed585647822cab6b356493b19c4b7de9edc4612deee1139`

### A72 old packages

- `6ddc55bfb8d79e8442644812582dc58643d87eb8223007982a821f1d67e0a803`
- `cc8b3ab8864766138a9697ffae79b2c86204cef086eabdeabfca6179b2ca57e0`

### Cellular predecessors

- `34689b1814e2a3ce12eb248d7e6594c57e9c661d8a9e1e3b38f9942bf30d184c`
- `edb4876686c6c18d534b3966948490280299d02fbd141f08e4f8fa4457f0f6cf`
- `40ce0860e3871476d1c3fc4f4888d7e7e54755445ec96d7479df510bab2d0f4a`
- `7147a4e4e212907dce103a45459807ac089f5f30fd350991df678bdff575e973`

The full cellular transport source/package tips remain outside this gate and
closed: they add firmware, transport, IRQ/NAPI, SIM/raw-IP, and optional eUICC
surfaces and are not an alternative to the read-only observer.

### GPU/G2/G3 historical candidates

- `d1d188f0a94ae00b4a4697cac56ccf066fc25e344caf9c7e3e94bd91552bf985`
- `9a667439e951297d931583a6629486b2c3fc292cdc885b249fdd07fb5f7fefbb`
- `002d312d2be67054f6a399b1ec3a715182c909819f05b22f6dd1f9a41e364f0f`
- `75e8b4c5907bda9de3b5f8c6caf7cc82fd635c503677d7c52e9e8fae22217f5b`
- `33f1d5c570fb4bc78cfb256c8498110d945b075452f1ef6f050bdf18ebf5335c`
- `303d37064beb76594c48e1a96fb1db33272fd15a9a7af0e6226b049e887f8b14`
- `791ca56fd75a2a2aab64626b30273be7b159d4f1ca31712900c2ebcfb34a5a63`
- `1570f2abdc3b40f906646ede60722d7674909426b57d27b5366ba3b688d4d725`
- `50191571f9bb678362f20caee51449eb5d52bcadf798b901934367f96f6f8dec`
- `e67c5964cf67a23d975b1976523be5dedfb3b309e9f39387bff8ea640af1d030`
- `21c6631a4c75107beeba9776b8f3497b5cc1073095a111d7c29a1cdf546cc71e`
- `b779a03a8f271ced26a6edb8a1ff3e50f7128f8dcb0cf6a64d3aeb1448460bcc`
- `6a82d410ce1171e06d53c705ff01b7dec9e57260c38ab52bc872eb25568cfa25`
- `dcc4897d3ad2b64e762ce41bdd2ae90ca2f286588d5a9c78f9fc32bf8dbc46d7`
- `af5f45a155317ab829e73d96711403c61d4a23a5cd2e34e597e357132b203bde`

The G2 entries include completed retained evidence; they are not repeatable.
Some historical GPU entries identify packaging-only rungs, not functional
hardware evidence.  Both categories are closed/no-retry.

## Mutual exclusion and stop rules

All lanes contend for the same physical device, p22 target, USB/preloader
catcher, watchdog, and Stage-A rollback path.  At most one independently
reviewed and specifically authorized exact candidate may own them.  A future
transaction must use a fresh nonexistent capture directory, arm its
independent rollback owner before the write, perform exactly one candidate
write, and restore Stage A after every result.  Exact restored p22 and
unchanged p30/p31 readbacks are prerequisites for considering another gate.

Do not batch candidates, combine overlays, carry a candidate into a second
test, or interpret a timeout/reset as permission to retry.  Any missing
marker, permission-owner loss, wrong hash, unexpected write, watchdog or
rollback failure, hard hang, or need for manual recovery closes that exact
hash.  Re-rank only from newly retained evidence; never advance
automatically.
