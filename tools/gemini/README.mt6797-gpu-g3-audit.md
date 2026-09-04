# MT6797 Gemini G3 no-job gate audit

Date: 2026-09-04

This note is a read-only reconciliation of the historical GPU experiments with
the exact G2 VGPU enable-bit qualification.  It does not qualify a new image
and is not authorization to retry an old one.

## Decision

The GPU lane may advance by one gate, but neither existing G3 source branch and
none of the historical p22 images is eligible for execution.  The smallest
new gate is a Panfrost **no-job** probe at one low operating point, initiated
from the isolated initramfs only after the watchdog is proved armed.

The new source must be a direct child of the hardware-qualified G2 source
`2bfbf91f1cfc74003c23564c4e5741bb487225eb`.  It may transplant the hardened
acceptance semantics from `44a5acc98d30c564fec805df6f661dc52daf394e` (the
same overlay and helper content also appears at `257891b8b9162d844a3c4fbabeaffc3cabce908d`),
but it must not package either old branch tip.

Panfrost must be a module.  A built-in Panfrost driver begins registration at
about 0.93 seconds in the historical captures, while the MT6797 watchdog did
not register until about 0.99--1.10 seconds.  Merely composing the old G3
overlay with `CONFIG_DRM_PANFROST=y` would therefore put the new active probe
outside the recovery boundary established by G2.

## Exact G2 boundary

Source `2bfbf91f1cfc74003c23564c4e5741bb487225eb`, packaged as p22
`75e8b4c5907bda9de3b5f8c6caf7cc82fd635c503677d7c52e9e8fae22217f5b`,
passed once.  It observed:

```
before=disabled:843750:0 vsel0=0x27
clock_before=infra_i2c_gpupm:0:1 infra_ap_dma:1:4
active=enabled:843750:1 vsel0=0xa7
after=disabled:843750:0 vsel0=0x27
clock_after=infra_i2c_gpupm:0:1 infra_ap_dma:1:4
VGPU_TOGGLE_PREFLIGHT=PASS before=0x27 active=0xa7 after=0x27
```

