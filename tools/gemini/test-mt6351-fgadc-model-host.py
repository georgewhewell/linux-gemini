#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the production battery estimator, including the actual board curves."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'drivers/power/supply/mt6351-fgadc-model.h').read_text()
header = re.sub(r'^#include .*\n', '', header, flags=re.M)
dts = (root / 'arch/arm64/boot/dts/mediatek/mt6797-gemini-battery.dtsi').read_text()
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int64_t s64;
typedef uint64_t u64;
#define ARRAY_SIZE(a) ((int)(sizeof(a)/sizeof((a)[0])))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(v,a,b) min(max(v,a),b)
#define clamp_t(t,v,a,b) min(max((t)(v),(t)(a)),(t)(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define div_s64(a,b) ((a)/(b))
struct power_supply_vbat_ri_table {int vbat_uv,ri_uohm;};
struct power_supply_battery_ocv_table {int ocv,capacity;};
enum {POWER_SUPPLY_STATUS_UNKNOWN,POWER_SUPPLY_STATUS_CHARGING,
 POWER_SUPPLY_STATUS_DISCHARGING,POWER_SUPPLY_STATUS_NOT_CHARGING,POWER_SUPPLY_STATUS_FULL};
'''
arrays = []
for i in range(4):
    for prop, ctype, name in (
        ('ocv-capacity-table', 'power_supply_battery_ocv_table', 'ocv'),
        ('mediatek,resistance-table', 'power_supply_vbat_ri_table', 'res'),
    ):
        body = re.search(rf'{prop}-{i}\s*=\s*(.*?);', dts, re.S)[1]
        pairs = re.findall(r'<(\d+) (\d+)>', body)
        arrays.append(f'static const struct {ctype} {name}{i}[]={{' +
                      ','.join('{' + a + ',' + b + '}' for a, b in pairs) + '};')
config = re.search(r'mediatek,capacity-model\s*=\s*(.*?);', dts, re.S)[1]
config = re.findall(r'<\(?(-?\d+)\)? (\d+)>', config)
assert config == [('-10','4336000'), ('0','4312000'), ('25','4268000'), ('50','4211000')]
arrays.append('static const struct mt6351_fg_model board={.count=4,.profiles={')
for i, (temp, charge) in enumerate(config):
    arrays.append(f'{{{int(temp)*10},{charge},res{i},ARRAY_SIZE(res{i}),ocv{i},ARRAY_SIZE(ocv{i})}},')
arrays.append('}};')
main = r'''
static const struct power_supply_vbat_ri_table resistance[]={{4200000,100000},{3200000,100000}};
static const struct power_supply_battery_ocv_table ocv[]={{4200000,100},{3200000,0}};
static const struct mt6351_fg_model simple={.count=1,.profiles={{250,1000000,resistance,2,ocv,2}}};
int main(void)
{
 struct mt6351_fg_estimate e={0};
 struct mt6351_fg_sample s={3600000,-1000000,100000,250,POWER_SUPPLY_STATUS_DISCHARGING,1};
 assert(!mt6351_model_validate(&simple));
 assert(!mt6351_model_update(&simple,&e,&s));
 assert(e.capacity==50 && e.charge_uah==500000 && e.ocv_uv==3700000);
 /* Coulombs, not voltage jitter, control subsequent load samples. */
 s.seconds+=5;s.counter_uah-=10000;s.voltage_uv=4000000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==49);
 s.seconds+=5;s.counter_uah+=20000;s.current_ua=1000000;s.status=POWER_SUPPLY_STATUS_CHARGING;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==51);
 s.status=POWER_SUPPLY_STATUS_FULL;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==100);
 s.status=POWER_SUPPLY_STATUS_DISCHARGING;s.current_ua=-1000000;
 s.seconds+=5;s.counter_uah-=40000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==96);
 /* Zero reset, large discontinuity and unknown wrap all reacquire OCV. */
 s.voltage_uv=3600000;s.seconds+=5;s.counter_uah=0;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==50);
 s.seconds+=5;s.counter_uah=13000000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==50);
 s.seconds+=5;s.counter_uah=-13000000;s.voltage_uv=3700000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==60);
 s.seconds+=60;s.counter_uah-=10000;s.voltage_uv=4000000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==59);
 s.seconds+=61;s.counter_uah-=10000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==90);
 /* A clock reversal loses continuity too. */
 s.seconds=1;s.voltage_uv=3500000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==40);
 /* Empty anchor, then bounds at both ends of accumulated charge. */
 s.seconds+=5;s.voltage_uv=3100000;s.counter_uah-=100;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==0);
 s.seconds+=5;s.counter_uah-=100;s.voltage_uv=3300000;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==0);
 s.status=POWER_SUPPLY_STATUS_FULL;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==100);
 s.status=POWER_SUPPLY_STATUS_CHARGING;s.current_ua=1000000;s.voltage_uv=4100000;s.counter_uah+=100;
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==100);
 /* Ten minutes below C/50 permits a relaxed-voltage correction. */
 e=(struct mt6351_fg_estimate){0};s=(struct mt6351_fg_sample){3700000,0,100,250,POWER_SUPPLY_STATUS_NOT_CHARGING,1};
 assert(!mt6351_model_update(&simple,&e,&s) && e.capacity==50);
 s.voltage_uv=3800000;
 for (int i=1;i<=120;i++) {
  s.seconds=1+i*5;
  assert(!mt6351_model_update(&simple,&e,&s));
  assert(e.capacity==(i==120 ? 60 : 50));
 }
 /* Bad input never changes an established estimate. */
 struct mt6351_fg_estimate before=e;
 s.voltage_uv=0;
 assert(mt6351_model_update(&simple,&e,&s)==-ERANGE && !memcmp(&before,&e,sizeof(e)));
 s.voltage_uv=3700000;s.temp=INT_MAX;
 assert(mt6351_model_update(&simple,&e,&s)==-ERANGE && !memcmp(&before,&e,sizeof(e)));
 /* Multiple electrical roots must not become a made-up initial percentage. */
 static const struct power_supply_vbat_ri_table folded[]={{4200000,100000},{3200000,2100000}};
 struct mt6351_fg_model m=simple;m.profiles[0].resistance=folded;
 assert(!mt6351_model_validate(&m));
 e=(struct mt6351_fg_estimate){0};s=(struct mt6351_fg_sample){4600000,1000000,100000,250,POWER_SUPPLY_STATUS_CHARGING,1};
 assert(mt6351_model_update(&m,&e,&s)==-ENODATA && !e.initialized);
 s.status=POWER_SUPPLY_STATUS_FULL;
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==100 && !e.ocv_valid);
 s.status=POWER_SUPPLY_STATUS_CHARGING;s.seconds+=5;s.counter_uah-=10000;
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==99 && !e.ocv_valid);
 /* Invalid model shapes/units fail admission. */
 for(int i=0;i<8;i++) {
  m=simple;
  switch(i) {
   case 0:m.count=0;break;case 1:m.count=5;break;
   case 2:m.profiles[0].temp=INT_MAX;break;
   case 3:m.profiles[0].full_uah=0;break;
   case 4:m.profiles[0].resistance=NULL;break;
   case 5:m.profiles[0].resistance_count=1;break;
   case 6:m.profiles[0].ocv_count=101;break;
   case 7:m.profiles[0].ocv=NULL;break;
  }
  assert(mt6351_model_validate(&m)==-EINVAL);
 }
 /* Known constant-R temperature blend and Qmax interpolation. */
 m=simple;m.count=2;m.profiles[1]=m.profiles[0];m.profiles[1].temp=500;m.profiles[1].full_uah=2000000;
 assert(!mt6351_model_validate(&m));
 e=(struct mt6351_fg_estimate){0};s=(struct mt6351_fg_sample){3600000,-1000000,100,375,POWER_SUPPLY_STATUS_DISCHARGING,1};
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==50 && e.charge_uah==750000);
 e=(struct mt6351_fg_estimate){0};s.temp=500;s.voltage_uv=3900000;
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==80 && e.charge_uah==1600000);
 s.seconds+=5;s.temp=250;
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==100 && e.charge_uah==1600000);
 s.seconds+=5;s.temp=500;
 assert(!mt6351_model_update(&m,&e,&s) && e.capacity==80 && e.charge_uah==1600000);
 /* Board sweeps and inverse checks across both current signs and temperatures. */
 assert(!mt6351_model_validate(&board));
 unsigned int valid=0,ambiguous=0;
 for(int temp=-100;temp<=500;temp+=25)
 for(int ua=-1000000;ua<=1000000;ua+=250000)
 for(int uv=3300000;uv<=4400000;uv+=10000) {
  e=(struct mt6351_fg_estimate){0};
  s=(struct mt6351_fg_sample){uv,ua,100,temp,ua>0?POWER_SUPPLY_STATUS_CHARGING:POWER_SUPPLY_STATUS_DISCHARGING,1};
  int ret=mt6351_model_update(&board,&e,&s);
  if(ret) {assert(ret==-ENODATA && !e.initialized);ambiguous++;continue;}
  assert(e.initialized && e.ocv_valid && e.capacity>=0 && e.capacity<=100);
  int index=0;
  while(index+1<board.count && temp>board.profiles[index+1].temp)index++;
  const struct mt6351_fg_profile *lo=&board.profiles[index],*hi=&board.profiles[min(index+1,board.count-1)];
  s64 recovered=mt6351_model_loaded_voltage(lo,hi,temp,ua,e.ocv_uv);
  assert(llabs(recovered-uv)<=5);
  valid++;
 }
 printf("PASS estimator anchors, coulombs, resets/gaps, relaxation, input errors; %u board roots, %u ambiguous/out-of-range cases\n",valid,ambiguous);
}
'''
with tempfile.TemporaryDirectory(prefix='mt6351-model-') as tmp:
    src, exe = Path(tmp,'test.c'), Path(tmp,'test')
    src.write_text(prelude+header+'\n'+'\n'.join(arrays)+main)
    subprocess.run([os.environ.get('HOSTCC','cc'), '-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined','-fno-sanitize-recover=all',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
