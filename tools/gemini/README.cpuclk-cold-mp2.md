# MT6797 cold MP2 clock observation

The CCF decoder accepts selector0 as identity, consistent with ixoo patch0533
(commit5b25b07189ec8ca883603c0f41c5ae33002e0aeb). All other unknown selectors
remain rejected. This changes observation arithmetic, not clock programming.

Gemini M6 boot9951c3e0-846e-4ef4-8616-74840c3b8cb6 captured mux54/div0,
MP0PLLc1114000, MP1PLL400c4000, CCIPLLc10c1d89 and both MP2PLL words zero
with CPU0-7 online. For the exact MP2 tuple (sourceclk26m, selector0,
CON0=CON1=0), decoding preserves armpll_rate0 while the CCF path uses its real
26MHz parent. No fallback PLL rate, additional sample or register write occurs.
A zero PLL selected by the mux, or a partially initialized zero-rate PLL,
continues to fail. Existing rate-plan validation still requires a nonzero PLL;
observing this cold tuple never authorizes a rate mutation.

Upstream firmware analysis and vendor runtime assign initial PLL/mux setup to
secure PSCI CPU_ON, after Linux rail/reset/isolation/SRAM preparation:
https://github.com/ixoo/gemini-pda-mainline/blob/7e17d42be831eacc1393f157be1afb4867c22c89/experiments/2026-07-22-a72-firmware-power-contract/results/a72-firmware-power-contract-prerequisites-20260722.md
https://github.com/ixoo/gemini-pda-mainline/blob/7e17d42be831eacc1393f157be1afb4867c22c89/experiments/2026-08-02-gemian-a72-first-cycle-latch/results/runtime-first-natural-pair-20260802.txt
These are upstream firmware/vendor observations, not proof of CPU_ON on our
integrated image. Our hardware evidence here covers only the offline sample.

The production KUnit suite includes that M6 tuple, exact A53/CCI rates,
unknown-selector rejection, zero selected PLL rejection, mutation refusal,
and the upstream running MP2 tuple CON0=00ff1101/CON1=b9b13b14/mux55/div8
which decodes750MHz. tools/gemini/test-mt6797-cpuclk-core-host.py runs every
production case body with native primitive/assertion adapters; it is not a
KUnit kernel execution or hardware qualification.

This decoder repair is not a complete A72 startup fix. The resource helper's
pre-CPU_ON running-PLL requirement and regulator-name assumptions still need
an explicit offline-state contract and source/provider checks. CPU8/9 startup
and post-start clock validation remain to be qualified under recovery.


The A72 owner now calls mt6797_cpuclk_check_mp2_boot at probe and again in
CPU8 preparation before starting its rail/reset transaction. The API resolves
the consumer's named mp2 clock phandle, verifies the bound supplier is this
clock driver, checks CLK_MCU_MP2 and exact CCF hardware identity, holds the
supplier device lock against removal, then takes a fresh snapshot through the
existing CSPM owner. Device/node references are released on every return.
This snapshot uses the existing CSPM access transaction (including its normal
interface-clock/semaphore operations); it does not program PLL/mux/divider.

The pure contract requires actual reference26MHz, selectedclk26m, divider0,
and raw MP2CON0=CON1=0. A programmed PLL is refused even when unselected.
Other clusters' mux/divider bits do not determine MP2 admission. CPU9 retains
its existing CPU8-online admission and does not repeat the cold check.
The resource transcript reports mp2-clock-contract=cold-clk26m-v1 only after
successful probe validation. This is a point-in-time check; secure firmware's
subsequent CPU_ON behavior still needs hardware qualification. No post-start
rate success is inferred from a pre-start check.

The core host runner now executes12 cases125 assertions. The boot API host
runner executes the actual function with device/CCF/snapshot boundary doubles
across13 success/failure paths, checking reference cleanup and no sampling on
identity failures. This is not a proof of real CCF locking or firmware behavior.