The nowayout watchdog then reset the device and the supervisor restored exact
Stage-A p22
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`.
The evidence is in:

- `/mnt/Home/src/nixos-gemini/repo-g2-p22/research/mt6797-vgpu-g2-evidence/2026-09-04-pstore-excerpt.log`
- `/mnt/Home/src/nixos-gemini/repo-g2-p22/research/HARDWARE_TEST_LEDGER.md`

This proves only the RT5735 enable-bit transaction at the inherited selector.
It does not prove a selector change, MFG power domains, GPU clocks or reset,
Panfrost, IRQ registration, a GPU job, DVFS, thermal operation, or suspend.

The two earlier G2 candidates rejected before enabling VGPU and are also
closed:

- `9a667439e951297d931583a6629486b2c3fc292cdc885b249fdd07fb5f7fefbb`
  rejected an incorrect shared AP-DMA baseline assumption.
- `002d312d2be67054f6a399b1ec3a715182c909819f05b22f6dd1f9a41e364f0f`
  rejected an incorrect prepared GPU-I2C baseline assumption.

There is no reason to repeat any of these three G2 images.

## Historical GPU evidence

### Staged diagnostics

The first staged image,
`33f1d5c570fb4bc78cfb256c8498110d945b075452f1ef6f050bdf18ebf5335c`,
proved RT5735 registration and the initial MFG-domain stage, but stage 2 failed
with `-ENOENT` because BG3D clock ID 1 was absent.  Cleanup released the VGPU
and MFG votes.  See:

- `/mnt/Home/src/nixos-gemini-cycle-7fbf3aa-artifacts/gpu-staged-run-231014/candidate-MANIFEST`
- `/mnt/Home/src/nixos-gemini-cycle-7fbf3aa-artifacts/gpu-staged-run-231014/gpu-live-dmesg.log`

The transaction's `lease_proof_passed_recovered_stage_a` state is recovery
harness proof, not GPU stage-2 proof.  Its manifest accepted the generic
`unattended_recovery=healthy` marker, and the kernel log independently records
`stage 2 failed`.  This image is closed and must not be described as an ID
read success.

The sparse-provider repair image,
`303d37064beb76594c48e1a96fb1db33272fd15a9a7af0e6226b049e887f8b14`,
subsequently completed the bounded diagnostic:

```
stage 0 pass: RT5735 registered, enabled=0 voltage=700000uV
stage 1 pass: MFG status=...
stage 2 pass: id=08801008 l2=07130206 ... shader=0000000f (read-only)
stage cleanup pass: VGPU and MFG votes released
```

See:

- `/mnt/Home/src/nixos-gemini-gpu-bg3d-fix/research/p22-gpu-bg3d-fix.manifest`
- `/mnt/Home/src/nixos-gemini-cycle-7fbf3aa-artifacts/gpu-bg3d-persistent/live-final-20260830-235000.log`

The stale dry-run trace contains a different mistyped expected hash and is not
proof.  The manifest, actual image digest, and live p22 capture agree on the
hash above.  A new custom staged-ID driver would duplicate this completed
experiment and is not the next gate.

### Panfrost binds with failed idle power transitions

P22 `791ca56fd75a2a2aab64626b30273be7b159d4f1ca31712900c2ebcfb34a5a63`
bound the T880 at 500500000 Hz and 975000 uV and registered DRM minor 0.  It
also emitted shader, tiler, and L2 power-transition timeouts.  Binding and DRM
registration are therefore not a pass.  Evidence:

- `/mnt/Home/src/nixos-gemini-cycle-7fbf3aa-artifacts/gpu-fixed-a78c-run/RESULT.txt`
- `/mnt/Home/src/nixos-gemini-cycle-7fbf3aa-artifacts/gpu-fixed-a78c-run/live-a78c-capture/remote-capture.txt`

P22 `1570f2abdc3b40f906646ede60722d7674909426b57d27b5366ba3b688d4d725`
added the 26000000 Hz bus clock.  It again identified T880 r1p0, shader mask
`0xf`, L2 mask `0x1`, and DRM minor 0, then emitted all three transition
timeouts.  It submitted no job and made no runtime-PM write.  Evidence:

- `/mnt/Home/src/gemini-gpu-busclk-hardware-20260902/README.md`
- `/mnt/Home/src/gemini-gpu-busclk-hardware-20260902/candidate-runtime/dmesg-and-runtime-readonly.txt`

Both images are rejected and closed.  Another 500.5 MHz / 975 mV bind would
repeat them.  The three internal power-transition timeouts remain the key
unresolved Panfrost failure and are mandatory rejection signals in G3.

The two paths named `gemini-gpu-poweroff-diagnostic-20260902` and
`gemini-gpu-poweroff-diagnostic-vendor-clock-20260902` are dangling links to
garbage-collected Nix store paths.  They provide no independent bytes or
evidence and must not be used to infer another result.

## September 3 hard-dark boots

The old summary in
`/mnt/Home/src/nixos-gemini/repo/research/GPU_PANFROST_BRINGUP.md` says the
clock-fixed and Panfrost-blacklisted boots left the board with no USB and
attributes the boundary to a newly resolvable BG3D clock, with unused genpd
sweeps still open.  The source audit identifies a more direct cause.

Commit `b3156355a31ce04fc0a4749b4fc374139cd9da0c` changed
`mfg_desc.num_clks` from one to two while `mfg_clks[]` still held only one
descriptor.  The generic MediaTek probe uses `num_clks` both to allocate the
onecell array and as the bound passed to `mtk_clk_register_gates()`.  The gate
helper then loops to `i < num` and dereferences `clks[1]` out of bounds.  This
provider probe occurs independently of Panfrost and explains why both the
ordinary clock-fixed image and the Panfrost-blacklisted image went hard dark.

The relevant source is:

- `drivers/clk/mediatek/clk-mtk.c`, where `num_clks` sizes the allocation and
  is passed to gate registration.
- `drivers/clk/mediatek/clk-gate.c`, where that count is the descriptor-loop
  bound.
- commit `c4d2022e9cb3ab626699711c4b6e0f9e431223a7`, which repairs MT6797 by
  inserting the normal dummy descriptor at binding ID 0 and retaining
  `ARRAY_SIZE(mfg_clks)` for both meanings.

The modern provider-only G0 gate passed on hardware before G2, so the repaired
provider registration path is no longer an open candidate variable.  This
closes the September 3 no-USB attribution; it does not close the independent
Panfrost power-transition timeouts.

The September 3 p22 bytes are all retired:

- `e67c5964cf67a23d975b1976523be5dedfb3b309e9f39387bff8ea640af1d030`:
  fixed OPP, but BG3D still absent; Panfrost did not bind.
- `21c6631a4c75107beeba9776b8f3497b5cc1073095a111d7c29a1cdf546cc71e`:
  the out-of-bounds `b3156355` clock-provider image; hard dark.
- `b779a03a8f271ced26a6edb8a1ff3e50f7128f8dcb0cf6a64d3aeb1448460bcc`:
  the same provider defect with Panfrost blacklisted; hard dark.
- `6a82d410ce1171e06d53c705ff01b7dec9e57260c38ab52bc872eb25568cfa25`:
  an unqualified `pd_ignore_unused` artifact with no trustworthy paired run;
  it is non-isolated and inherits the bad provider lineage.

Their manifests are
`/mnt/Home/src/nixos-gemini/repo/research/p22-gpu-{fixed-opp,clkfix,deferred,pdignore}-20260903.manifest`.
They must not be retried.  The files named `p22-gpu-busprot-20260903.manifest`
and `p22-gpu-wdt-20260903.manifest` describe baseline/recovery packaging, not
additional GPU functional evidence.

## Why the old G3 branches cannot be packaged

`codex/mt6797-gpu-g3-preflight-bd42` ends at
`44a5acc98d30c564fec805df6f661dc52daf394e`; its merge base with G2 is
`da6de4d275e84a18e74e3f46959e1bdf667bad1e`.  It only hardened the helper and
overlay comments and never produced a candidate image.

`codex/mt6797-g3-source-gate-edfb` ends at
`7e3cd3610dba6a862306c219394bd5d03b2fdb90`; it is based on the obsolete G1
tip `edfb643dcbfb9108277219a00441abdc35ae698a`, and its merge base with G2 is
`361be1d7d6828c46dc56a6825c8a768d2759866b`.  Its own commit message calls it
source-only.  It also never produced a candidate hash.

Both overlays expose the complete board OPP table, and their ordinary built-in
Panfrost packaging would start the active probe before watchdog registration.
The helper semantics are useful; the branch ancestry and activation mechanism
are not.

## Exact next package contract

The package is eligible for offline construction only when all of the
following are enforced.

1. Source is a new direct child of
   `2bfbf91f1cfc74003c23564c4e5741bb487225eb`.  The manifest records both
   identities, and its new p22 hash differs from every retired hash above.
2. Packaging inherits the G2 entry-isolation profile: exact accepted Stage-A
   DT base and rollback, CPU8/CPU9 closed, no Stage2 or storage access, no
   radios, CPU frequency, CPU idle, or thermal activation, standard ramoops,
   `regulator_ignore_unused`, and no `pd_ignore_unused`.
3. The opt-in DT changes only I2C7, RT5735 with
   `regulator-ramp-delay = <32000>`, MFGSYS, and `gpu@13040000`.  The GPU points
   to a new one-entry OPP table containing exactly 238000000 Hz with
   `opp-microvolt = <846700 846700 850000>`.  It does not add
   `regulator-boot-on`, does not clamp regulator min/max, and does not retain
   the G2 `regulator-output` consumer.
4. `CONFIG_DRM_PANFROST=m`, not built-in.  The module and dependencies are in
   the dedicated initramfs.  No process opens a DRM primary or render node,
   and there is no job, fence, runtime-PM write, unbind, rebind, or unload.
5. The external supervisor is armed before candidate boot, and policy is
   observe-always-rollback with no self-accept.  Initramfs first proves
   `mtk-wdt`, running state, timeout 31, and nowayout 1, runs the inherited
   read-only provider prerequisites, emits a durable `g3_activation=begin`
   marker, and only then invokes one timeout-bounded `modprobe panfrost`.
   A stuck probe is left to watchdog reset and supervisor rollback.
6. The capture helper retains the fail-closed checks from `44a5acc9`, adapted
   to the fixed-low profile.  It observes a two-second stability window and
   emits `GPU_PREFLIGHT=PASS` only if every condition below holds.

The exact G3 acceptance conditions are:

- Exactly one `mali-t880 id 0x880 major 0x1 minor 0x0 status 0x8` line and
  nonzero topology, specifically shader mask `0xf` and L2 mask `0x1`.
- Dedicated `panfrost-job`, `panfrost-mmu`, and `panfrost-gpu` IRQs are Linux
  IRQs 296, 295, and 294.  All counts are zero and remain unchanged.
- No DRM node is open before or after the stability window.
- Runtime PM is `suspended`, control is `auto`, and autosuspend delay is 50 ms
  before and after observation.
- `mfg_async`, `mfg`, and `mfg_core0` through `mfg_core3` are all off.
- SPM MFG bits 13:8 are clear in both status words; TOPAXI protection bits 21
  and 23 are asserted in both enable and status; TOPRGU MFG reset bit 2 is
  clear.
- `mfg_bg3d`, `infra_mfg_vcg`, `mfg_52m_sel`, and `mfg_sel` each have exactly
  one enable and prepare vote and remain byte-for-byte stable.
- Devfreq available, current, minimum, and maximum frequencies are all exactly
  238000000 Hz and remain unchanged.
- VGPU is exactly `enabled:850000:1` and remains unchanged.
- The complete kernel log contains no fatal init, reset, regulator, OPP,
  devfreq, domain, bus-protection, GPU fault, page fault, or shader/tiler/L2
  power-transition timeout.

After the terminal PASS marker, the candidate still must reset and the
independent supervisor must restore and read back Stage-A p22
`1b5f808e494f8c91ec5feedcbce89bf018271b30b87ce3910082cd298d8817dc`.
Absence of any condition or of the final rollback proof is rejection.

A pass qualifies only the current production clock/reset/domain path, the
one-selector-step rail transition, Panfrost bind and identity, IRQ
registration, and no-job idle autosuspend at 238 MHz.  It does not qualify a
render job, any other OPP, DVFS, thermal behavior, system suspend/resume, or a
persistent GPU-enabled system.
