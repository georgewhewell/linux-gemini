# Restricted, default-off MT6797 pre-submit PTE capture

This is host-reviewed diagnostic source, not a GPU fix or hardware approval.
Enable only in an explicitly reviewed debug boot with
`panfrost.mt6797_first_submit_pte=1` (immutable root-readable parameter, mode0400).
It is otherwise false. Only MT6797's vendor-init identity with ARM_MALI_LPAE
and an available `pgtable_walk` callback is eligible. Other formats/platforms
and all existing default diagnostic records remain unchanged.

WARNING: the new records deliberately expose raw physical page-table addresses
and user GPU VAs. Keep kernel logs and saved artifacts restricted to the owner;
the parameter's mode does not itself restrict downstream dmesg/pstore copies.
Do not enable this on an untrusted multiuser system. Existing redacted records
in panfrost_gpu.c remain redacted.

## What is captured

One atomic claim per device lifetime, including skipped attempts. No runtime
rearm, retry or extra GPU job. After successful AS acquisition but before
job_lock or any submission-register writes, capture copies the software walk
for the current JC and the first page of at most eight referenced BO mappings.
Each record contains four initially-zero PTE values and the walk return code.
The header records total/sampled/omitted BO counts; BO indices are the submitted
mapping array order. `offset` is offset within the page, not a command payload
offset. No hardcoded JC, target address, physical address or job-data decoding.

The host must correlate WRITE_VALUE targets using its saved referenced-BO dump.
A target in an omitted BO or beyond a sampled first page has no captured target
walk: label it missing, never infer it from a nearby BO or successful JC walk.
An unmapped/invalid walk retains its actual error and zero/unvisited entries.
This is a pre-submit snapshot, not proof GO occurred: reset can race after it.
Only software table bytes are read; it does not prove GPU cache/TLB contents.

## Lifetime and mutation exclusion

`panfrost_lookup_bos` obtains mapping krefs and increments gpu_usecount before
queueing. Each mapping's constructor takes `panfrost_mmu_ctx_get`; its final
release tears down mapping and drops the MMU ref. Job cleanup drops these refs
only after scheduling finishes. Optional capture requires its first referenced
mapping to exist and belong to job->mmu; empty/missing/mismatched first mapping
consumes the attempt with `skipped=no-pinned-mapping`. The job's mmu pointer is
not itself an explicit kref; the proof uses the mapping-held MMU reference.
gpu_usecount prevents shrinker teardown of these BOs while the job is alive.

The ordinary mm_lock protects the VA allocator, NOT page-table mutation.
The new opt-in per-MMU spin gate therefore protects an active-mutator count:

- mmu_map_sg announces before all map_pages calls, decrements on success and
  after its error-unwind mmu_unmap_range. That helper has no other caller.
- panfrost_mmu_unmap announces before all iova_to_phys/unmap_pages operations
  and decrements before the final hardware TLB flush.
- Both ordinary initial mapping and fault-driven heap mapping call mmu_map_sg.
- Unpublished allocation-failure tables have no concurrent job; final table
  destruction cannot run while the submitted mapping's MMU ref exists.

Writers hold the gate ONLY while incrementing/decrementing. No lock is held
across GFP_KERNEL allocation, recursive reclaim, TLB callbacks, as_lock, or
hardware operations. Nested reclaim/unmap can announce another writer without
deadlocking. No GFP/noreclaim change is made. Existing map writers can still
run concurrently, exactly as the underlying io-pgtable implementation permits.

Capture uses spin_trylock_irqsave, refuses immediately if the gate is busy or
any writer is active, and otherwise holds it only through at most nine bounded,
non-allocating/non-sleeping io-pgtable walks and scalar copies. A new writer
waits only for this bounded copy, never for allocation or printk. The gate is
released before logs. No job_lock/as_lock/BO reservation lock is held by capture;
there is no gate-to-as_lock edge. TLB callbacks during mutations see no held
gate. No arbitrary PA dereference, usercopy, MMIO read/write, cache operation,
allocation or runtime power operation is added by the capture helper.

Default-off contexts never acquire the new gate. Opt-in changes timing through
brief gate operations and logging; it does not change allocation policy or
default cache/shareability/configuration. Kernel lockdown/log access policy
must be evaluated by any future package owner; no packaging is included here.

## Offline checks

`test-pte-diag.py` compiles and executes the actual capture/mutator function
bodies with host lock/walk mocks. It tests default-off and immutable gate
wiring, consumed refusal, 2048 nested writers, missing mapping, invalid IOVA,
zeroed missing/error PTEs, full-width >4GiB PTE values, max9 walks and omitted
count. A separate pthread test holds the actual capture helper inside a
test-only walker rendezvous and proves a new writer enters only after release.
The rendezvous is not present in the kernel callback. Source assertions cover
both map exits, unwind, standalone unmap and placement before job_lock.

Existing first-fault, START_MMU and pristine-AS tests pass. The START_MMU host
harness gains only a no-MMIO default-off helper stub; its 512 configuration
cases and submission ordering expectations are unchanged.

ARM64 GCC12 full panfrost_mmu.o and panfrost_job.o object compilation is checked
with CONFIG_DRM_PANFROST=y, CONFIG_PROVE_LOCKING=y and CONFIG_LOCKDEP=y in
`/tmp/gemini-pte-diag-build-RaVlRDNR`. This is compilation with lockdep enabled,
not a runtime lockdep proof or a boot test.
