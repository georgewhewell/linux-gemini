#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check production FGADC raw decode, signed units, calibration and failures.

Vectors use the documented current step and rounded 359.86 uAh CAR step.
This checks numerical conversion, not installed-battery calibration.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/power/supply/mt6351-fgadc.c').read_text()
def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]
functions = '\n'.join(extract(s) for s in (
    'static s64 mt6351_fgadc_apply_current_calibration(',
    'static s64 mt6351_fgadc_apply_counter_calibration(',
    'static int mt6351_fgadc_read(',
))
constants = '\n'.join(re.findall(r'^#define MT6351_FGADC_(?:CURRENT_LSB_NA|CAR_LSB_NAH).*$', source, re.M))
model = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef int64_t s64;
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define div_s64(n,d) ((n)/(d))
struct mt6351_fgadc {
 u32 resistance_mohm, board_base, board_slope, car_tune_percent, trim_permille, osr;
};
static unsigned int current_raw, car_hi_raw, car_lo_raw, captures;
static int capture_error;
static int mt6351_fgadc_capture(struct mt6351_fgadc *fg,unsigned int *current,
 unsigned int *hi,unsigned int *lo)
{
 (void)fg; captures++; *current=current_raw; *hi=car_hi_raw; *lo=car_lo_raw;
 return capture_error;
}
'''
main = r'''
int main(void)
{
 struct mt6351_fgadc fg={10,1000,1000,115,1000,8};
 struct {unsigned int raw; s64 ua;} current_vectors[]={
  {0,0},{1,2300},{2,6900},{100,363400},{1000,3636300},
  {0xffff,0},{0xfffe,-2300},{0xfffd,-6900},{0xff9b,-363400},
 };
 s64 current, counter;
 for (unsigned int i=0;i<sizeof(current_vectors)/sizeof(current_vectors[0]);i++) {
  current_raw=current_vectors[i].raw;
  assert(!mt6351_fgadc_read(&fg,&current,&counter));
  assert(current==current_vectors[i].ua && counter==0);
 }
 /* CAR is one's complement with a separate sign bit and two zero codes. */
 struct {unsigned int raw; int negative; s64 uah;} car_vectors[]={
  {0,0,0},{0,1,0},{0x1ffff,0,0},{0x1ffff,1,0},
  {1,0,103},{2,0,207},{100,0,10345},{1000,0,103458},
  {0x1fffe,1,-103},{0x1fffd,1,-207},{0x1ff9b,1,-10345},
 };
 current_raw=0;
 for (unsigned int i=0;i<sizeof(car_vectors)/sizeof(car_vectors[0]);i++)
 for (unsigned int reserved=0;reserved<2;reserved++) {
  unsigned int raw=car_vectors[i].raw;
  car_hi_raw=(raw>>5) | (car_vectors[i].negative ? BIT(15) : 0) |
   (reserved ? 0x7000 : 0);
  car_lo_raw=((raw&31)<<11) | (reserved ? 0x7ff : 0);
  assert(!mt6351_fgadc_read(&fg,&current,&counter));
  assert(current==0 && counter==car_vectors[i].uah);
 }
 /* The board-factor correction must scale both quantities, not their signs. */
 car_hi_raw=0; car_lo_raw=1<<11; current_raw=1; fg.car_tune_percent=118;
 assert(!mt6351_fgadc_read(&fg,&current,&counter) && current==2360 && counter==106);
 fg.car_tune_percent=100; fg.resistance_mohm=20;
 assert(!mt6351_fgadc_read(&fg,&current,&counter) && current==1000 && counter==45);
 /* Board slope belongs to instantaneous current, not accumulated charge. */
 fg.board_slope=2000;
 assert(!mt6351_fgadc_read(&fg,&current,&counter) && current==500 && counter==45);
 fg.trim_permille=1010;
 assert(!mt6351_fgadc_read(&fg,&current,&counter) && current==505 && counter==45);
 int errors[]={-EIO,-ETIMEDOUT};
 for (unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  capture_error=errors[i]; current=123456; counter=-654321;
  assert(mt6351_fgadc_read(&fg,&current,&counter)==errors[i]);
  assert(current==123456 && counter==-654321);
 }
 printf("PASS %u coherent FGADC conversions: signed units, calibration, reserved bits and errors\n",captures);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6351-fgadc-conversion-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(model + constants + '\n' + functions + main)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
