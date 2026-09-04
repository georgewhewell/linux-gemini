# Gemini thermal: actual boundary, 2026-09-05

Run `sh thermal-oneboot-inventory.sh` over the working SSH connection before
and after the existing bounded GPU/A72 workload. It reads existing sysfs
interfaces only, changes no policy, and must not be interpreted as a pass
for calibration or cooling. No separate thermal-only boot is needed.

## What exists

The MT6797 driver in `drivers/thermal/mediatek/auxadc_thermal.c` already
programs six hardware banks and uses required, validated NVMEM calibration.
It exports **one maximum temperature**, not six independently measured
hardware temperatures. The vendor source confirms this mapping in
`drivers/misc/mediatek/thermal/mt6797/src/mtk_tc.c:71`:

| Bank | Vendor domain | Physical sensor |
| --- | --- | --- |
| 0 | BIG / A72 | TS_MCU1 |
| 1 | GPU | TS_MCU4 |
| 2 | SoC | TS_MCU2 and TS_MCU3 |
| 3 | CPU-L / A53 | TS_MCU2, shared |
| 4 | CPU-LL / A53 | TS_MCU2, shared |
| 5 | CCI | TS_MCU2, shared |

There are five calibration entries including ABB, but the active bank table
does not sample ABB. Do not claim five readable independent sensors or a
separate thermometer per CPU. Existing `#thermal-sensor-cells = <0>` and
registration of zone index zero expose only the aggregate. Per-sensor hwmon
inputs require a driver addition using the driver's bank lock and calibrated
conversion; userspace must not bypass it with arbitrary MMIO bank writes.

The base DT leaves thermal and AUXADC disabled. Existing thermal overlays
enable them; a GPU-only image can therefore have no zone by construction.
Check the exact assembled DT/config before treating missing sysfs as a bug.

## Other hardware is not covered by the SoC sensor

- PMIC die: vendor `mt6797/src/mtk_pmic_efuse.c:148` reads PMIC AUXADC CH4
  and applies PMIC efuse calibration. The current MT6351 MFD has no matching
  AUXADC child; `mt6359-auxadc.c` does not support MT6351. This is a missing
  driver/integration path, not an extra SoC thermal-zone declaration.
- Battery: vendor battery thermal delegates to the battery-meter path,
  including an initial **synthetic 25000 mC** reading. A plausible 25 C
  alone is not proof of a live thermistor. Need verified MT6351 battery ADC
  acquisition plus actual board thermistor conversion before claiming it.
- DA9214: vendor `common/thermal_zones/ts_da9214.c:69` maps threshold status
  to fabricated representative readings 60000/125000/140000 mC. This is a
  threshold alarm, not a calibrated continuous thermometer.
- Connectivity: retained vendor boot
  `/mnt/Home/src/gemini-gpu-vendor-oracle-20260902/03-vendor-dmesg-full.txt`
  contains live WMT thermal responses (for example line 5160, 0x1d).
  Requires connectivity firmware transport; not a SoC ADC channel. Early
  zero responses explicitly occur while STP is not ready.
- Modem/PA, camera, charger and DRAM: vendor thermal-zone names alone do not
  establish populated physical sensors or functional mainline reads. These
  remain unverified; do not invent hardware coverage from virtual zones.

## Why previous thermal boot failed

The retained R2 pstore in
`handoff/a53-thermal-p22-f2fc2447-e123c198-one-shot-20260904T221000+0200/console-ramoops-0.raw`
records a combined collector failure after CSPM ownership/regulator/cpufreq
errors. It does not establish a thermal conversion failure or a successful
temperature read. The earlier run also deferred thermal probing behind its
clock/NVMEM problems. Decouple sensor reads from CPU-DVFS acceptance:
inventory the live session directly and retain failures as well as values.

Readability, fuse-valid calibration, per-sensor identity, and functioning
cooling action are separate claims. No thermal shutdown, heating test, or
cooling-policy emulation is authorized by this inventory helper.
