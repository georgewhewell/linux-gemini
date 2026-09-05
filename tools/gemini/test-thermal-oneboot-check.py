#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "checker", Path(__file__).with_name("check-thermal-oneboot.py"))
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def fixture(uptime=10):
    lines = ["THERMAL_INVENTORY_BEGIN units=thermal_hwmon_millicelsius",
             "battery_temperature=hardware_disconnected owner_attested=plus_minus_only",
             "/proc/sys/kernel/random/boot_id=23e7d65b-6880-48ab-88c8-24319f6e4bca",
             f"/proc/uptime={uptime}.00 0.00",
             "/sys/class/hwmon/hwmon2/name=mt6797_sensors",
             "/sys/class/thermal/thermal_zone0/type=soc-thermal",
             "/sys/class/thermal/thermal_zone0/temp=36000"]
    for i, label in enumerate(checker.LABELS, 1):
        lines += [f"/sys/class/hwmon/hwmon2/temp{i}_label={label}",
                  f"/sys/class/hwmon/hwmon2/temp{i}_input={35000 + i}"]
    return "\n".join(lines + ["THERMAL_INVENTORY_END zones=1"])


class Checks(unittest.TestCase):
    def test_same_boot(self):
        self.assertEqual(len(checker.check([fixture(), fixture(20)])), 2)

    def test_physical_zero(self):
        checker.check([fixture().replace("35001", "0"), fixture(20)])

    def test_failures(self):
        bad = [fixture().replace("35001", "READ_ERROR"),
               fixture().replace("35001", "150001"),
               fixture().replace("MCU1 (A72)", "ABB"),
               fixture().replace("mt6797_sensors", "other"),
               fixture().replace("/temp4_input=35004", "/missing=35004"),
               fixture().replace("THERMAL_INVENTORY_END", "TRUNCATED"),
               fixture() + "\n/sys/class/hwmon/hwmon2/temp1_input=35001",
               fixture().replace("/temp1_input=35001", "/temp1_input=\nREAD_ERROR"),
               fixture().replace("zones=1", "zones=1\n/sys/class/hwmon/hwmon2/temp5_input=36000"),
               fixture().replace("/proc/uptime=10.00 0.00", "/proc/uptime=nan 0.00")]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                checker.check([text, fixture(20)])

    def test_reboot_or_stale_capture(self):
        for second in [fixture(), fixture(9), fixture(20).replace("23e7d65b", "33e7d65b")]:
            with self.subTest(second=second), self.assertRaises(ValueError):
                checker.check([fixture(), second])


if __name__ == "__main__":
    unittest.main()
