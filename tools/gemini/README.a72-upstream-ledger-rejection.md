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

## Upstream precedent and offline checker

The upstream `2026-08-28-mainline-a72-cpu8-admission-candidate` experiment
documents a live `DBGC/start=130/size=130` stale header and an exact two-word
initialization to logical-empty. Its `initialize-transition-ledger.sh` pins
that device's prior prefix hash and Gemian identity. It must not be run on
our unknown state. This precedent strengthens the preparation hypothesis;
it does not establish that our retained words are also 130.

The old upstream Python decoder caps stages at 9, whereas our selected kernel
accepts stage 10. Use the actual-code offline checker for our source:

```
HOSTCC=<native-gcc> python3 tools/gemini/check-a72-transition-ledger.py \
    /path/to/linux-gemini-a72-upstream-oneboot [saved-slot.bin]
```

It compiles the selected kernel's admission/CRC/record functions and headers,
checks 12 raw/empty/stale/corrupt/stage/generation fixtures, and optionally
classifies an exact 84-byte prefix or 4096-byte saved slot. Output includes the
capture hash, header and exact admission errno. Native write/sync callbacks
abort: this tool cannot initialize or clear retained RAM. Passing this check
does not prove the record stays unchanged across reboot or CPU admission.

## Live rescue layout verified after T3

Read-only collection on rescue boot `344393d0-5bf3-4917-8951-a8de58a066dc`
confirms the active ramoops module parameters and live DT match the upstream
layout: base `0x44410000`, size `0xe0000`, record/console/ftrace/pmsg sizes
`0x1000/0x10000/0x1000/0x20000`, memory type zero. A layout-conflict claim is
therefore unsupported. The live DT SHA256 is
`0b65c19394fdd9da631ac57f3fc6e7f0beb2d20ee8d20598a766c221422c3bbf`.
Evidence is in `handoff/gpu-thermal-t3-gcstVt3C/rescue-live.dtb` and
`rescue-ledger-readonly-inventory.txt`.

The rescue exposes neither `/dev/mem` nor a raw retained-record interface.
No device node or module was added, and no retained bytes were changed.
The next source task is a default-off fixed-slot snapshot with separately
reviewed, explicit compare-before-write preparation of recognized stale
conventional pstore records only. Unknown or corrupt transition records must
remain rejected. The intent is evidence capture and, only when justified,
preparation followed by the complete CPU workload in the same boot—not a
separate observer-only boot or automatic reset-on-error behavior.

## Reviewed preparation successor

The fixed-slot facility is implemented in source `37489020c0315`, with the
conditional configuration identity followup `9656543cfd045`. Package
`repo-a72-ledger-prepare@e851727` builds candidate
`e8348d4f34acba0051d5e378df0ede4d08f57bc1c5828b7b5c50c1f2e2001613`.
Root independently checked the candidate hash, identity-only followup,
archival/prepare/readback tests and runtime adapter refusal of unknown slots.

After C1 recovery closed, the sole hardware owner was authorized for one
complete bounded transaction (`handoff/a72-ledger-prepare-rBRTJPog`). A saved
4096-byte slot must be archived and classified by the actual kernel C before
any preparation. Raw, empty or valid ledger states skip preparation. Only
recognized conventional stale text may receive one exact compare-before-write
preparation, after root reviews that same-boot capture. The readback must
preserve every byte except the two zeroed header words and pass admission
before the original CPU token is used. Unknown state remains a refusal.
This is authorization and host-test evidence, not a CPU runtime result.

The successor reached SSH on boot
`4ed487f6-c236-4af7-a8d5-df60fdadf8b3`. Its first exact 4096-byte snapshot
(`ledger-before.slot`, SHA256
`45a28dce2af652cb2f8af31afe9c7075acb3003a52869cc0a5eac7976298d6f7`)
begins with ASCII `<<<RAM_RW>>>`, not DBGC. Root independently ran the actual
C classifier: header words `[1379679292, 1381977409, 1044266583]`, admission
`-74`, stale-text eligibility false. No preparation or CPU token was used;
the owner captured evidence and began exact recovery.

Some historical package research calls that string a console circular-wrap
marker, but that attribution has not been established in the selected
kernel's pstore implementation or the inspected vendor/LK source. It is not
permission to initialize this slot. The captured bytes now replace the
earlier stale-DBGC hypothesis for this boot; marker provenance and retained
slot ownership need evidence before selecting a successor preparation policy.

The original transaction closed at 04:09:06 with exact rescue, all partition
readbacks, USB/RJ45 health, no failed units and both locks free. Recovered
Stage A boot is `3229b943-62cd-44b7-aa91-d575983c46fe`. Pstore SHA256
`84021ee5a6f341f8cf919e9f352d3fead6a16ed36ca488bc5e06d9469b75444f`
is attributable to prior Stage A, not this A72 candidate: the ledger profile
intentionally bypasses normal ramoops registration. The same-boot RAM archive
and raw slot, not that recovery console, prove this candidate's refusal.
