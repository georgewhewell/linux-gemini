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
The helper gives asynchronous provider/consumer probing a bounded five-second
readiness window before rejecting; that window precedes the state transaction.

The cleanup trap is armed before enable. The recovery init/supervisor, rather
than this helper, must impose a bound, for example
`timeout -k 2 15 capture-vgpu-toggle-preflight.sh`; its independent hardware
watchdog and observe-always-rollback policy must already be armed. TERM/HUP/INT
and ordinary errors run the disable path. A stuck kernel I2C transaction or
SIGKILL cannot be unwound by a shell trap. Absence of the final PASS marker is
a hard stop; never advance directly to a GPU-enabled image.
