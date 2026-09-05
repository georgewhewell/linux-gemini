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
registration of zone index zero expose only the aggregate. This branch adds
`mt6797_sensors` hwmon with four labelled `temp*_input` channels for MCU1–4,
using existing calibrated conversion and the driver's bank lock, restoring
the complete selector after every read. Unpopulated zero ADC samples and
invalid converted temperatures return `-EAGAIN`, never fake 0 C. The aggregate
thermal zone and its cooling policy remain unchanged. No ABB channel or
duplicate A53 thermometers are invented. Userspace must not bypass the
driver with arbitrary MMIO bank writes.

The base DT leaves thermal and AUXADC disabled. Thermal overlays enable only
the thermal controller; its driver owns indirect AUXADC sampling and its clock.
The standalone AUXADC driver must remain disabled. A GPU-only image can
therefore have no zone by construction.
Check the exact assembled DT/config before treating missing sysfs as a bug.
The new sensor-only overlay enables the existing thermal controller and
one aggregate zone, with no CPU clock/CSPM, cpufreq, Hall, trips or cooling
changes. Required configs are `CONFIG_MTK_THERMAL`, `CONFIG_MTK_SOC_THERMAL`,
`CONFIG_THERMAL_OF`, `CONFIG_HWMON`, and the existing clock/NVMEM providers.
The thermal module must actually be included/loaded in the initramfs.

For saved before/after inventories, run:

```
python3 tools/gemini/check-thermal-oneboot.py thermal-before thermal-final
```

This requires one four-channel `mt6797_sensors` provider with the exact physical
labels, valid numeric readings, one readable `soc-thermal` zone, and a common
boot ID with advancing uptime. It rejects missing/read-error channels and
rebooted or duplicated captures. A physical 0 C reading is valid. The result
is only `THERMAL_INTERFACES_PASS`: it does not prove calibration accuracy,
workload safety, cooling action, GPU rendering, or battery temperature.
`test-thermal-oneboot-check.py` exercises the host checker without hardware.

Retained vendor boot lines 1060–1073 show calibration words
`995ff095 01529771 48a3c000`, enable=1, gain=613, offset=511,
calibration temperature=56, slope=0 and VTS MCU1/2/3/4/ABB
169/151/149/145/143. These values satisfy the current parser bounds. This
is evidence that this device has usable fuses, not a new-mainline-boot
temperature measurement or independent laboratory calibration.

Validation: the full modified driver object cross-compiles with GCC12
against the prepared G3 kernel headers. The actual hwmon read function is
compiled by `test-thermal-physical-hwmon.py` against banked MMIO mocks,
checking all four mappings, lock ownership, complete selector restoration,
out-of-range and zero readings, and unsupported requests. These tests are
not hardware proof. Root retains sole ownership of device boot and capture.

## Other hardware is not covered by the SoC sensor

- PMIC die: vendor `mt6797/src/mtk_pmic_efuse.c:148` reads PMIC AUXADC CH4
  and applies PMIC efuse calibration. The current MT6351 MFD has no matching
  AUXADC child; `mt6359-auxadc.c` does not support MT6351. This is a missing
  driver/integration path, not an extra SoC thermal-zone declaration.
- Battery: **physically unavailable**. The owner confirms the damaged
  connector has only battery + and - connected; temperature sensing is
  disconnected. This cannot be restored in software and is outside this
  lane. Never synthesize a safe temperature or satisfy charging policy with
  a substitute. The vendor's initial synthetic 25000 mC is not sensor proof.
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
