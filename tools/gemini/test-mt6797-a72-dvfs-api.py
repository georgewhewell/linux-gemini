#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production A72 DVFS owner API and core against hardware doubles."""
import os
import re
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
def strip(s):
    return re.sub(r'^#include .*\n', '', s, flags=re.MULTILINE)
core = '\n'.join(strip((root / p).read_text()) for p in (
    'drivers/soc/mediatek/mt6797-a72-power.h',
    'drivers/soc/mediatek/mt6797-a72-power-core.c'))
source = (root / 'drivers/soc/mediatek/mt6797-a72-power.c').read_text()
a = source.index('static struct task_struct *mt6797_a72_dvfs_task;')
b = source.index('\nstatic int mt6797_a72_overlay_notify(', a)
api = source[a:b]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define static_assert _Static_assert
#define READ_ONCE(p) (p)
#define WRITE_ONCE(p,v) ((p)=(v))
#define EXPORT_SYMBOL_GPL(x)
#define MT6797_A72_IDVFS_CONTROL_REG 0x10222470U
'''
stubs = r'''
struct device { int tag; };
struct task_struct { int tag; };
static struct task_struct task, other_task;
static struct task_struct *test_current=&task;
#define current test_current
struct mutex { bool held; };
struct mt6797_sram_sample { u32 control, calibration; };
struct mt6797_a72_preflight {
 void *dev, *vproc_big, *sram_big, *pwrap_reset, *spm, *mp2_clk;
 struct mutex operation_lock;
 struct mt6797_sram_sample sram_boot;
 unsigned int boot_vproc_uv; bool dvfs_used;
 bool cpu9_starting;
 bool faulted, started, committed[2], off_armed[2], cpu8_armed, guarded, vproc_vote_owned;
 struct { bool active; } transaction;
};
static struct mutex mt6797_a72_owner_lock;
static struct mt6797_a72_preflight p, *mt6797_a72_owner;
static bool online[10], cpus_held, effect;
static int step, fail_step, writes, clock_calls, enabled, reset_status, vproc, waits;
static u32 idvfs, isolation, power;
static struct mt6797_sram_sample sample;
static bool cpu_online(unsigned cpu) { assert(cpu<10); return online[cpu]; }
static void mutex_lock(struct mutex *m) {
 assert(!m->held);
 if(m==&p.operation_lock) assert(mt6797_a72_owner_lock.held);
 m->held=true;
}
static void mutex_unlock(struct mutex *m) { assert(m->held);m->held=false; }
static void lockdep_assert_held(struct mutex *m) { assert(m->held); }
static bool boundary(void) {
 assert(mt6797_a72_owner_lock.held && p.operation_lock.held);
 return ++step==fail_step;
}
static int regulator_is_enabled(void *s) { assert(s==p.vproc_big);return boundary()?-EIO:enabled; }
static int reset_control_status(void *s) { assert(s==p.pwrap_reset);return boundary()?-EIO:reset_status; }
static int mt6797_a72_secure_read(u32 a,u32 *v) { assert(a==MT6797_A72_IDVFS_CONTROL_REG);*v=idvfs;return boundary()?-EIO:0; }
static int regmap_read(void *s,u32 a,u32 *v) {
 assert(s==p.spm);
 if(a==0x290)*v=isolation;else {assert(a==0x188);*v=power;}
 return boundary()?-EIO:0;
}
static int regulator_get_voltage(void *s) { assert(s==p.vproc_big);return boundary()?-EIO:vproc; }
static int mt6797_sram_read_boot_sample(void *d,void *s,struct mt6797_sram_sample *v) {
 assert(d==p.dev && s==p.sram_big);*v=sample;return boundary()?-EIO:0;
}
static int regulator_set_voltage(void *s,int low,int high) {
 assert(s==p.vproc_big && low==high);bool bad=boundary();writes++;
 if(!bad || effect)vproc=low;return bad?-EIO:0;
}
bool mt6797_a72_power_owns_dvfs(struct device *consumer);
static int mt6797_sram_set_running_voltage(void *d,void *s,unsigned uv,struct mt6797_sram_sample *v) {
 assert(d==p.dev && s==p.sram_big && mt6797_a72_power_owns_dvfs(d));bool bad=boundary();writes++;
 if(!bad || effect)sample.control=(sample.control & ~15U) | ((uv-900000)/25000+3);
 if(!bad)*v=sample;return bad?-EIO:0;
}
static int mt6797_cpuclk_set_mp2_rate(void *d,void *c,unsigned long rate,unsigned div) {
 assert(d==p.dev && c==p.mp2_clk && rate && (div==1 || div==2));clock_calls++;
 return boundary()?-EIO:0;
}
static int mt6797_cci_run_a72(void *d,unsigned long rate,int (*change)(void *),void *ctx) {
 assert(d==p.dev && rate && mt6797_a72_power_owns_dvfs(d));
 assert(mt6797_a72_owner_lock.held && p.operation_lock.held);return change(ctx);
}
static void usleep_range(unsigned low,unsigned high) { assert(high==low+50 && low>=25);waits++; }
'''
tests = r'''
static void setup(bool down) {
 memset(&p,0,sizeof(p));mt6797_a72_owner=&p;mt6797_a72_owner_lock.held=false;
 p.dev=&p;p.vproc_big=&vproc;p.sram_big=&sample;p.pwrap_reset=&reset_status;p.spm=&power;p.mp2_clk=&clock_calls;
 p.boot_vproc_uv=1000000;
 p.started=p.committed[0]=p.vproc_vote_owned=true;
 sample=(struct mt6797_sram_sample){down?0x4008ff:0x4008fb,0x8967};p.sram_boot=sample;
 memset(online,0,sizeof(online));online[4]=online[8]=true;cpus_held=true;
 effect=false;step=fail_step=writes=clock_calls=waits=0;enabled=1;reset_status=0;
 vproc=down?1200000:1000000;idvfs=isolation=0;power=BIT(7)|BIT(17);
}
static void clean(void) { assert(!p.operation_lock.held && !mt6797_a72_owner_lock.held);assert(!mt6797_a72_power_owns_dvfs(p.dev)); }
static int change(bool down) { return mt6797_a72_power_set_opp(8,down?338000000:1495000000,down?2:1,down?800000:1100000); }
int main(void) {
 setup(false);
 assert(mt6797_a72_power_set_opp(8,1495000000,1,1100001)==-EINVAL);
 assert(mt6797_a72_power_set_opp(8,1495000001,1,1100000)==-EINVAL);
 assert(mt6797_a72_power_set_opp(8,1495000000,0,1100000)==-EINVAL);
 assert(!step);clean();
 setup(false);cpus_held=false;assert(!change(false));clean();
 mt6797_a72_dvfs_consumer=p.dev;mt6797_a72_dvfs_task=current;
 assert(mt6797_a72_power_owns_dvfs(p.dev));
 assert(!mt6797_a72_power_owns_dvfs(NULL));
 test_current=&other_task;assert(!mt6797_a72_power_owns_dvfs(p.dev));
 test_current=&task;mt6797_a72_dvfs_task=NULL;mt6797_a72_dvfs_consumer=NULL;

 for(int down=0;down<2;down++) {
  setup(down);assert(!change(down));clean();int count=step;
  assert(vproc==(down?800000:1100000) && clock_calls==1 && waits==writes);
  assert(sample.control==p.sram_boot.control && p.dvfs_used);
  for(int n=1;n<=count;n++) for(int applied=0;applied<2;applied++) {
   setup(down);fail_step=n;effect=applied;int r=change(down);assert(r<0);clean();
   assert(p.faulted==!!(writes || clock_calls));
   if(p.faulted) {int before=step;assert(r==-EUCLEAN && change(down)==-EUCLEAN);assert(step==before);clean();}
  }
  printf("PASS owner DVFS %s: %d hardware failure boundaries, both write-effect models\n",down?"down":"up",count);
 }
 for(int n=0;n<25;n++) {
  setup(false);
  switch(n) {
  case 0:mt6797_a72_owner=NULL;break;case 1:p.faulted=true;break;
  case 2:online[4]=false;break;case 3:online[8]=false;break;case 4:p.started=false;break;
  case 5:p.committed[0]=false;break;case 6:online[9]=true;break;
  case 7:p.off_armed[0]=true;break;case 8:p.off_armed[1]=true;break;
  case 9:p.cpu8_armed=true;break;case 10:p.guarded=true;break;
  case 11:p.transaction.active=true;break;case 12:p.vproc_vote_owned=false;break;
  case 13:enabled=0;break;case 14:reset_status=1;break;case 15:idvfs=1;break;
  case 16:isolation=2;break;case 17:power=0;break;case 18:sample.control^=1;break;
  case 19:sample.calibration^=1;break;case 20:sample.control=p.sram_boot.control=0x4008f6;break;
  case 21:p.boot_vproc_uv=790000;break;case 22:p.boot_vproc_uv=1110000;break;
  case 23:p.boot_vproc_uv=1000001;break;case 24:p.cpu9_starting=true;break;
  }
  assert(change(false)<0);assert(!writes && !clock_calls);clean();
 }
 setup(false);assert(mt6797_a72_power_set_opp(4,1495000000,1,1100000)==-EINVAL);assert(!step);clean();
 setup(false);assert(mt6797_a72_power_set_opp(9,1495000000,1,1100000)==-EBUSY);assert(!step);clean();
 setup(false);online[9]=p.committed[1]=true;power|=BIT(6);
 assert(!mt6797_a72_power_set_opp(9,1495000000,1,1100000));clean();
 puts("PASS owner DVFS lock order, admission, full SRAM state, output update and sticky faults");
}
'''
with tempfile.TemporaryDirectory(prefix='a72-dvfs-api-') as temp:
    path = Path(temp)
    (path/'test.c').write_text(prefix+core+stubs+api+tests)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-sign-compare','-Wno-misleading-indentation','-fsanitize=undefined',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
