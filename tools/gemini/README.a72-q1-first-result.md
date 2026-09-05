# A72 retention Q1: first attempt incomplete

The first authorized Q1 transaction did not qualify retention and did not
attempt to bring CPU8/9 online. The candidate boot reached SSH, but the first
full evidence capture returned 1 before any seed operation. Its stderr did
not identify the failing gate. This is not evidence of a failed retention
snapshot or of an A72 hardware failure.

Authoritative host records are in
`/mnt/Home/src/nixos-gemini/handoff/retention-q1-authorized-20260905`.
The executed owner source was `09a0e35e8483663fea32a905c11c82c4a3559770`;
authorized contract SHA256 was
`472ca21bb4c34d4fb30e0f78466c7864a87d6c973f8a83fd4a4068b155077449`.
Candidate boot UUID was `87272cb1-a982-4064-9c13-bbf8aff0df66`.
Command0028 (channel request0007) records the failed capture. No seed request
was submitted. The workflow permanently selected accepted rescue S.

The existing supervisor subsequently recorded a successful exact-S write
in event0022. A successful write is not recovery closure: the automatic
final capture failed at the host gadget-identity gate before SSH, and the
orchestrator exited 1 with `failed-recovery-unverified.json` preserved.

Source review identifies a separate recovery bookkeeping defect:
`wait-accepted` skips `wait_ready('S')` when `last_role == 'S'`. The failed
candidate capture never updated `last_role`, so it still described the
initial baseline S, not the newly flashed rescue. Thus the automatic path
could attempt capture before the rescue gadget appeared. This explains the
missing readiness wait, not the original candidate capture failure.

Bounded read-only manual closure through the original backend and supervisor
subsequently succeeded. Client77613 exited0; the original orchestrator94866
and backend43652 exited1, preserving the failed automatic result. The original
supervisor2981244 was reaped, its session empty and both locks verified free.
No new writer, reboot, seed, source change or deadline extension was used.

Accepted boot `1213eab8-535f-4fed-b799-69fde4a79d21` matched USB and separate
RJ45 SSH. All three hashes matched their original values; CID, release,
cmdline and baseline `mtk-wdt/inactive/NOWAYOUT0/31` passed validation.
The manual archive digest is
`2947699706058f6e467c719780e9714702ecb4094fd178abfa863a1407683627`;
retained pstore tar SHA256 is
`1d533ed43be53cb83de72f26050e3b09ea0672fdbf48f69f7a50c508f8aadd63`.
See `owner/manual-accepted-closed.json` and `owner/manual-accepted-archive`
under the run directory. Root independently checked the raw partition hashes,
RJ45 boot identity, watchdog tuple, pstore hash and absent original processes.
The failed automatic receipt remains preserved. Manual recovery closure does
not qualify retention; no A72 CPU transition occurred. The fixed final capture
did not query systemd failed units; do not infer that additional health check.

Saved pstore gives a concrete storage-dependency lead: PMIC-wrapper probe
failed with -EINVAL at 0.777285s, before eMMC repeatedly deferred through
11.269642s. Independent source/carrier review found Q1's wrapper reset ID64
incompatible with the kernel's compact reset IDs0/1. Separately, the enabled
DVFSP-handoff node lacks its required SCP attestation resource; its probe
failed at 1.218630s and left I2C6 deferred. These are separate dependency
paths, not proof of the exact silent capture substep.

A corrected reset mapping alone is not a hardware-ready successor: it would
permit wrapper initialization earlier than the observed Linux watchdog probe,
and this Q1 kernel lineage lacks the later integrated wrapper watchdog guard.
Any successor must reconcile that write boundary as well as validate the
compiled carrier against its actual kernel providers. Keep consumed Q1
artifacts unchanged and review a distinct successor before another boot.

Host-only follow-up capture diagnostics are isolated at `8372060` in
`repo-a72-retention-capture-diagnostics`, not applied to the running owner.
They identify failed stages on stderr without dumping device data, preserve
the failure code, and keep changed-boot refusal off archive stdout. Target
BusyBox capture, backend and full owner tests passed independent review in
`handoff/a72-capture-diagnostics-review-20260905.md`. No retry is authorized by
these diagnostics or by this result record.
