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
