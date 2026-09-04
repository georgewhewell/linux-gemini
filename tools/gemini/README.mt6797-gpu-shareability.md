# MT6797 legacy GPU MMU shareability experiment

Status: source-supported hypothesis, not a demonstrated hardware fix. The first
real Mesa draw on the one-boot GPU candidate reported GPU_SHAREABILITY_FAULT
(GPU fault status 0x00ff0388) and stuck AS_ACTIVE. Initial no-job preflight had
passed. No hardware operation is performed by this patch or its test.

The MT6797 compatible uses legacy ARM_MALI_LPAE tables and is not DMA coherent.
Before this change, its cached BO mappings used AS_MEMATTR byte 0x8d and PTE
SH[9:8]=2, like every other legacy Panfrost integration. This experiment gives
only the non-coherent MT6797 integration GPU-local coherence: cached mappings
use byte 0x4d and SH=3; uncached/MMIO mappings use byte 0x48 and SH=0.
Indices, permissions, execute policy, page-table walker cache maintenance,
CPU/GPU buffer synchronization, and all unselected integrations are unchanged.
The quirk is rejected for a coherent page-table walker.

## Vendor evidence

All paths below are relative to the vendor kernel's
`drivers/misc/mediatek/gpu/gpu_mali/mali_midgard/mali-r12p1/drivers/gpu/arm/midgard/`.

- `mali_midg_regmap.h`: AS_MEMATTR_LPAE_IMPL_DEF_CACHE_POLICY is 0x48 and
  AS_MEMATTR_LPAE_WRITE_ALLOC is 0x4d. Its separate OUTER_IMPL_DEF/OUTER_WA
  values are 0x88/0x8d.
- `mali_kbase_mmu_mode_lpae.c:mmu_get_as_setup`: programs those legacy
  attributes, not the AArch64 attribute constants.
- `mali_kbase_mem.c:kbase_update_region_flags`: normal default MEMATTR is the
  non-outer implementation-defined index. The outer default is selected only
  for COHERENCY_ACE plus KBASE_REG_SHARE_BOTH.
- `mali_kbase_defs.h`: SHARE_INNER_BITS is 3<<8; SHARE_BOTH_BITS is 2<<8.
- `mali_kbase_mmu_mode_lpae.c:get_mmu_flags`: selects SH=3 for local coherence,
  SH=2 for system coherence, and SH=0 with neither flag. Ordinary vendor
  userspace allocations are therefore NOT proven to use SH=3 universally.
- `mali_kbase_mem.c:kbase_jit_allocate`: its new JIT allocations explicitly
  request BASE_MEM_COHERENT_LOCAL. This is a concrete vendor use of SH=3.

Panfrost's normal and heap mappings request IOMMU_CACHE. Mapping that request
to GPU-local coherence here is intentional; it does not assert that vendor
userspace made exactly the same allocation requests. We retain write-allocation
(0x4d) rather than switching to vendor's ordinary implementation-defined
allocation policy (0x48), while removing the outer-cache attribute.

Do not force AArch64 page tables: this driver's T880 feature set does not expose
HW_FEATURE_AARCH64_MMU. Do not add dma-coherent without hardware evidence.
Mesa cannot change these kernel-owned MMU attributes through an environment
variable. This affects built-in io-pgtable code as well as Panfrost, so replacing
the Panfrost module alone is insufficient.

## Validation and remaining acceptance

`python3 tools/gemini/test-gpu-mt6797-shareability.py` compiles the actual PTE
function, attribute assignments, allocation validation and Panfrost quirk gate.
It checks all 64 combinations of mapping flags, unchanged non-shareability
bits, old and experimental MEMATTR values, rejection of coherent/unknown quirk
combinations, and the format/platform gates. It is not a GPU simulator.

The complete Panfrost object and io-pgtable-arm object cross-compile with GCC
12.2 using the previous GPU kernel configuration. Full-image integration and a
real Panfrost draw/readback followed by runtime suspend and another successful
draw are still required. Preserve exact rescue and bounded recovery ownership.
