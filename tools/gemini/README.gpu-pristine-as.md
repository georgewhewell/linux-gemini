# MT6797 reset-pristine legacy AS activation

This is a source-supported behavioral differential, not a hardware-proven fix.
Base: exact T2 source `664b2de786854d6e08c1bc91c3a725547b94a5a8`.

## Failure evidence and vendor comparison

T2 boot `83b8cf8d-0d19-436f-96fb-686d473ba81a` produced the first GPU fault at
192.876 seconds, before any submit diagnostic. All slot heads/configurations
were empty. AS0 was active; recorded MEMATTR/TRANSCFG and low TRANSTAB flags
were zero. Physical root addresses were redacted, so the record does not prove
a zero physical root address. At 192.980 seconds the first MMU timeout reported
AS0, command 5 (FLUSH_MEM), before=0 (completion wait). Software issues this
flush in `panfrost_mmu_enable()` before writing the new AS configuration.

The vendor MT6797 r12p1 implementation under `drivers/gpu/arm/midgard/` uses:

- `backend/gpu/mali_kbase_pm_driver.c`, `kbase_pm_clock_on()`: after reset,
  restore occupied ASes or explicitly configure unused ASes unmapped.
- `backend/gpu/mali_kbase_jm_as.c`, `assign_and_activate_kctx_addr_space()`:
  call `kbase_mmu_update()` before permitting jobs.
- `mali_kbase_mmu_mode_lpae.c`, `mmu_update()`, and
  `backend/gpu/mali_kbase_mmu_hw_direct.c`, `kbase_mmu_hw_configure()`:
  write AS configuration then UPDATE, without a leading FLUSH_MEM.
- `mali_kbase_mmu.c`, `kbase_mmu_flush()`: flush a retained running context.

Our full-range lock request computes 0x3f. Vendor definitions permit maximum
width 63; this patch does not change range geometry or claim it invalid.
Neither the T2 failure nor this comparison establishes faulty PTE attributes.

## Scope and ownership

Only MT6797 (identified by its existing vendor-init hook), legacy Mali format,
and an AS never successfully configured since reset bypass the pre-enable
memory flush. The path first checks AS idle, installs the same configuration,
issues UPDATE and verifies completion. No MMIO attributes, clocks, voltages,
coherency policy, job commands or DT properties change.

`as_configured_mask` is owned by `as_lock` and set only after UPDATE completes.
Freeing an AS, LRU reclamation and fault-disable do not clear it; these retain
their pre-enable flush. The existing disable path still requests its flush.
Only `panfrost_mmu_reset()`, called after successful GPU reset, rearms the mask.

LOCK, FLUSH and UPDATE errors now propagate through enable to `as_get()`.
Any failed enable also sets `as_enable_failed_mask`, blocking new acquisitions
until reset. This separately records uncertain partially programmed hardware
without falsely marking it successfully configured. In particular a failed
UPDATE cannot receive another pristine bypass before reset.

Failed new/reclaimed assignments drop the new refcount/list/allocation and
return the error; an evicted zero-ref LRU owner remains detached. Failed faulty
re-enable preserves all pre-existing references/list/allocation and its fault
mask. It clears pending fault status before enabling but removes the software
fault mask and unmasks IRQs only after enable succeeds. The unchanged
job caller branches to its PM error unwind when `as_get()` returns negative,
before writing the job head or submitting it.

This does not redesign the existing void disable/teardown error handling.
Other platforms retain their normal flush/configuration command sequence;
they also benefit from the new failure propagation and UPDATE completion check.

## Validation and remaining hardware question

`HOSTCC=<host-gcc> python3 tools/gemini/test-gpu-pristine-as.py` compiles the
actual wait, command, lock, enable, disable, allocation, put and reset functions
against injected poll/MMIO and list/atomic plumbing. Tests cover pristine AS0
and AS1, successful reset, free/reuse, idle-LRU reclamation, no-idle-victim
refusal, faulty re-enable, non-MT6797/nonlegacy behavior, 13 failure boundaries,
no configuration after failed flush, sticky failure and balanced ownership.
The original 0x3f full-range lock encoding is checked explicitly.

The existing actual-code diagnostic and 64-combination attribute tests pass.
Full Panfrost arm64 GCC 12 cross-compilation passes; Checkpatch reports zero
errors/warnings for the production diff. No hardware was touched.

Next runtime question: does direct pristine AS configuration reach the existing
submit diagnostic and produce verified pixels/job IRQ progress? Preserve the
same bounded render and thermal evidence protocol, and retain errors if it
does not. T2's later truncated DABT after evidence save and petter termination
has no PC/stack and remains unattributed; it is not explained by this patch.

## T3 hardware result: setup advances, rendering still fails

Candidate `d5783d87d297d944fa680697b1c79ac9a4df26213374c9914c6d0e54072b7ac1`
was consumed exactly once. Boot `e1014109-d06b-453e-9e1e-018dd25677d7`
passed GPU preflight and reached the first submit diagnostic at uptime
312.400. The root-table match predicate passed; AS0 was idle with flags
`007`, MEMATTR `0000000000484d48`, TRANSCFG zero and no AS fault. This
demonstrates progress past T2's pre-configuration FLUSH_MEM timeout.

The first render nevertheless failed its pixel check. Immediately after
submission, GPU fault `00ff0388` recurred; JS1 was active (`status=8`) with
the submitted head retained, and MMU fault status remained zero. Expected
job configuration `00083b00` read back `00003b00`; that difference alone
does not establish a defect because the vendor also requests priority 8.
Subsequent kernel scheduler resets/timeouts belong to that same render
invocation, not another host dispatch. The second render was not invoked.
Do not promote the attribute quirk or claim functional GPU rendering.

Evidence: workspace `handoff/gpu-thermal-t3-gcstVt3C/first-draw-live.txt`
and full RAM archive SHA256
`77a6e204411d996add17de47016045f12bd82de9aa332f3c40e3c65dade9208e`.
Root independently checked two saved thermal inventories: four readable
channels, same boot, PASS. Calibration/protection remain unverified.
The hardware owner restored the exact rescue image; final health/readback
closure is recorded separately in the handoff. No retry of this candidate.
