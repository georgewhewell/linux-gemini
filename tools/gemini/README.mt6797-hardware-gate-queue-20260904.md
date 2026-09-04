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

This note never grants specific execution authorization or schedules a
hardware window. Offline qualification is not permission to write p22. Each
transaction recorded here required separate owner authorization naming one
exact candidate SHA-256 and immediate reconfirmation of the accepted Stage-A
rollback SHA-256 before the write:

`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`

The detailed authority record and package-policy audit are in
`tools/gemini/README.mt6797-power-owner-attestation-20260904.md`.

## Current reviewed artifact identities and disposition

These entries are immutable identities, not a run order or schedule.

| Lane | Source commit | Package commit | Candidate SHA-256 | Disposition |
| --- | --- | --- | --- | --- |
| A53 registration/calibration observation R2 | `f2fc2447a7465b96b9816487d6c36074cf2ca4fb` | `5dca07f1fd7a83dcbf8b905353725fb85619da3f` -> `8a62fff69574a8555baca600baf898fa2d02bd4c` | `e123c1984d46ee7609a98ac087232dfb13ac9e8d55f6540c94c1ee2e037c0a56` | Hardware FAIL; exact Stage-A rollback/readback complete; retired/no-retry; CSPM ownership remediation required |
| MD1 post-vector observer R1 | `8faeb854c00524a2a39450f9c72aaa3601e1c2a5` | `7027acfa7ad57242a0bf930559088bfc11a8f285` | `76387822474c35d55fb4804b670166e5279c239d965418481733f6998c2c1c16` | Reminted offline package and rendered BusyBox-ash recovery checks: PASS; hardware not run; independent live-readiness audit required |
| G3 fixed-low Panfrost no-job R1 | `ec191faadb50d01a709fcb7786c4ea530839b7d8` | `f51d661e823d9ece2011c5688141ddaa7e3c44b6` -> `a81879abb63a7ad7c40bc946b3c95bf1657edef7` -> `02b36ca602774b032e633bbfec103ea6a1c4baec` | `78342f989983ac7de48abbc5b9c0763dcc600421df4e418508d48c2888551d1b` | Hardware FAIL; exact Stage-A rollback/readback/pstore complete; retired/no-retry; efuse/OPP and MFG-domain remediation required |
| G3 fixed-low Panfrost no-job R2 | `f3f6dbb4ec9b4f95854c2a3df5a2c842559116b4` | `16e9ee2378f53803ea9dd32eb0eaf8036edb5bdb` | `ed85f692394ebd4924ec582feb2f354d7a156eeb5cd6c6145c20be14b85a72a3` | Efuse linkage and SCPSYS VGPU ordering remediated; full offline build/fixtures PASS; hardware not run; independent audit required |
| A72 provider/resource observation | `301a95567a799bbb3c81eba6653acc7491050b88` | `7f528b23b8a10a5338229449c26d7da1cc18a6c8` | `da66b64a37bc3e8c9695f0181fde45e8084179aacab55b5e4aae36c315ae98a5` | Offline contract review: PASS; source/package are not hardware-ready |
| A72 CPU8-admission observation | `301a95567a799bbb3c81eba6653acc7491050b88` | `7f528b23b8a10a5338229449c26d7da1cc18a6c8` | `fd2c457aee5aedd3d736acd3f68463c20c0a07db9334d81d0376b2e3ef164dd3` | Blocked with provider; no provider-success predecessor exists |

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

The first unrun replacement bundle
`/nix/store/fp5pi0i1yxmfpbhnh6qmdlxyz9q8c2xl-gemini-a53-thermal-preflight-p22-bundle`
was also rejected before scheduling.  Its candidate `5529a03b...` fixed the
clock, NVMEM, and board-identity defects, but its post-rollback selector
accepted only a resolved path ending in `/bin/tar`; accepted Stage A resolves
that applet to its BusyBox executable.  The rendered A53 collector also still
contained the obsolete `d772bb04...` source predicate.  Candidate `5529a03b...`
was never written and is closed. R2 used a new cmdline identity and candidate
hash, pinned the corrected `f2fc2447...` collector identity, accepted only the
exact SHA-256-pinned Stage-A BusyBox alternative (or a root-owned non-writable
Nix-store tar), and carried that BusyBox in the bundle closure.

Independent audit admitted R2 for one exact attempt. Candidate `e123c198...`
was written once and booted the expected R2 kernel, exact board identity, and
CPUs 0--7, but the mandatory `a53_thermal_preflight=pass` marker never appeared.
The watchdog expired as designed. The rollback scanner's first BROM attempt
timed out and its second succeeded, for exactly two p22 writes total: one
candidate and one rescue. There were zero p30/p31 writes. Final Stage A booted
healthy with exact p22/p30/p31 readback.

