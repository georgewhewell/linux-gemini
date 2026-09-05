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

At this record's publication, bounded read-only closure through the original
backend and supervisor is authorized but not yet verified. It must prove the
new accepted boot, all three partition hashes, CID, baseline watchdog policy,
USB and RJ45 access, pstore preservation, and process/lock closure. Preserve
the failed automatic receipt and label any subsequent manual closure as such;
neither recovery nor manual closure would qualify the retention experiment.

Host-only follow-up capture diagnostics are isolated at `8372060` in
`repo-a72-retention-capture-diagnostics`, not applied to the running owner.
They identify failed stages on stderr without dumping device data, preserve
the failure code, and keep changed-boot refusal off archive stdout. Target
BusyBox capture, backend and full owner tests passed independent review in
`handoff/a72-capture-diagnostics-review-20260905.md`. No retry is authorized by
these diagnostics or by this result record.
