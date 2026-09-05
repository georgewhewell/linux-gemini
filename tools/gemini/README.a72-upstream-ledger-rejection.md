# Upstream-derived CPU8 rejection: retained ledger admission

Examined source: `400b1abd630326a6b52de881cb7aa70599bd416f` in
`linux-gemini-a72-upstream-oneboot`. Hardware evidence is under
`handoff/a72-upstream-oneboot-h2C8RrjU` in the workspace, especially
`runtime.txt` and the saved RAM `controller-status` and `dmesg-final`.
Candidate `fa148df6c9d547c53321165d6369ec5682aaacddd4c0abc2d1cc522aac87a099`
is consumed; do not repeat it.

The boot reached Linux with pristine admission and completed derivation.
CPU8 returned -74 (`EBADMSG`), while binder lifecycle, attempted and
watchdog_armed remained zero. CPU9 and CPU_OFF were never requested.

## Source boundary

`arch/arm64/kernel/mt6797_psci.c:mt6797_psci_cpu_boot()` routes initial CPU8
startup to `mt6797_a72_binder_cpu_boot()`. The production binder calls:

1. `mt6797_a72_membership_claim_cpu8()`; its return set is zero, `EINVAL`,
   `EOPNOTSUPP`, or `EPERM`, not `EBADMSG`.
2. `gemini_transition_ledger_begin()`; this can return `EBADMSG`.
3. Provider availability, then `mt6797_a72_transition_begin()`.

The transition's pre-attempt checks return `EINVAL`, `EPERM`, or `EALREADY`.
It sets `attempted=true` before checkpoint/watchdog/physical effects. The
observed result therefore strongly isolates ledger admission, not failed
CPU power sequencing. This is call-path inference, not a captured raw header.

`fs/pstore/gemini_transition_ledger.c` maps the first 4 KiB at `0x44410000`.
Admission accepts only:

- Signature/start/size all `0xffffffff` (raw).
- Signature `0x43474244`, start and size zero (empty).
- That signature with start=size=72, plus a valid retained dual-copy record.

Other headers or invalid committed records return `EBADMSG` without taking
ledger ownership. A conventional retained dmesg record is not automatically
a valid transition ledger. A corrupt record must not simply be called empty.

## Retention and next evidence

With `CONFIG_PSTORE_GEMINI_TRANSITION_LEDGER=y`, `fs/pstore/ram.c` deliberately
returns from `ramoops_init()` before registering the normal driver. The lack
of candidate console registration is intentional, not proof of missing Linux
entry. Recovered pstore contains the preceding Stage-A console; only the
off-device RAM capture proves this candidate's failure.

The accepted input DT's reserved region has the same base, size and partition
properties required by the upstream ledger guard. That does not establish the
running rescue's effective ramoops layout, boot-argument overrides, or the
three raw words seen at ledger admission. No raw-header capture is available.

Before selecting a correction, preserve and compare the actual rescue layout
and ledger header/record bytes with upstream preparation. Do not erase an
unknown retained record, disable integrity checks, or claim an A72 hardware
failure from this rejection. Any successor should preserve this boundary's
evidence while retaining the complete CPU8/CPU9 workload protocol.
