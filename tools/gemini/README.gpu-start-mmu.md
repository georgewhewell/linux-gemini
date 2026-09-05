# MT6797 legacy pre-job MMU synchronization differential

Exact source parent is T3 `3efe6a44cc60d3fa4afff49ab40aacd3c1efbaa6`.
The only runtime change is to include `JS_CONFIG_START_MMU` (bit10) on every
MT6797 submission whose actual page-table format is `ARM_MALI_LPAE`. Existing
HW_ISSUE_10649 behavior remains unchanged on every other platform/format.
T3's reset-pristine AS setup, attributes, thermal, DT and recovery policy remain
unchanged. This is a source-supported differential, not a proven GPU fix.

## Evidence and uncertainty

Saved T3 run `gpu-thermal-t3-gcstVt3C` reaches actual submit at312.400–312.407s.
AS0 has matching expected table flags and attributes, idle status and no AS
fault. The first global GPU_SHAREABILITY_FAULT follows at312.408685s while
JS1 is ACTIVE(0x08). Four subsequent scheduler timeouts occur within the one
user render, which reports pixel mismatch. There is no evidence of a working
render and no basis to interpret that error as merely a pixel-check issue.
The missing readback priority bit19 is also programmed by vendor priority8;
it is not evidence that bit10 or coherency is the cause.

Exact MT6797 vendor `mali-r12p1/drivers/gpu/arm/midgard/backend/gpu/`
`mali_kbase_jm_hw.c:93` sets START_MMU unconditionally before JS_CONFIG_NEXT
and START. The same local r7p0 and r12p0 lineage does so as well. Mainline
restricts this bit to HW_ISSUE_10649, absent from its T880 issue mask.

An Arm engineer's [initial Panfrost review](https://patches.linaro.org/cover/161518/)
explains that START_MMU flushes caches and invalidates TLBs before a job, reports
it working on T760, and discusses unconditional use versus mapping-time TLB
invalidation. That supports its meaning and later-Midgard applicability, but
does not establish the cause of this MT6797 global fault.

Current Panfrost maps synchronize CPU-written PTEs with the DMA API, but its
GPU FLUSH_PT operation is skipped when the context has no assigned AS or the
GPU is suspended. T3's first AS activation performs UPDATE without a later
MMU flush. Thus the vendor bit adds a real pre-job synchronization boundary;
it is not simply duplicating a guaranteed first-map flush. Nevertheless, a
properly reset GPU ought not to retain old translations, and no stale PTE or
cache contents were measured. Do not label the fault a proven cache defect,
force snooping or change memory attributes on this evidence.

## Verification

`test-gpu-start-mmu.py` compiles the actual job pre-submit/configuration prefix
with PM/AS/MMIO adapters and production register definitions. It covers512
platform/actual-format/issue/feature/chain/AS combinations, each evaluated three
times to ensure no one-shot latch suppresses the bit on subsequent calls.
These repeated calls represent configuration rebuild on first/reused/reset
submissions, not a hardware reset simulation. Power failure, occupied NEXT and
three AS errors write no job registers. Source checks retain config-before-
START ordering. The existing actual pristine-AS reset/allocation and bounded
first-fault tests remain applicable and are rerun. Compile checks cover the
complete Panfrost object directory with ARM64 GCC12.

No hardware action, candidate activation or attribute change is part of this
source commit. The sole hardware owner and root still control any next boot.
