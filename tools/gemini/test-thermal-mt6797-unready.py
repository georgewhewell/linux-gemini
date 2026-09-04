#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test real MT6797 conversion, bank aggregation and thermal-zone callback."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/thermal/mediatek/auxadc_thermal.c").read_text()


def function(name):
    # Locate the declaration, then take its balanced function body verbatim.
    pos = source.index(name + "(")
    start = source.rfind("\nstatic ", 0, pos) + 1
    opening = source.index("{", pos)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


prefix = r"""
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t u32;
#define THERMAL_TEMP_INVALID -274000
#define MT8173_TEMP_MIN -20000
#define MT8173_TEMP_MAX 150000
#define max(a,b) ((a) > (b) ? (a) : (b))
struct mtk_thermal;
struct mtk_thermal_bank { struct mtk_thermal *mt; unsigned id; };
struct thermal_bank_cfg { unsigned num_sensors; const int *sensors; };
struct mtk_thermal_data {
 unsigned num_banks; struct thermal_bank_cfg bank_data[6]; unsigned msr[2];
};
struct mtk_thermal {
 char *thermal_base; s32 adc_ge, adc_oe, degc_cali, o_slope, o_slope_sign;
 s32 vts[5]; const struct mtk_thermal_data *conf;
 struct mtk_thermal_bank banks[6];
 int (*raw_to_mcelsius)(struct mtk_thermal *, int, s32);
};
struct thermal_zone_device { struct mtk_thermal *data; };
static struct mtk_thermal_data mt6797_thermal_data;
static char registers[8];
static u32 samples[6][2];
static int selected = -1, gets, puts_count;
static void *thermal_zone_device_priv(struct thermal_zone_device *tz) { return tz->data; }
static void mtk_thermal_get_bank(struct mtk_thermal_bank *bank) {
 assert(selected == -1); selected = bank->id; gets++;
}
static void mtk_thermal_put_bank(struct mtk_thermal_bank *bank) {
 assert(selected == (int)bank->id); selected = -1; puts_count++;
}
static u32 readl(char *addr) {
 assert(selected >= 0 && selected < 6);
 assert(addr - registers == 0 || addr - registers == 4);
 return samples[selected][(addr - registers) / 4];
}
"""
tests = r"""
int main(void) {
 const int sensors[6][2] = {{0,0},{3,0},{1,2},{1,0},{1,0},{1,0}};
 /* Actual vendor efuse decode: gain613, offset511, degc56, slope0. */
 struct mtk_thermal mt = {
  .thermal_base=registers, .adc_ge=613, .adc_oe=511, .degc_cali=56,
  .vts={169,151,149,145,143}, .conf=&mt6797_thermal_data,
  .raw_to_mcelsius=raw_to_mcelsius_v4
 };
 struct thermal_zone_device tz = { &mt };
 mt6797_thermal_data.num_banks=6;
 mt6797_thermal_data.msr[1]=4;
 for (int bank=0; bank<6; bank++) {
  mt6797_thermal_data.bank_data[bank].num_sensors=bank==2 ? 2 : 1;
  mt6797_thermal_data.bank_data[bank].sensors=sensors[bank];
  mt.banks[bank].mt=&mt; mt.banks[bank].id=bank;
 }
 for (int sensor=0; sensor<5; sensor++) {
  assert(raw_to_mcelsius_v4(&mt,sensor,0)==THERMAL_TEMP_INVALID);
  assert(raw_to_mcelsius_v4(&mt,sensor,0x1000)==THERMAL_TEMP_INVALID);
  assert(raw_to_mcelsius_v4(&mt,sensor,0x80000000u)==THERMAL_TEMP_INVALID);
  assert(raw_to_mcelsius_v4(&mt,sensor,3500)==raw_to_mcelsius_v4(&mt,sensor,0xf000|3500));
 }
 int temperature=777;
 assert(mtk_read_temp(&tz,&temperature)==-EAGAIN && temperature==777);
 assert(gets==6 && puts_count==6 && selected==-1);
 for (int bank=0; bank<6; bank++) for (int slot=0; slot<2; slot++) samples[bank][slot]=0xf000;
 assert(mtk_read_temp(&tz,&temperature)==-EAGAIN && temperature==777);
 for (int bank=0; bank<6; bank++) for (int slot=0; slot<2; slot++) samples[bank][slot]=1;
 assert(mtk_read_temp(&tz,&temperature)==-EAGAIN && temperature==777);
 memset(samples,0,sizeof(samples));
 samples[2][1]=3500;
 assert(mtk_read_temp(&tz,&temperature)==0);
 assert(temperature==raw_to_mcelsius_v4(&mt,2,3500));
 /* Find a genuine calibrated 0 C; zero temperature is not an error sentinel. */
 bool found=false;
 for (int bank=0; bank<6 && !found; bank++) {
  for (int slot=0; slot<(bank==2 ? 2 : 1) && !found; slot++) {
   for (int raw=1; raw<4096; raw++) {
    if (raw_to_mcelsius_v4(&mt,sensors[bank][slot],raw)!=0) continue;
    memset(samples,0,sizeof(samples)); samples[bank][slot]=raw;
    temperature=777;
    assert(mtk_read_temp(&tz,&temperature)==0 && temperature==0);
    printf("legitimate_zero sensor=%d raw=%d\n",sensors[bank][slot],raw);
    found=true; break;
   }
  }
 }
 assert(found);
 /* Preserve the pre-existing return convention for other SoC descriptors. */
 struct mtk_thermal_data other=mt6797_thermal_data;
 mt.conf=&other; memset(samples,0,sizeof(samples));
 assert(mtk_read_temp(&tz,&temperature)==0 && temperature==THERMAL_TEMP_INVALID);
 assert(gets==puts_count && selected==-1);
 puts("PASS: real conversion/bank/zone code rejects empty samples, preserves physical 0 C and other SoCs");
}
"""
body = "".join(function(name) for name in (
    "mtk_thermal_temp_is_valid", "raw_to_mcelsius_v4",
    "mtk_thermal_bank_temperature", "mtk_read_temp"))
with tempfile.TemporaryDirectory(prefix="gemini-thermal-unready-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + body + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-sign-compare", "-o",
                    str(path / "test"), str(path / "test.c")], check=True)
    subprocess.run([str(path / "test")], check=True)
