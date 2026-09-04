# MT6797 Gemini VGPU enable-bit preflight

This G2 diagnostic is valid only after the G0 MFG-provider preflight passes.
Compose `mt6797-gemini-pda-mfg-provider-preflight.dtbo` followed by
`mt6797-gemini-pda-vgpu-toggle-preflight.dtbo`, and build
`CONFIG_REGULATOR_USERSPACE_CONSUMER=y`. The GPU node remains disabled.

Run `capture-vgpu-toggle-preflight.sh` from a storage-inert recovery initramfs
with `regulator_ignore_unused`. The script requires the firmware state found by
G1 (`VSEL0=0x27`: disabled, selector 39, 843750 uV), takes exclusive ownership
through the standard `regulator-output` driver, and changes only the enable
bit. The overlay removes the base provider's ramp-rate constraint so provider
probe cannot program that separate register. Its successful transaction is
`0x27 -> 0xa7 -> 0x27`; it rejects voltage,
user-count, clock, power-domain, GPU, or Panfrost changes. Each raw value is a
read-only register-0x11 sample from the RT5735 regmap selected by exact I2C
device ancestry and driver identity; it is not inferred from regulator state.
Clock preparation is allowed to have an inherited nonzero baseline, but the
dedicated GPU-I2C clock must be disabled and both it and shared AP-DMA must
return to their complete pre-transaction enable/prepare counts.
The helper gives asynchronous provider/consumer probing a bounded five-second
readiness window before rejecting; that window precedes the state transaction.

The cleanup trap is armed before enable. The recovery init/supervisor, rather
than this helper, must impose a bound, for example
`timeout -k 2 15 capture-vgpu-toggle-preflight.sh`; its independent hardware
watchdog and observe-always-rollback policy must already be armed. TERM/HUP/INT
and ordinary errors run the disable path. A stuck kernel I2C transaction or
SIGKILL cannot be unwound by a shell trap. Absence of the final PASS marker is
a hard stop; never advance directly to a GPU-enabled image.

## Qualification

The exact source commit
`2bfbf91f1cfc74003c23564c4e5741bb487225eb`, packaged as p22 image
`75e8b4c5907bda9de3b5f8c6caf7cc82fd635c503677d7c52e9e8fae22217f5b`,
passed one guarded hardware transaction on 2026-09-04. It observed stable
clock baselines `infra_i2c_gpupm:0:1` and `infra_ap_dma:1:4`, performed exactly
`0x27 -> 0xa7 -> 0x27`, and returned the rail to disabled at 843750 uV with
zero consumers. The watchdog reset and exact Stage-A rollback then completed.
This candidate is closed: do not repeat it as another qualification run.

That result qualifies only the isolated enable-bit transaction. It does not
qualify GPU power domains, clocks, IRQs, Panfrost binding, workloads, voltage
changes, thermal behavior, or suspend/resume.