The live pstore root cause is deterministic: the CPU clock provider reserved
the CSPM resource at `0x11015000` first, so the DVFSP handoff's exclusive
mapping failed with `-EBUSY`; the CPU0 processor-regulator lookup then returned
`-ENODEV`, `cpufreq-dt` deferred, and the collector failed. Candidate
`e123c198...` is retired/no-retry. Its immutable capture is
`/mnt/Home/src/nixos-gemini/handoff/a53-thermal-p22-f2fc2447-e123c198-one-shot-20260904T221000+0200`.
The recovered raw pstore console has SHA-256
`6e383949474b07679f113dc89a5312b8da3814adf003396e59c46d02f1d6914a`;
the focused excerpt has SHA-256
`70cc70446f1899da153b68af00ddc9f1e91a1656ecdcf1935d6ec8fab6f94ad7`.

Post-rollback collection also revealed that R2's single-quoted SSH body lost
its nested empty-string case pattern during local-shell rendering, producing
invalid BusyBox `ash` syntax. A provenance-bound resume performed no writes
and reproduced that packaging failure. The raw live pstore above was therefore
recovered separately and remains failure evidence, not a trusted R2 pass.
The next A53 package must use one explicit CSPM owner/provider, synchronize CPU
clock access with DVFSP lifecycle validation, mint a fresh source/package/hash,
and test the exact post-local-shell command under BusyBox `ash` before audit.

Evidence:

- `/mnt/Home/src/nixos-gemini/repo-a53-thermal-remediation/research/A53_DVFS_THERMAL_SOFTWARE_PREFLIGHT_20260904.md`
- `/nix/store/x5m20gna2w7mzalywikvk92bs16g26hg-gemini-a53-thermal-preflight-p22-bundle/p22-one-shot.manifest`

### MD1 post-vector observer predecessor and reminted R1

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
That review established the source and observation predicate, but a later
live-readiness audit found two packaging defects.  All four candidate
validators require model substring `Gemini` while LK exposes the exact model
`MT6797X`, and the recovery harness has neither run-scoped post-rollback pstore
collection nor a durable `--resume-run` path.  Its sole pstore read is a
best-effort pre-run console read.  Consequently the exact bundle cannot prove
recovery after a watchdog or connectivity interruption.

Candidate `777c1828...` was not run, is not hardware-ready, and is closed
pending a newly hashed and independently reviewed package.  The source's
watchdog-before-first-write, no-CCCI/radio/SIM predicate, and strict
5,000-poll/at-least-five-second read-only boundary remain useful offline; they
do not override the recovery defect.  Its owner token is exact, while
`scheduling=none`, `specific-p22-authorization=none`, and
`hardware-proof=none-offline-build-only` remain explicit.

The reminted R1 package commit is
`7027acfa7ad57242a0bf930559088bfc11a8f285`; its bundle is
`/nix/store/8y8zkniqbr0mdm56lr04qi8vlwizfzx9-gemini-md1-cldma-readonly-observer-postrun-remediated-p22-bundle`
and its fresh candidate SHA-256 is
`76387822474c35d55fb4804b670166e5279c239d965418481733f6998c2c1c16`.
It replaces model matching with exact NUL-delimited compatible tokens, adds
run-bound p22/p30/p31 plus pstore archive/inventory/status proof, and provides
an immutable package/manifest/candidate/rollback-bound resume path. Its
SHA-pinned BusyBox selectors have no PATH fallback and the fully rendered
remote bodies parse under BusyBox `ash`. Interrupt cleanup distinguishes a
retained scanner, confirmed rollback awaiting pstore, and terminal unconfirmed
rollback. Full generic and MD1 acceptance fixtures, shell checks, Nix parsing,
offline local build, and all bundle checksums pass. It remains hardware-not-run
and requires independent live-readiness audit before any exact authorization.

### G3 fixed-low Panfrost no-job old bundle blocked; R1 hardware result

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

The replacement package is
`/nix/store/xakzwxiz0jsvpxhjy4nqfvfnxx60mbi0-gemini-g3-low-opp-nojob-p22-bundle`.
It uses exact `planet,gemini-pda` plus `mediatek,mt6797` compatible tokens and
the exact SHA-256-pinned Stage-A BusyBox closure for normal and resumed pstore
collection, without a model predicate or PATH fallback.  Its new candidate
`78342f98...` differs only by a root-owned mode-0444 inert initramfs provenance
file; kernel, DTBs, cmdline, `/init`, and boot-header semantics remain
byte-identical. Local offline checks and independent live-readiness review
passed, admitting exactly one separately authorized transaction.

Candidate `78342f98...` was written exactly once. It never reached SSH
acceptance; the retained scanner caught a later natural BROM/reset window and
wrote the exact rescue once. No manual reset was performed. The
provenance-bound resume completed final proof with state
`observation_failed_recovered_stage_a_pstore_captured`. Final Stage A boot ID
is `a12002d9-e264-44b2-876a-49ce9252945b`; the system is running with zero
failed units, exact p22/p30/p31 hashes, and zero p30/p31 writes.

