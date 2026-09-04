#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real hwmon read routine against a banked-MMIO test backend."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/thermal/mediatek/auxadc_thermal.c").read_text()
mapping = source[source.index("static const struct {\n\tu8 bank;"):
                 source.index("static umode_t mt6797_hwmon_is_visible")]
reader = source[source.index("static int mt6797_hwmon_read("):
                source.index("static int mt6797_hwmon_read_string(")]
prefix = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define MT6797_TS_MCU1 0
#define MT6797_TS_MCU2 1
#define MT6797_TS_MCU3 2
#define MT6797_TS_MCU4 3
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define PTPCORESEL 0x400
enum hwmon_sensor_types { hwmon_temp, hwmon_other };
enum { hwmon_temp_input, hwmon_temp_label };
struct config { unsigned msr[2]; };
struct mtk_thermal {
 char *thermal_base; int lock; struct config *conf;
 int (*raw_to_mcelsius)(struct mtk_thermal *, int, u32);
};
struct device { struct mtk_thermal *data; };
static u32 selector, measurement[6][2];
static int locked, writes, converted_sensor, converted_temp;
static char base[0x500];
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void mutex_lock(int *l) { (void)l; assert(!locked); locked = 1; }
static void mutex_unlock(int *l) { (void)l; assert(locked); locked = 0; }
static u32 readl(char *p) {
 assert(locked);
 if (p - base == PTPCORESEL) return selector;
 assert((selector & 15) < 6);
 assert(p - base == 0 || p - base == 4);
 return measurement[selector & 15][(p - base) / 4];
}
static void writel(u32 v, char *p) {
 assert(locked && p - base == PTPCORESEL); selector = v; writes++;
}
static int convert(struct mtk_thermal *m, int sensor, u32 raw) {
 (void)m; assert(!locked); assert(raw == 3500u + sensor);
 converted_sensor = sensor; return converted_temp;
}
static bool mtk_thermal_temp_is_valid(int t) { return t >= -20000 && t <= 150000; }
"""
tests = r"""
int main(void) {
 struct config conf = { {0, 4} };
 struct mtk_thermal mt = { base, 0, &conf, convert };
 struct device dev = { &mt };
 long value;
 const u32 original = 0xa5a50105;
 for (int i = 0; i < 4; i++) {
  measurement[mt6797_hwmon_sensors[i].bank][mt6797_hwmon_sensors[i].slot] = 3500 + i;
 }
 for (int i = 0; i < 4; i++) {
  selector = original; converted_temp = 42000 + i; writes = 0;
  assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_input, i, &value) == 0);
  assert(value == 42000 + i && converted_sensor == i);
  assert(selector == original && !locked && writes == 2);
 }
 for (int i = 0; i < 4; i++) {
  selector = original; converted_temp = 200000;
  assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_input, i, &value) == -EAGAIN);
  assert(selector == original && !locked);
 }
 measurement[0][0] = 0;
 assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_input, 0, &value) == -EAGAIN);
 assert(selector == original && !locked);
 writes = 0;
 assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_input, -1, &value) == -EOPNOTSUPP);
 assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_input, 4, &value) == -EOPNOTSUPP);
 assert(mt6797_hwmon_read(&dev, hwmon_other, hwmon_temp_input, 0, &value) == -EOPNOTSUPP);
 assert(mt6797_hwmon_read(&dev, hwmon_temp, hwmon_temp_label, 0, &value) == -EOPNOTSUPP);
 assert(writes == 0 && !locked);
 puts("PASS: four mappings, selector restoration, invalid/zero samples, invalid requests");
}
"""
with tempfile.TemporaryDirectory(prefix="gemini-thermal-hwmon-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + mapping + reader + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-sign-compare", "-o",
                    str(path / "test"), str(path / "test.c")], check=True)
    subprocess.run([str(path / "test")], check=True)
