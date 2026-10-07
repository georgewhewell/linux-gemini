#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check Gemini profile provenance and execute Linux's real OCV lookup.

These checks establish data and numerical behavior, not physical calibration
or a complete state-of-charge estimator.
"""

from fractions import Fraction
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DTS = ROOT / "arch/arm64/boot/dts/mediatek"
source = (DTS / "mt6797-gemini-battery.dtsi").read_text()
expected = (
    (94, "704a6e4e19740754d02b2e2b897a936e1e5de4c567c7050680771cf5dd51de41"),
    (87, "bfbbe4de2375830cfac5369fe1e970ba582884c562053effe3429a5aa27efb29"),
    (87, "df1bc0fcc84c571dd7cbc2f6de3ecbd3418f26b03ff7db1bf42a8921389b9c44"),
    (85, "c0006095345a99ca0233639d01af8e41e4c923422614eadd4b651a4efe8391cd"),
)
assert "ocv-capacity-celsius = <(-10) 0 25 50>;" in source
board = (DTS / "mt6797-gemini-pda.dts").read_text()
assert '#include "mt6797-gemini-battery.dtsi"' in board
fg = re.search(r"&mt6351fgadc\s*\{(.*?)\};", board, re.S)[1]
assert "monitored-battery = <&gemini_battery>;" in fg
# The voltage profiles do not establish pack design capacity or charge limits.
assert set(re.findall(r"([\w,-]+)\s*=", source.split("&mt6351fgadc")[0])) == {
    "compatible", "ocv-capacity-celsius",
    *(f"ocv-capacity-table-{i}" for i in range(4)),
}

tables = []
for i, (count, sha) in enumerate(expected):
    body = re.search(rf"ocv-capacity-table-{i}\s*=\s*(.*?);", source, re.S)[1]
    table = [tuple(map(int, p)) for p in re.findall(r"<(\d+) (\d+)>", body)]
    canonical = "".join(f"{v},{c}\n" for v, c in table).encode()
    assert len(table) == count and hashlib.sha256(canonical).hexdigest() == sha
    assert table[0][1] == 100 and table[-1] == (3400000, 0)
    assert all(a[0] > b[0] and a[1] >= b[1] for a, b in zip(table, table[1:]))
    tables.append(table)


def capacity_reference(table, uv):
    """Exact rational line segment, clamped at the table ends."""
    if uv >= table[0][0]:
        return table[0][1]
    if uv <= table[-1][0]:
        return table[-1][1]
    for (hi, hc), (lo, lc) in zip(table, table[1:]):
        if lo <= uv <= hi:
            capacity = (Fraction(hi - uv, hi - lo) * lc +
                        Fraction(uv - lo, hi - lo) * hc)
            return capacity.numerator // capacity.denominator
    raise AssertionError("uncovered voltage")


def extract(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


core = (ROOT / "drivers/power/supply/power_supply_core.c").read_text()
arith = (ROOT / "include/linux/fixp-arith.h").read_text()
functions = extract(arith, "static inline int fixp_linear_interpolate(")
for signature in (
    "int power_supply_ocv2cap_simple(",
    "const struct power_supply_battery_ocv_table *\npower_supply_find_ocv2cap_table(",
    "int power_supply_batinfo_ocv2cap(",
):
    functions += extract(core, signature)

c = [r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
#define POWER_SUPPLY_OCV_TEMP_MAX 20
struct power_supply_battery_ocv_table { int ocv, capacity; };
struct power_supply_battery_info {
 int ocv_temp[POWER_SUPPLY_OCV_TEMP_MAX];
 int ocv_table_size[POWER_SUPPLY_OCV_TEMP_MAX];
 const struct power_supply_battery_ocv_table *ocv_table[POWER_SUPPLY_OCV_TEMP_MAX];
};
''', functions]
for i, table in enumerate(tables):
    c.append(f"static const struct power_supply_battery_ocv_table t{i}[] = {{")
    c.extend(f"{{{uv},{cap}}}," for uv, cap in table)
    c.append("};")

vectors = []
for i, table in enumerate(tables):
    voltages = set(range(2900000, 4500001, 1000))
    voltages.update(uv + offset for uv, _ in table for offset in (-1, 0, 1))
    voltages.update((a[0] + b[0]) // 2 for a, b in zip(table, table[1:]))
    vectors.extend((i, uv, capacity_reference(table, uv)) for uv in sorted(voltages))
c.append("static const struct { int index, uv, capacity; } vectors[] = {")
c.extend(f"{{{i},{uv},{cap}}}," for i, uv, cap in vectors)
c.append("};")
c.append(r'''
int main(void)
{
 struct power_supply_battery_info info={
  .ocv_temp={-10,0,25,50},
  .ocv_table_size={94,87,87,85},
  .ocv_table={t0,t1,t2,t3},
 };
 unsigned int i;
 for (i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++) {
  int index=vectors[i].index;
  int cap=power_supply_batinfo_ocv2cap(&info,vectors[i].uv,info.ocv_temp[index]);
  assert(cap==vectors[i].capacity && cap>=0 && cap<=100);
 }
 /* Test all temperatures and midpoints: nearest table, cold wins ties. */
 for (int temp=-60;temp<=100;temp++) {
  int index=temp<=-5 ? 0 : temp<=12 ? 1 : temp<=37 ? 2 : 3;
  int len=0;
  assert(power_supply_find_ocv2cap_table(&info,temp,&len)==info.ocv_table[index]);
  assert(len==info.ocv_table_size[index]);
 }
 struct power_supply_battery_info absent={0};
 assert(power_supply_batinfo_ocv2cap(&absent,4000000,25)==-EINVAL);
 printf("PASS %u OCV lookup vectors, 161 temperatures, absent profile\n",i);
 return 0;
}
''')
with tempfile.TemporaryDirectory(prefix="gemini-battery-profile-") as tmp:
    src, exe = Path(tmp, "test.c"), Path(tmp, "test")
    src.write_text("\n".join(c))
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Exercise the importer boundary independently of the immutable source hash.
spec = importlib.util.spec_from_file_location(
    "profile_import", Path(__file__).with_name("import-gemini-battery-profile.py"))
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)
valid = "BATTERY_PROFILE_STRUCT battery_profile_t0[] = {" + \
        "{0,4370},{50,3800}," + "{100,3400}," * 94 + "};"
assert importer.extract_profile(valid, 0) == [(4370000, 100), (3800000, 50), (3400000, 0)]
for bad in (
    valid + valid,
    valid.replace("{50,3800}", "{50,4370}"),  # same voltage, different DoD
    valid.replace("{50,3800}", "{101,3800}"),
    valid.replace("{50,3800}", "{50,4400}"),
    valid.replace("{50,3800},", ""),
    valid.replace("{50,3800}", "{50,3800 + 1}"),
    valid.replace("{100,3400}", "{100,3300}"),
):
    try:
        importer.extract_profile(bad, 0)
    except ValueError:
        pass
    else:
        raise AssertionError("malformed profile accepted")
print("PASS original-profile hashes, board reference and importer rejection cases")