Pstore proves that the watchdog and no-job boundary held, then records GPU
efuse unavailable, unknown safe OPP, MFG power-domain acknowledge timeout
`-110`, and fatal Panfrost initialization failure. The collector correctly
rejected absent T880 identity/bind/IRQ/runtime evidence. No DRM node was opened,
no job was submitted, and there is no panic, Oops, BUG, or lockup marker. The
candidate is retired/no-retry. The immutable capture is
`/mnt/Home/src/nixos-gemini/handoff/g3-low-opp-nojob-p22-ec191faa-78342f98-one-shot-20260904T223000+0200`;
post-rollback archive SHA-256 is
`b39d691f1ffcaa101cf604a752b690122b866b7ae861bd0a725372e4309f2f0f`
and console SHA-256 is
`20ed96737c33dd7c3839d5f4997881eb35db3952b1d3d762f47977f8ea69f08c`.
The next rung must remediate efuse/safe-OPP identity and the MFG-domain timeout,
mint a fresh source/package/hash, and receive a new independent audit.

That remediation is now packaged, but remains audit-only and has not been run.
Source `f3f6dbb4...` restores the GPU efuse link and makes the SCPSYS MFG
transition own the VGPU prerequisite before clocks and `PWR_ON`. Package
`16e9ee23...` produced
`/nix/store/nd5lnghfa264cchx8fqkfl64brh7lkwl-gemini-g3-low-opp-nojob-r2-p22-bundle`
and candidate `ed85f692...`; bundle NAR hash is
`sha256-zdWZlWUW6/7JLgQ08xQbTci+mg4WEJrZEz2Ic9iCMGs=`. The full ARM64
target build, source contract, exact rendered-DT assertions, module plan,
parser, supervisor, recovery, and pstore fixtures pass offline. Its manifest
retains `authorization=none` and `scheduling=none`; it requires independent
readiness review before any separately authorized transaction.

Evidence:

- `/mnt/Home/src/nixos-gemini/linux-gemini-g3-audit-decision/tools/gemini/README.mt6797-gpu-g3-audit.md`
- `/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle/MANIFEST`
- `/nix/store/ygwllkhz39xv4a07m5ys9a479qfmv481-gemini-g3-low-opp-nojob-p22-bundle/p22-one-shot.manifest`

## A72 provider and CPU8-admission observations blocked

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

A subsequent live-readiness audit found four shared deterministic defects.
Both kernels enable a CPU clock provider whose path ops implement
`.set_parent` without the required `.determine_rate`, so clk core rejects
`mt6797_mp2` registration with `-EINVAL` before either observation can pass.
Both packages also validate model `Gemini` instead of the live `MT6797X`,
require an obsolete Dropbear `/run/probe-status` even though accepted Stage A
is switched-root NixOS with sshd, and cannot resume the required outer
post-rollback Stage-A/pstore proof after a harness interruption.

The provider and admission observations remain separate, mutually exclusive
boots.  Neither requests CPU_ON, enables CPU9, changes an OPP, or exercises
cpufreq.  Both remain `hardware_run=not-run`, `authorization=none`, and
`scheduling=none`.  Candidates `da66b64a...` and `fd2c457a...` were not run,
are not hardware-ready, and are closed pending new source/package identities.
The CPU8 lane additionally remains ineligible until a remediated provider has
passed once and exact Stage A has been restored.

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
- `5529a03b7c15ea8ed4f6bd6c1baafac3d244db08d5dc78e3d93f011018f6e08c`
- `e123c1984d46ee7609a98ac087232dfb13ac9e8d55f6540c94c1ee2e037c0a56`
- `50f3cca869b43cadd32e5dd97b1d848004080ff93234da8962817093db5fd5b0`
- `e0566d596d4b0be3e92b77215b0269251734da8da62cfadadf1a7eed5991362c`
- `8f65e46a94e288b2fed585647822cab6b356493b19c4b7de9edc4612deee1139`

### A72 old packages

- `da66b64a37bc3e8c9695f0181fde45e8084179aacab55b5e4aae36c315ae98a5`
- `fd2c457aee5aedd3d736acd3f68463c20c0a07db9334d81d0376b2e3ef164dd3`
- `6ddc55bfb8d79e8442644812582dc58643d87eb8223007982a821f1d67e0a803`
- `cc8b3ab8864766138a9697ffae79b2c86204cef086eabdeabfca6179b2ca57e0`

### Cellular predecessors

- `777c182860863009174817e1d57970fc9fa8eb6f0bcc416803ba7ea8952a3213`
- `34689b1814e2a3ce12eb248d7e6594c57e9c661d8a9e1e3b38f9942bf30d184c`
- `edb4876686c6c18d534b3966948490280299d02fbd141f08e4f8fa4457f0f6cf`
- `40ce0860e3871476d1c3fc4f4888d7e7e54755445ec96d7479df510bab2d0f4a`
- `7147a4e4e212907dce103a45459807ac089f5f30fd350991df678bdff575e973`

The full cellular transport source/package tips remain outside this gate and
closed: they add firmware, transport, IRQ/NAPI, SIM/raw-IP, and optional eUICC
surfaces and are not an alternative to the read-only observer.

### GPU/G2/G3 historical candidates

- `78342f989983ac7de48abbc5b9c0763dcc600421df4e418508d48c2888551d1b`
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
