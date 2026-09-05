# MT6797 cold-owner to CCCI readiness

Originally based on the full transport source lineage `9b501ccd5`; that full
transport and its subsequent reset/lease fixes are now in this integration
tree. No default enables, firmware loading, radio commands, SIM operations or
hardware transactions are added by this source change.

## Fixed race and lifetime

The cold owner publishes immediate post-vector GO synchronously, then schedules
5000 passive polls in process context. Previously CCCI treated GO with
`passive_complete=false` as permanent `-EPERM`; later publication did not
reprobe that failed consumer. A boot with valid final evidence could therefore
never reach ring ownership solely because probe ran early.

The new sleepable handoff API returns `-EPROBE_DEFER` before the supplier is
published, then waits at most 10 seconds for the asynchronous result. Observer
read errors wake it with the actual error. The timeout bounds a delayed or
missing observer; it is not proof the modem failed. The evidence is copied
under the same lock as publication. A managed consumer/supplier device link
keeps normal unbind and shutdown ordering; failed probes release their link
through device-core cleanup.

The link must report `DL_STATE_CONSUMER_PROBE` after creation. In this kernel,
`drivers/base/core.c:device_links_busy()` sets supplier `DL_DEV_UNBINDING`
under the same link lock as `device_link_add()`. A link created after that
barrier starts in `SUPPLIER_UNBIND` and is rejected, even when our devres cleanup
has not run yet. A link created before it makes the supplier busy; unbind waits
for the in-progress consumer probe. This closes the late-link race described
in `Documentation/driver-api/device_link.rst:59-69`. No supplier device mutex
is taken while holding the consumer probe lock. Link creation takes the core
link/PM locks under the evidence mutex; driver-core unbind releases its link
lock before invoking remove/devres callbacks, so those callbacks do not invert
that order. The published supplier is already functional: its one-shot power/
vector transaction and stage2 context are established before publication.

Supplier teardown marks immutable `-ESHUTDOWN` and wakes waiters before
draining passive work or releasing MMIO. Late work cannot republish readiness.
The worker checks shutdown between bounded polls. The HS1 stage2 borrowed
device pointer is disarmed under its existing lock before devres is released.
Pre-publication probe failures do not acquire this new terminal latch, so safe
pre-transaction deferral is preserved. Transport shutdown now uses its
idempotent remove/quiesce sequence; this does not claim hardware DMA stopped
until the existing stop-status checks establish that result.

## Admission deliberately unchanged

The original handoff-only patch required GO, completed passive observation, positive CLDMA register
activity and validated LK layout/remap before touching rings. GO is not HS1,
and neither GO nor a successful readiness wait proves HS1/HS2.

Vendor `drivers/misc/mediatek/eccci/modem_cldma.c:2154-2200` resets AP CLDMA,
powers MD, releases the vector, checks the AP-side clock when required, resets
the engine and starts rings. It does not require active AP queue/status bits
before installing those rings. The candidate's activity gate may therefore
exclude a legitimate idle engine. This patch does not merely delete it: clean
ownership needs a separately reviewed reset/clock lease and fresh idle/head/
interrupt snapshot, accounting for the cold owner's overlapping register
mapping and the vendor CLDMA clock condition. That is the remaining source
admission work at that revision, not evidence of a battery or firmware-download blocker.

The current integration has now supplied that missing ownership: exclusive
AO/PD reset controls, a single supplier-owned mapping lease after observer
drain, and nineteen fresh consumer queue/IRQ/busy/head reads before its first
write. It no longer requires circular pre-ring activity. See
`README.cellular-cldma-lease.md` and `README.cellular-integrated.md`; hardware
handshake and data validation remain outstanding.

## Verification and remaining hardware needs

`test-cellular-handoff-readiness.py` compiles the actual wait/publication/finish
C functions with pthread wait and device-link test adapters. It covers ready
before probe, pending result, timeout, allocation failure, observer error,
32 publication/wakeup races and shutdown versus late publication. Additional
source checks cover work-drain ordering and unchanged admission. These are
not in-kernel device-core, IRQ, NAPI or real MMIO tests.

Both modified driver translation units are cross-compiled. Hardware still
must establish current LK handoff, non-degraded power/PLL ownership, AP CLDMA
clock/reset/idle state, IRQ polarity, bounded ring startup and real HS1/HS2
before AT response or SIM visibility is claimed. Raw-IP service requires a
usable subscription and is separate from transport bring-up. No new boot is
scheduled by this source change; use the sole hardware owner and existing
bounded rollback workflow after the remaining ownership review.

## C1 artifact acceptance review

The separately enabled C1 package `repo-cellular-oneboot@88fa7da` pins kernel
source `0adbe79a26f783`. Its hardware target is HS1 runtime enqueue, HS2, and
one plain AT/OK exchange, not network attachment or packet data. The inherited
GPU success marker and no-radio observation policy were corrected to
`cellular_oneboot=pass` and `allow-platform` in the cellular manifest only.
The shared harness and GPU profiles were not changed.

The host validator checks saved evidence, exact candidate boot identity,
ordered handshake logs, exact OK and absence of terminal failures, then
fsyncs a hash receipt before optional publication through the hardware owner's
transport. Missing files, symlinks, rejected watchdog state and wrong expected
boot cannot publish. Root independently reran the actual host validator and
device acceptance-tail tests successfully. These are acceptance-protocol
tests, not evidence that the modem has booted on hardware.

Root reviewed the final bundle
`/nix/store/qg9zzk5lc3h72qv5pf6h29ik3q8qmb63-gemini-gpu-oneboot-p22-bundle`
and authorized the sole hardware owner for one complete bounded transaction.
Candidate SHA256:
`8674616e5d1863a464111cfdc3eea6a13b82a79e093ba9037586bd82ce6c8071`.
Manifest SHA256:
`96da6dfb7a17f9965fc6fbed51d879a48c9fed35fd57a6a0e67dcebe1dcbb41a`.
Host validator SHA256:
`426b0d662e3ef2d699db343c7786de5be8442d3b97d229c97c41dbc5677f924c`.
This authorization is not an execution result. The owner must preserve the
original transaction through terminal recovery and save exact rescue health,
run-scoped pstore and all three partition readbacks. No retry or reprobe is
authorized; the A72 successor remains queued separately.
