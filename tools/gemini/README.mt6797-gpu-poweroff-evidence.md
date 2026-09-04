# MT6797 GPU shutdown evidence after G3 R4

R4 bound Panfrost but timed out in SHADER, TILER and L2 PWRTRANS polls.
This is not evidence of failed internal power-on. The candidate is consumed;
see the hardware gate queue for the exact capture and recovery identities.

## Verified vendor behavior

Local vendor source root: `/mnt/Home/src/gemini-vendor-kernel-3.18`.
Paths below are relative to
`drivers/misc/mediatek/gpu/gpu_mali/mali_midgard/mali-r12p1/drivers/gpu/arm/midgard`.

- The captured vendor boot actually identifies its driver as
  `Midgard r12p1-01alp0`, at line 894 of
  `/mnt/Home/src/gemini-gpu-vendor-oracle-20260902/03-vendor-dmesg-full.txt`.
  Thus r12p1 is relevant; the r7p0 transition code is different.
- `Kbuild-mtk-custom-env:64` enables `MTK_MT6797_DEBUG` whenever
  `CONFIG_MTK_PLATFORM` is `"mt6797"`; this is not restricted to engineering
  builds despite the macro name.
- `backend/gpu/mali_kbase_pm_driver.c:488` explicitly skips SHADER PWROFF
  under that macro, with the comment that 6797 does not need shader
  power-down. The other core types retain their generic transition path.
- The same file's `kbase_pm_check_transitions_nolock()` derives desired L2
  state from the current shader READY mask, not just desired shaders, and
  keeps the tiler on while L2 is on. Therefore retaining shaders also
  retains their cache/tiler dependencies; copying only a shader-write skip
  into Panfrost's unconditional three-step shutdown would not reproduce
  the vendor sequence.
- `platform/mt6797/mtk_config_platform.c:350` performs whole-MFG idle polling
  before platform shutdown: select 3 at MFG offset 0x180, poll bit 2 of
  offset 0x184 for up to 100000 one-microsecond iterations. It then asserts
  bus protection and disables main/core/MFG/async/52M clocks and VGPU.
  Offsets are defined in `platform/mt6797/mtk_kbase_spm.h:71`.
- `platform/mt6797/Kbuild` leaves GPU APM disabled. Power-on writes all ones
  to PMU offsets 0x3e0..0x3f0, as the current Panfrost port does; those
  writes alone are not evidence of an accidental deviation from vendor.

## Mainline ordering and implications

Panfrost enables runtime PM, then calls set_autosuspend_delay(50), then
use_autosuspend(), then registers DRM. `update_autosuspend()` calls
`rpm_idle(RPM_AUTO)` even when use_autosuspend is still false; the delay
is not effective until use_autosuspend is enabled. Consequently an early
suspend before DRM registration is explainable without a probe failure.
This is a source-derived explanation, not a captured callback trace.

The runtime-suspend callback masks/synchronizes IRQs and invokes the void
power-off helper. MT6797 does not have GPU_PM_RT, so supplier genpd shutdown
follows the consumer callback; generic clocks-off-before-PWROFF is not
supported by this ordering. Errors in the void helper cannot veto suspend.

Next implementation must evaluate an MT6797 whole-GPU idle shutdown path,
preserving core dependencies rather than merely suppressing timeout logs.
Any idle failure must prevent successful runtime suspend and leave IRQ and
devfreq state recoverable. Removal/error unwind paths need separate review;
changing the helper return type alone does not make those paths safe.
No-job hardware verification must prove domain/protection shutdown and
resume behavior before jobs are admitted. The vendor sequence is a strong
lead, not proof that the proposed mainline behavior works.

Integration commit bfaa41e105fba already adds READY/PWRTRANS/PRESENT timeout
telemetry; R4 omitted it. Use it in any diagnostic candidate. R4's embedded
config also proves CONFIG_DEVMEM is disabled, whereas CONFIG_SPARSE_IRQ and
CONFIG_DEBUG_FS are enabled. The collector's MMIO observation interface
must be made available deliberately, without treating absent reads as PASS.

## Implementation status

Source `9822218ed0e06a58f852645d5c93e705014bb810` implements an MT6797-only
whole-GPU shutdown-readiness callback. It saves the debug selector, performs
the bounded vendor idle poll, and restores the selector on success and
failure. Runtime suspend calls it before devfreq/IRQ changes and returns
any failure to runtime PM, blocking supplier shutdown. After success the
individual shader/tiler/L2 PWROFF sequence is skipped; IRQ synchronization
and genpd shutdown remain in place. Other compatibles retain their path.

Removal and failed-resume cleanup retain the existing best-effort teardown
convention: idle errors are logged but cannot veto device removal. They do
not claim a successful runtime suspend. The normal teardown still asserts
the platform reset before detaching domains. This does not prove recovery
from a wedged active workload; no-job admission remains the next boundary.

Nine host fault-injected cases compile the actual idle/runtime-suspend
function bodies: success, busy jobs, selector read failure, selection write
failure, idle timeout, poll read error, selector restoration failure,
devfreq suspension failure, and the generic-compatible path. They prove
control flow, not actual MMIO timing or electrical power behavior. All pass.
The complete Panfrost composite object cross-compiles with AArch64 GCC 12.2;
kernel patch style check reports zero errors/warnings. Initial local build
setup needed the full GNU Make and an olddefconfig refresh; the subsequent
configured build completed successfully. No boot image has been built or
flashed for this source, and hardware validation remains outstanding.
