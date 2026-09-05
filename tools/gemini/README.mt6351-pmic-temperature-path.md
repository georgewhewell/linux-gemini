# MT6351 PMIC temperature acquisition: source evidence

Source-only audit, 2026-09-05. No PMIC access, ADC request, efuse read,
calibrated temperature, or hardware qualification was performed.

Vendor source: `gemini-vendor-kernel-3.18` commit
`8cfe6596a503612e3332d9c26e292a19525a7f07`.
Paths below are relative to that tree.

## Actual acquisition path

`drivers/misc/mediatek/power/mt6797/pmic_auxadc.c`, function
`PMIC_IMM_GetOneChannelValue()`, implements the AP channel-4 path:

| Register | Field | Vendor action / interpretation |
| --- | --- | --- |
| AUXADC_CON0, 0x0ea2 | bit 15, CK_AON | set before conversion |
| TOP_CKPDN_CON0, 0x023a | bit 9, AUXADC_SMPS_CK_PDN | clear before conversion |
| AUXADC_DCXO_MDRT_2, 0x0f32 | bit 4, DCXO_CH4_MUX_AP_SEL | clear for PMIC die; set for DCXO |
| AUXADC_RQST0_SET, 0x0e98 | bit 4 | request AP channel 4 |
| AUXADC_ADC4, 0x0e08 | bit 15 | conversion ready |
| AUXADC_ADC4, 0x0e08 | bits 11:0 | raw conversion |

Addresses and fields come from
`drivers/misc/mediatek/include/mt-plat/mt6797/include/mach/upmu_hw.h`.
The vendor reads AUXADC_STA0 (0x0e90), delays 50 microseconds, and polls
readiness with 1300–1500 microsecond sleeps. Channel 4 converts raw ADC
to integer millivolts as `raw * 1800 / 4096`; it is not a temperature
in degrees and not a 15-bit channel. The separate MD channel-4 result
uses ADC19 and is not the AP acquisition endpoint.

## Do not inherit vendor failure behavior

The vendor breaks out of its readiness loop on timeout and still reads
and converts the output. It does not propagate every register-access
error. Its mux and clock writes also precede its ADC mutex. Copying
this literally would not establish serialized or fresh acquisition.
Clock power-down at function exit is commented out; this source alone
does not prove it is safe to restore clocks after an ambiguous request.

`drivers/misc/mediatek/thermal/mt6797/src/mtk_pmic_efuse.c` performs
the subsequent calibration/conversion and substitutes the previous
temperature for some implausible readings. A new implementation must
return an acquisition/calibration error instead of claiming a stale
sample is a new hardware measurement.

## Implementation boundary

Current integration has no MT6351 AUXADC match in its IIO ADC or
MediaTek thermal drivers. The existing `mt6359-auxadc.c` is a useful
IIO ownership model, not evidence that its register layout, reset,
request protocol or calibration applies to MT6351.

Before adding PMIC temperature to a candidate, implement an owner that
serializes the shared channel-4 mux and request, validates clock and
request ownership, bounds polling, propagates regmap/timeout errors,
and never reads the MD result as the AP result. Resolve request
completion and shared-clock lifecycle before adding restoration writes.
Provide separate tests for raw acquisition and efuse-based temperature
conversion; neither a default calibration nor a cached previous sample
is evidence of calibrated live temperature.

This PMIC die sensor is separate from the four SoC thermal channels.
Battery NTC wiring is physically broken according to the user and must
not be represented by a synthetic or substituted battery temperature.
