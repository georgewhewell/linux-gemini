# Default-off first-job GPULDO observation

Exact source baseline: T6 e23f25eddde3135efc707ca10a8cfa99c22bdd83. The original implementation was host-only; the separately reviewed T7 observation is recorded below.

`panfrost.mt6797_gpuldo_diag` is immutable0400 and defaults false. Disabled mode performs no DT lookup, mapping or read. Enabled mode requires MT6797 vendor initialization, Gemini+MT6797 root compatibles, a unique available `mediatek,mt6797-infracfg` generic syscon node, and exact resource10001000/1000. Nodes declaring clocks/resets are refused. The shared `device_node_to_regmap` path deliberately uses check_res=false: unlike syscon_node_to_regmap it does not acquire clocks or deassert reset on new-map creation. There is no private/arbitrary MMIO mapping or fallback address.

First real job, after its successful existing runtime-PM acquisition and before AS allocation/enable, claims a separate per-device atomic bit. It reads only offsetsfc0/fc4/fc8 once, in that order, outside job_lock/as_lock. Job-lock/as-lock lockdep assertions and might_sleep document/check the execution context. Exactly three calls, no loop, retries, extra power transitions or writes. Failed reads are status-qualified and values zeroed; never interpret an error as measured zero. Lookup failure is recorded unavailable and does not change probe/job success or trigger fallback. The one-shot bit is not reset by GPU recovery, power cycling, read failure or further submits.

The offsets are physically10001fc0/10001fc4/10001fc8, not10000fxx. Read precedent is vendor r12p1 MT6797 `mtk_config_platform.c:508–510`. Other GPULDO controls fbc/fd8/fe4 are deliberately excluded: this audit did not establish their read semantics. No interpretation of voltage, enable state, SRAM protection or causality is built into the diagnostic. Values show current pre-AS codes only; they do not prove which boot stage set them. Three extra reads and logging can perturb timing and must be recorded as the diagnostic delta.

Host tests:

- `python3 drivers/gpu/drm/panfrost/test-gpuldo-diag.py`: actual C functions with strict DT/resource/map/read/PM mocks. Disabled, wrong platform, missing/duplicate/unavailable/non-syscon nodes, failed resource, wrong base/size, clocks/resets and lookup errors cause zero reads. All8 read-failure subsets still cause only the exact three calls; error values initialized/qualified.100 repeated calls and32 concurrent claims do not rearm. Source checks enforce immutable parameter, no writes, pre-AS placement outside job lock.
- With HOSTCC set to the GCC12 path below: existing PTE/concurrency, first-fault, pristine-AS tests PASS. Actual START_MMU/job test updated only with diagnostic/lock mocks and call assertions:512 combinations x3 first/reuse/reset still emit identical job configurations and retain all power/busy/AS error paths. Failed PM acquisition causes zero diagnostic calls.
- ARM64 object-only compile PASS, handle54667 exit0, output `/tmp/gemini-gpuldo-diag-build-WwiZ3CeT/{config.log,build.log}`. CONFIG_DRM_PANFROST=y, CONFIG_PROVE_LOCKING=y, CONFIG_LOCKDEP=y. Objects panfrost_gpu.o and panfrost_job.o. This is compile-time lockdep coverage, not runtime validation.
- HOSTCC `/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc`; cross compiler `/nix/store/2j8rr9fqjqpj4fwb062l1pkbvy51lsf9-aarch64-unknown-linux-gnu-stage-final-gcc-wrapper-12.2.0/bin/aarch64-unknown-linux-gnu-`; make4.4.1. Fresh outdir copied prior lockdep configuration then olddefconfig. No reused source-dependent generated files.

## T7 observation, 2026-09-05

Image `1f8329ae7c5763ea72307788a9232950227f292b8293a6929e50153670f08d12`
uses source `9728e9a5b3964b429e406ca94956b0c08680abec`, the exact T6 DT,
and both GPULDO and PTE diagnostics enabled. Saved boot identity is
`c9d2b2e4-8387-4481-9f29-2e7c9c9f85fd`. At uptime129.964769 the three
pre-AS reads succeeded: fc0=09090909, fc4=09090909, fc8=00000009.
The first saved GPU_SHAREABILITY_FAULT followed at130.018871, with address
000000ffdffffb80. The single test failed its first pixel comparison;
rendering remains unproven. No repeated host test was dispatched.

The captured values differ from vendor r12p1 initialization's
0f0f0f0f/0f0f0f0f/0000000f. This is a code difference, not a measured voltage
or causal proof. The vendor GPU DVFS PCM binary in `mtk_kbase_spm_fw.c`
also embeds these three addresses, and its startup is conditional on
MTK_GPU_SPM. Initialization values therefore do not establish stable
draw-time state or sole ownership. Firmware behavior, register encoding and
safe sequencing must be established before proposing control writes.

Follow-up configuration check narrows that firmware lead: root freshly
extracted IKCONFIG from the known-good Gemian Image blob SHA256
`b53d191dc41d3f7364b0fa62b4bc920b1d013a1942b2e6b06727263fc56fcf4d`.
The extracted configuration SHA256
`231d8a2ffe7afac3a4cc62c27d0eb6fe8bd9165ebd096e3e3346dd6df35c18f4`
matches `repo/research/vendor-kernel.config` exactly. It explicitly disables
CONFIG_MTK_GPU_SPM_DVFS_SUPPORT and enables COMMON_DVFS_SUPPORT. The optional
PCM is therefore not enabled by this actual working-image configuration;
its embedded addresses alone are not evidence of a competing owner in that
profile. Focus further comparison on the conventional vendor setup/power
path, while retaining the unresolved register-encoding/sequencing questions.

Integration constraint: root decompiled the actual T7 DT and found
`mali-supply` but no `mfg_async-supply`. Current scpsys regulator enable/disable
are no-ops when that domain has no supply. Panfrost's device initialization
attaches domains, enables clocks, then calls devfreq initialization, whose
`dev_pm_opp_set_opp()` enables/configures VGPU. Consequently a GPULDO hook
merely surrounding `scpsys_regulator_enable/disable()` does not by itself
establish the vendor's external-supply ordering or prove VGPU is off before
clearing the LDO control. A proposed fix must resolve actual supply ownership
and both initialization/runtime lifecycles; no DT ownership change or GPU
control-write candidate is approved by this observation.

Owner-restricted evidence is in `handoff/gpu-thermal-t7-sXr5Olyw` under the
workspace, with full RAM archive SHA256
`a52a868bd944e20f28edd792ab6267348a9586c07612b083e771aa1bcf200684`.
Recovery closed at06:56:20: exact accepted rescue boot
`3dfe1aaf-025a-4bba-b267-c48ba903ecec`, USB and RJ45 healthy, all three
partition hashes unchanged, no failed units, and transaction processes/locks
released. Attributed pstore SHA256
`2de8e09298c22ae8fdc48d477e15d31df2bccdcbabf1446e8ae577fec8868af2`
matches the harness archive. The image is consumed and must not be retried.
No public push.
