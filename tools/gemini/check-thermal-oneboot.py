#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check saved inventories; this proves readable interfaces, not thermal safety."""
import argparse
from pathlib import Path
import re
import sys
import uuid

LABELS = ("MCU1 (A72)", "MCU2 (A53/SoC shared)", "MCU3 (SoC)", "MCU4 (GPU)")


def snapshot(text):
    lines = text.splitlines()
    if sum(line.startswith("THERMAL_INVENTORY_BEGIN ") for line in lines) != 1:
        raise ValueError("missing or repeated inventory beginning")
    if not lines or not lines[-1].startswith("THERMAL_INVENTORY_END "):
        raise ValueError("incomplete inventory")
    if "battery_temperature=hardware_disconnected owner_attested=plus_minus_only" not in lines:
        raise ValueError("missing disconnected-battery qualification")
    attrs = {}
    for line in lines:
        if line.startswith("/") and "=" in line:
            key, value = line.split("=", 1)
            if key in attrs:
                raise ValueError(f"duplicate attribute: {key}")
            attrs[key] = value
    boot = attrs.get("/proc/sys/kernel/random/boot_id", "")
    if str(uuid.UUID(boot)) != boot:
        raise ValueError("invalid boot ID")
    uptime = attrs.get("/proc/uptime", "").split()
    if len(uptime) != 2 or any(not re.fullmatch(r"[0-9]+\.[0-9]+", x) for x in uptime):
        raise ValueError("invalid uptime")
    providers = [key[:-5] for key, value in attrs.items()
                 if re.fullmatch(r"/sys/class/hwmon/hwmon[0-9]+/name", key)
                 and value == "mt6797_sensors"]
    if len(providers) != 1:
        raise ValueError("expected exactly one mt6797_sensors provider")
    provider = providers[0]

    def temperature(key):
        value = attrs.get(key, "")
        if not re.fullmatch(r"-?[0-9]+", value):
            raise ValueError(f"unreadable temperature: {key}")
        number = int(value)
        if not -20000 <= number <= 150000:
            raise ValueError(f"temperature outside driver validity range: {key}")
        return number

    channels = {key for key in attrs if key.startswith(provider + "/temp")
                and key.endswith("_input")}
    expected = {f"{provider}/temp{i}_input" for i in range(1, 5)}
    if channels != expected:
        raise ValueError("expected exactly four physical sensor channels")
    values = []
    for i, label in enumerate(LABELS, 1):
        if attrs.get(f"{provider}/temp{i}_label") != label:
            raise ValueError(f"incorrect physical sensor label {i}")
        fault = attrs.get(f"{provider}/temp{i}_fault", "0")
        if fault != "0":
            raise ValueError(f"sensor {i} reports a fault")
        values.append(temperature(f"{provider}/temp{i}_input"))
    zones = [key[:-5] for key, value in attrs.items()
             if re.fullmatch(r"/sys/class/thermal/thermal_zone[0-9]+/type", key)
             and value == "soc-thermal"]
    if len(zones) != 1:
        raise ValueError("expected exactly one soc-thermal zone")
    aggregate = temperature(zones[0] + "/temp")
    # Sequential hardware samples need not match exactly; no equality invented.
    return boot, float(uptime[0]), values, aggregate


def check(texts):
    if len(texts) < 2:
        raise ValueError("require before and after inventories")
    samples = [snapshot(text) for text in texts]
    if len({sample[0] for sample in samples}) != 1:
        raise ValueError("inventories cross a reboot")
    if any(b[1] <= a[1] for a, b in zip(samples, samples[1:])):
        raise ValueError("inventories must advance uptime in supplied order")
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventories", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        samples = check([path.read_text() for path in args.inventories])
    except (OSError, ValueError) as error:
        print(f"THERMAL_INTERFACES_FAIL {error}", file=sys.stderr)
        return 1
    print(f"THERMAL_INTERFACES_PASS snapshots={len(samples)} channels=4 same_boot=yes")
    print("scope=readable_interfaces_only calibration_accuracy=unverified "
          "thermal_protection=unverified battery_temperature=disconnected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
