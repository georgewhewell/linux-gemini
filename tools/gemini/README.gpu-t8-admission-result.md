# T8 admission failure — closed hardware result

Candidate boot `9fadb841-4981-4090-9ec9-795168f948ae`, exact T8 release/token.
Image `4ebd5671160df6a24bb0aea37a259d7d47d3201f86b746dd37d4b01e12c6ab4f`
was written once. This image is consumed; do not retry it.

Saved first-event evidence:
`handoff/gpu-owner-t8-lzOqXg2i/dmesg-first-event.txt` in the workspace.
At 1.162318 seconds scpsys probe returned -EBUSY. At 1.167408 seconds,
`_regulator_put()` warned during devres cleanup. Panfrost subsequently deferred
with -EPROBE_DEFER, and the collector rejected missing successful initialization
and render node. No renderer was uploaded or dispatched; this is not a new
GPU_SHAREABILITY_FAULT observation.

## What the warning establishes

In exact source `3606f6b513117e630601dcd97e8c4c6eca1f566b`,
`drivers/regulator/core.c:2657` warns on nonzero handle enable_count.
Exclusive acquisition at lines2502–2522 sets enable_count=1 when the regulator's
is-enabled callback returns positive. The new scpsys initialization acquires
that handle before its off-only admission gate; it has not yet called the
owner's regulator-enable operation. This exposes inherited enabled-regulator
accounting that the host admission mocks did not model through devres cleanup.

The warning is not proof of the six GPU domains' physical state, nor proof
that the regulator was disabled by cleanup. `_regulator_put()` destroys the
handle and warns; it is not a regulator-disable call. Do not claim a safe
ownership handoff merely by suppressing the warning or dropping the guard.

## Next source review

Resolve actual inherited-rail ownership and failure lifetime using the regulator
core contract and working vendor initialization order. In particular, vendor
r12p1 platform init writes GPULDO setup at lines708–714 without an adjacent
rail-off check; whether off-only initialization is necessary must be established,
not assumed. An orderly quiescent normalization and vendor-equivalent setup
under inherited rail power are alternatives to evaluate, not authorized changes.
No separate observer-only boot or live takeover is authorized.

## Recovery closure

Supervisor2324990 started08:18:04 before writer2325140 at08:18:05; candidate
write completed08:18:14. RAM evidence was fsynced before verified petter111
was stopped at178.20 uptime. The supervisor verified exact rescue at08:22:04;
original owner execution60897 terminated exit1 at08:23:27 with
`observation_failed_recovered_stage_a_pstore_captured`.

Owner receipt `handoff/gpu-owner-t8-lzOqXg2i/RESULT.md` records rescue boot
`b94758f3-e7f3-4aca-bee8-20f1e2aeb69a` through USB and RJ45, CPUs0–7,
no failed units, exact p22/p30/p31 hashes, transaction processes gone and
both locks free. Immediate and harness pstore archives have different member
paths but byte-identical console SHA256
`72d71a2b2a1962cf73995acd1dd2149cce3d15c7f46695acd6794e4b9ead69f9`.
No manual reset, deadline extension, retry or renderer dispatch occurred.
