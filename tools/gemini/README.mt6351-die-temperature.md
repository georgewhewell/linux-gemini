# MT6351 die temperature: integration boundary

2026-09-05. This is source evidence and an implementation contract, not a
hardware result. No PMIC register transaction or device boot was performed.
Battery NTC is physically disconnected (owner: only + and - wired); it is
explicitly excluded. No synthetic battery temperature or charging changes.

## Minimal viable production path

Add one opt-in MT6351 MFD child that owns die-AUXADC and thermal-OTP
transactions under one mutex, and exposes one `temp1_input`/`temp1_label`
through hwmon. It must consume the parent regmap, not remap PMIC-wrapper
MMIO, and refuse unknown PMIC IDs. Do not attach it to the FGADC battery
driver: FGADC is a separate block with separate calibration and ownership.

There is currently no MT6351 AUXADC child in `drivers/mfd/mt6397-core.c`,
no MT6351 support in `drivers/iio/adc/mt6359-auxadc.c`, and no MT6351 OTP
provider in `drivers/nvmem`. Existing MT6351 codec/FGADC/regulator code
does not reference AUXADC or OTP transactions. Thus one dedicated owner
can be introduced without competing Linux owners today. This does not
prove absence of firmware access; retain modem/GPS state and use only AP
CH4 request/mux fields. Future ADC/OTP consumers must share the owner,
not create private mutexes around the same engine.

Separating generic IIO and NVMEM providers is a possible later cleanup,
but it would add two new drivers and a thermal consumer now. Adding
MT6351 as a mere compatible alias of MT6359 is incorrect: MT6351 has SET/
CLR request registers, additional clock/mux requirements and a different
calibration layout. A die-only child must never expose battery channels.

## Exact vendor transaction evidence

Vendor root: `/mnt/Home/src/gemini-vendor-kernel-3.18`.
ADC source: `drivers/misc/mediatek/power/mt6797/pmic_auxadc.c`, function
`PMIC_IMM_GetOneChannelValue` (line 235). Constants from
`drivers/misc/mediatek/include/mt-plat/mt6797/include/mach/upmu_hw.h`.

| Register | Field | Meaning |
| --- | --- | --- |
| 0x023a | bit 9 = 0 | AUXADC SMPS clock enabled |
| 0x0ea2 | bit 15 = 1 | AP AUXADC clock always-on |
| 0x0f32 | bit 4 = 0 | CH4 die sensor, not DCXO mux |
| 0x0e96 | bit 4 | AP CH4 request state |
| 0x0e98 | write bit 4 | SET AP CH4 request |
| 0x0e9a | write bit 4 | CLR AP CH4 request |
| 0x0e08 | bit 15 | CH4 conversion ready |
| 0x0e08 | bits 11:0 | CH4 raw ADC code |

Vendor waits 50 us after requesting, then polls in 1.3–1.5 ms intervals
with a nominal count limit of 15, but erroneously consumes data even on
timeout. New code must use a bounded deadline and return timeout, never
stale data. CH4 output converts to millivolts as `raw * 1800 / 4096`.
The thermal conversion takes **millivolts**, not raw ADC codes.

The vendor global AUXADC init also programs averaging at 0x0ea8
(large bits 5:3 = 6, small bits 2:0 = 2), selector at 0x0eaa (0x83),
and VBUF bit 9 at 0x0eba. These affect shared channels: do not blindly
copy the global initializer into a die-only read. Before implementation
decide whether to preserve boot initialization, or establish exclusive
whole-engine ownership and restore every affected field. CH4 uses the
small averaging setting in the vendor selector. No battery channel 3,
BATON, BIF, charger or fuel-gauge latch should be touched.

## Thermal OTP/calibration

`thermal/mt6797/src/mtk_pmic_efuse.c` reads words 0 and 1 using
`power/mt6797/pmic.c:4965`, `pmic_Read_Efuse_HPOffset`. That reader:

1. Clears EFUSE HW clock gate bit 6 at 0x0282 and clock gate bit 2 at 0x024c.
2. Sets software-read bit 0 at 0x0c16.
3. Sets row address bits 5:0 at 0x0c00 to `word_index * 2`.
4. Waits 100 us and toggles read-trigger bit 0 at 0x0c10.
5. Waits 300 us, polls busy bit 0 at 0x0c1a, then waits at least 1 ms.
6. Reads the 16-bit word from 0x0c18.

The original has an unbounded busy loop and ignores bus errors; neither
is acceptable. Restore only owned mode/address/clock fields, including
on error. **Do not restore the trigger by toggling it again:** that would
start another OTP read. Never touch OTP programming controls/keys.

Decode:

- calibration-enable = word0 bit0; disabled means unavailable, not defaults;
- calibration temperature = word0 bits6:1;
- VTS = (word1 bits3:0 << 9) | word0 bits15:7;
- slope-sign = word1 bit4, slope = word1 bits10:5, ID = word1 bit11;
- ID=0 forces slope=0 in the vendor contract.

Vendor replaces calibration temperatures outside 38..60 with 53 and
disabled calibration with VTS=1600/temperature=50. Do not copy these
substitutions into a driver claiming calibrated readings. Reject missing
or invalid fuses, retain raw words for diagnosis, and report unavailable.

Vendor conversion uses deliberately different slope factors in slope2
and intercept: slope2 = -(1598 +/- slope), whereas intercept denominator
is -(1598 +/- slope*10). Preserve the source contract pending evidence;
do not silently "fix" this to a prettier equation. Intermediate arithmetic
needs signed 64-bit: VTS is 13 bits and the legacy intermediate can
overflow signed 32-bit for malformed values. Limit final temperatures
to the vendor accepted range -50000..150000 mC and reject invalid values;
do not replace a bad reading with cached plausible data.

No PMIC thermal efuse words or live CH4 samples were found in the retained
vendor boot log `gemini-gpu-vendor-oracle-20260902/03-vendor-dmesg-full.txt`.
Its SoC calibration words are unrelated and must not be reused here.

## Before one useful shared hardware session

The implementation needs error-injection tests for every regmap read/write,
busy timeout, ADC timeout, pre-existing request refusal, failed cleanup,
and calibration/overflow limits. A failed restore must poison subsequent
reads until a new driver lifecycle; a successful ADC sample is not success
if ownership/configuration was not restored. Verify no writes outside the
small register/bit allowlist, including OTP programming and battery blocks.
Read die temperature alongside the already planned workload/SoC inventory;
do not schedule another collector-only thermal boot.
