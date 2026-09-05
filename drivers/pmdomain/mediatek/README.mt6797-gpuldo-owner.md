# Experimental Gemini GPULDO / VGPU ownership transfer

Default off. Host-reviewed source only; no hardware success, voltage calibration
or authorization follows from compiling this driver. The exact source baseline
is T7 `9728e9a5b3964b429e406ca94956b0c08680abec`.

## Why this is a supply-lifecycle change

T7's first real job observed internal GPULDO codes `09090909/09090909/09` at
infracfg offsets fc0/fc4/fc8; all reads succeeded. It still failed the first
pixel render with GPU_SHAREABILITY_FAULT. No numeric voltage is inferred.

The working Gemian Image.gz-dtb SHA256 is
`b53d191dc41d3f7364b0fa62b4bc920b1d013a1942b2e6b06727263fc56fcf4d`.
Its extracted config SHA256 is
`231d8a2ffe7afac3a4cc62c27d0eb6fe8bd9165ebd096e3e3346dd6df35c18f4`:
Midgard r12p1, common DVFS, GPU SPM DVFS explicitly disabled. Thus the optional
PCM firmware's presence in source is not evidence of its execution in this
working binary. The conventional r12p1 `platform/mt6797/mtk_config_platform.c`
at vendor source `8cfe6596a503612e3332d9c26e292a19525a7f07` supplies the copied
sequence: init708–714, power-on285, power-off389–404.

In T7 the MFG domain and Panfrost OPP code BOTH held VGPU regulator votes;
MFG_ASYNC had no regulator. Panfrost GPU/bus clocks stayed enabled over runtime
suspend. Adding LDO writes to a regulator-less async domain would not reproduce
the vendor ordering. This mode explicitly transfers both rail votes and the
consumer clock lifecycle instead.

## Activation and admission

Both scpsys and GPU require DT property `mediatek,gemini-gpuldo-owner`.
The built-in driver's immutable bool must explicitly be enabled with
`mtk_scpsys.mt6797_gpuldo_owner=1`. It defaults false. No option alone modifies
the default DT/profile. A flagged DT without the option refuses admission.

`gpuldo-owner-carrier.py INPUT OUTPUT` accepts only exact T7 DT SHA256
`7ca55149138e0ba5a682b8fca0c7ed9e3c9b49404a6b56e7f16deb2b55fa0af4` and creates
an exclusive new output. It removes GPU mali-supply and scpsys mfg-supply,
adds mfg_async-supply to the same phandle, and adds the two mode flags. The
derived DT SHA256 is
`af7e03c3e4600089cbd301e8f9c44ea59aad374ebdb159c7eccfc817dba284c2`.
No OPP, voltage interval, clock, reset, thermal or memory property changes.
This is a flattened-DT transformation, not a plugin pretending that omitted
delete-property directives remove properties from the existing carrier.

Runtime admission independently requires:

- Exact Gemini/MT6797, unique available infracfg syscon at10001000/1000,
  no syscon clocks or reset side effects.
- Physical RT5735 reg1c/namevgpu under the enabled MT6797 I2C controller with
  resources11010000/1000 and11000580/80. No always-on/boot-on/upstream-supply
  property, no second DT supply reference, and exclusive regulator acquisition.
- Unique GPU13040000/4000 and exactly the existing single238MHz OPP with
  microvolt interval846700/846700/850000.
- All six MFG status bits clear in BOTH status registers, plus external rail
  disabled. Inherited-on or ambiguous state refuses; it is never powered down
  automatically to manufacture admission.
- Successful genpd initialization, OF provider registration and all hierarchy
  edges before provider-ready publication. Any registration error retains
  non-removable objects, latches failure and does not publish usable admission.
- Panfrost checks that exact ready provider/GPU node and ordered core0–core3
  phandle/name list before domain attachment or enabling this mode.

## Sequence, locks and failure lifetime

The outermost async domain sets fd8=88888888, fe4=8, fc0/fc4=0f0f0f0f,
fc8=0f once while all GPU domains and VGPU are off. It then writes fbc=1ff,
sets the existing external846700–850000uV interval and enables VGPU before
clock/domain power-on. A normal later resume does not repeat fixed setup.

Panfrost's per-device compatible copy removes regulator ownership and its
duplicate system-sleep clock gate. Its existing runtime callbacks now balance
GPU/bus clocks for this mode; no extra reset calls are added. Clock votes are
dropped only after successful whole-GPU shutdown. Parent-domain power-off
then completes bus protection/domain ACK and domain clock disable before
VGPU disable; physical regulator-disabled readback precedes fbc=0 last.

Any policy, supply or domain-transition error latches this owner's boot as
failed. A possibly effective write is not undone. In particular the opt-in
power-on error path bypasses generic clock/regulator unwind after uncertain
domain operations, and a failed child prevents later parent power-off. A
regulator-disable failure never proceeds to fbc=0. No register restore read,
arbitrary MMIO interface, retry or timeout extension is added.

All GPU domain callbacks take the owner mutex before the existing shared bus
protection mutex; raw callbacks never invoke genpd transitions recursively.
The provider-ready query takes only the owner mutex. No path acquires owner
mutex under the bus-protection lock. Regulator operations may sleep under the
owner mutex, never a spinlock. Exclusive supply and DT guards exclude a second
Linux consumer. This does not prove an unknown firmware agent is inactive.

The existing provider is built in, has no remove callback, and suppresses bind
attributes. No initialized owner backing is released on a registration error.
Before registration, admission does not write GPULDO or enable VGPU. The
managed exclusive regulator resource uses regulator_put, not an automatic
disable action; later transition failures do not run probe devres cleanup.
Recovery is the independently supervised exact rescue boot, not an attempted
runtime restoration of unknown register values. The initial physical-state
gate may reject an inherited-on device; no observer-only boot is justified by
this source alone, and no takeover has been implemented.

## Verification

Host suites execute the actual source functions, not copied expected logic:

- `test-gpuldo-owner.c`: read-only admission, one-time setup/order, all nine
  write/supply failures including possibly effective errors, terminal latch.
- `test-gpuldo-integration.py`: actual scpsys callbacks;4096 paired status
  combinations,12 on/5 off failure sites, shared-child failures, ordinary unwind.
- `test-gpuldo-registration.py`: all six GPU-domain init failures, provider
  failure, each five hierarchy-edge failures; no publication or unsafe free.
- `test-gpuldo-provider.py`: actual provider and Panfrost init,17 ownership and
  malformed-provider/name/index/flag paths; no control path before admission.
- `test-gpuldo-pm.py`: actual runtime/system callbacks, four resume and two
  suspend failures, busy refusal, no duplicate clock votes/reset/OPP calls.
- `test-gpuldo-dt.py CARRIER DERIVATIVE`: actual C admission through a read-only
  OF/libfdt shim, valid/default/disabled cases and15 real-DT corruptions.

Use HOSTCC for a host compiler and FDTPUT for dtc's fdtput. The DT test compiles
this tree's read-only libfdt sources, avoiding a host libc ABI mismatch.
Existing PTE/concurrency, read-only GPULDO diagnostic, START_MMU, pristine-AS
and first-fault diagnostic suites also pass unchanged.

ARM64 GCC12 object build includes scpsys, Panfrost GPU/device/devfreq with
CONFIG_MTK_SCPSYS=y, CONFIG_PROVE_LOCKING=y and CONFIG_LOCKDEP=y. This is
compile-time lockdep configuration, not a claim that lockdep ran on hardware.
No kernel image or boot transaction was built or executed for this mode.
