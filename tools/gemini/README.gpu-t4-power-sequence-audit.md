# T4 GPU power-sequence audit (offline)

Source under test: e5e16d9750b6a650e55594509778dc6e43645833.
Vendor source: gemini-vendor-kernel-3.18,
8cfe6596a503612e3332d9c26e292a19525a7f07, clean worktree.
No device access, runtime changes, or successor package is part of this audit.

## Outcome

The core-domain reset/isolation/clock order differs from the vendor sequence.
The vendor source documents its steps, but supplies no explanation that the
alternative order is forbidden or produces a shareability fault. No mandatory
ordering contract establishing that causal link was found in these sources.
Do not promote a vendor-parity proposal into a demonstrated rendering fix.

The apparent AXI-release-before-clock issue applies to initial probe, not
necessarily the actual first-draw resume. Tracing the Linux runtime-PM call
chain and the saved T4 clock inventory substantially weakens that hypothesis.

## Actual Linux execution

`panfrost_device_init()` attaches four named domain suppliers before reset and
clock initialization. `genpd_dev_pm_attach_by_id()` attaches the virtual device
with `power_on=false`; it is the subsequent
`device_link_add(DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE | DL_FLAG_STATELESS)`
which calls `pm_runtime_get_sync(supplier)`. `genpd_power_on()` recursively powers
parents first: MFG_ASYNC, MFG, then the requested core. Each core callback
serializes shared AXI protection; release only occurs when both power-status
registers show the whole four-core group on. Thus initial probe can release
protection before `panfrost_clk_init()` enables BG3D and the bus gate.

That is not the draw-time sequence. MT6797's actual compatible data enables
GPU_PM_CLK_DIS and GPU_PM_VREG_OFF, but **not GPU_PM_RT**. Those first two flags
govern system suspend, not runtime suspend. The runtime callback neither
disables the main/bus clocks nor asserts TOPRGU reset. On subsequent resume,
`rpm_callback()` calls `rpm_get_suppliers()` before the consumer callback;
the already-enabled gates therefore precede domain reactivation and its AXI
release. `panfrost_device_reset()` subsequently does vendor setup, GPU soft
reset, GPU power-on and MMU bookkeeping reset before submission.

Saved T4 evidence independently agrees: at approximately 53 seconds all six
MFG domains are off, but mfg_bg3d and infra_mfg_vcg each have enable_count=1.
The draw is at approximately 166 seconds. Evidence directory:
`handoff/gpu-thermal-t4-a9dRJSnj/ram-evidence/run/gpu-oneboot`.
Relevant entries are in `dmesg-final`, lines 1428-1446 at capture time.
This is not proof of every register transition, but does rule out treating the
probe call order alone as the first-draw clock order.

## Vendor comparison

The active CCF implementation is `drivers/clk/mediatek/clk-mt6797-pg.c`, built
by CONFIG_COMMON_CLK_MT6797. The older similarly named mt_spm_mtcmos.c is not a
substitute for tracing these GPU clock-provider callbacks.

* Vendor core0-3 power-on: power bits and both ACKs; set PWR_RST_B; clear
  PWR_ISO; clear PWR_CLK_DIS; clear SRAM_PDN and wait for its ACK to clear.
* Current `scpsys_power_on()`: same power/ACK and SRAM boundaries, but clear
  PWR_CLK_DIS; clear PWR_ISO; set PWR_RST_B.
* Vendor MFG parent order is CLK_DIS, ISO, RST_B, matching current generic code.
  Any future parity change must be scoped to the four core domains, not MFG,
  MFG_ASYNC or other MediaTek platforms.
* Vendor Mali r12p1 `platform/mt6797/mtk_config_platform.c` enables the bus gate,
  parent/core power clocks and main gate, waits 100 microseconds, then clears
  AXI protection bits 21 and 23. Current code has no explicit corresponding
  100-microsecond delay at group release. Its actual draw-time gates are already
  enabled as described above; source alone gives no minimum stabilization
  interval measured from the final SRAM ACK.
* TOPRGU reset pulse, selected MFG_52M parent, asynchronous timing and PMU
  writes are already represented in current initialization. Vendor and current
  noncoherent T880 shader/tiler/JM/L2 quirk branches do not establish another
  cache/coherency flag fix. In particular L2_MMU_CONFIG=0 is consistent with
  the vendor's default AXI ID limits and non-ACE mode.

## T4 boundary and limitations

START_MMU readback is 0x3f00, versus T3's 0x3b00. A submitted job remains ACTIVE
(status 8), then global GPU_SHAREABILITY_FAULT 0x00ff0388 appears before the
scheduler timeout. AS/MMU fault status remains zero. Thus START_MMU reached
hardware but did not fix rendering. No second host draw was issued; scheduler
replays must not be described as independent host tests. Physical table roots
are deliberately redacted, not zero.

No PTE-to-physical-address or fault-origin evidence currently distinguishes
the residual global fault as core sequencing, cache contents, or bus behavior.
An exact vendor-parity core sequence is a bounded candidate for source review,
but remains a hardware hypothesis, not a fix justified by this capture. This
audit does not authorize another diagnostic-only boot or guessed register write.

## Verification

`test-gpu-mt6797-poweroff.py` now compiles the actual runtime-suspend function
using the actual compatible PM-feature expression and enum. It checks that
MT6797 performs zero clock disables and zero reset assertions, while an RT
control fixture performs two disables and one assertion. Existing fault paths
remain covered. These are host actual-code tests, not hardware simulation.
